// SPDX-License-Identifier: GPL-2.0
// main.cpp - MoeKoeMusic 任务栏歌词插件入口
//
#include "config.h"
#include "config_dialog.h"
#include "constants.h"
#include "http_server.h"
#include "logger.h"
#include "settings_window.h"
#include "d2d_settings_window.h"
#include "lyrics_data.h"
#include "lyrics_parser.h"
#include "native_messaging.h"
#include "process_monitor.h"
#include "renderer.h"
#include "taskbar_window.h"
#include "tray_icon.h"
#include "websocket_client.h"
#include "api_enabler.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

namespace {

// 日志快捷方式（使用统一日志系统）
using moekoe::Log;

// 全局应用上下文（成员按析构顺序排列：后声明的先析构）
// 依赖关系：taskbarWindow → renderer → config
struct AppContext {
    HINSTANCE                hInstance{nullptr};
    HWND                     hwnd{nullptr};
    bool                     running{true};
    int                      fullscreenDebounceCnt{0};
    bool                     lastFullscreenState{false};

    // RAII 管理的组件（按依赖顺序声明：先声明后析构）
    std::unique_ptr<moekoe::NativeMessagingHost> nativeHost;
    std::unique_ptr<moekoe::D2DSettingsWindow>   d2dSettingsWindow;
    std::unique_ptr<moekoe::SettingsWindow>       settingsWindow;
    std::unique_ptr<moekoe::HttpServer>           httpServer;
    std::unique_ptr<moekoe::ProcessMonitor>       processMonitor;
    std::unique_ptr<moekoe::WebSocketClient>      wsClient;
    std::unique_ptr<moekoe::LyricsParser>         parser;
    std::unique_ptr<moekoe::TaskbarRenderer>      renderer;        // 依赖 taskbarWindow 的 HWND
    std::unique_ptr<moekoe::TaskbarWindow>         taskbarWindow;   // 依赖 renderer
    std::unique_ptr<moekoe::TrayIcon>             tray;
    std::unique_ptr<moekoe::Config>               config;          // 最后声明，最先析构
};

using namespace moekoe::constants;

// 工具: 把 UTF-8 字符串限制到 Windows Tooltip 长度
std::wstring ToTooltipWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    if (len > WINDOWS_TOOLTIP_MAX_LEN) len = WINDOWS_TOOLTIP_MAX_LEN;
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len);
    return out;
}

// 检测前台窗口是否处于全屏状态
// 判断标准：窗口矩形覆盖所在显示器（>= 显示器尺寸），且无标题栏
bool IsForegroundFullscreen(bool debugLog) {
    HWND fgw = ::GetForegroundWindow();
    if (!fgw) return false;

    // 排除桌面类窗口（Progman / WorkerW）
    wchar_t cls[64] = {};
    ::GetClassNameW(fgw, cls, 63);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0) {
        return false;
    }

    // 排除本进程自己的窗口
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fgw, &pid);
    if (pid == ::GetCurrentProcessId()) return false;

    // 获取窗口矩形
    RECT wr{};
    if (!::GetWindowRect(fgw, &wr)) return false;

    // 获取所在显示器矩形
    HMONITOR mon = ::MonitorFromWindow(fgw, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return false;

    const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int winW = wr.right - wr.left;
    const int winH = wr.bottom - wr.top;

    // 【调试】每秒输出一次检测详情，定位检测失效原因
    static int s_debugCounter = 0;
    if (debugLog && ++s_debugCounter >= 60) {
        s_debugCounter = 0;
        LONG style = ::GetWindowLongW(fgw, GWL_STYLE);
        LONG exStyle = ::GetWindowLongW(fgw, GWL_EXSTYLE);
        // 将 wchar_t 类名转为 UTF-8，适配项目 Log() 系统
        char clsUtf8[128] = {};
        ::WideCharToMultiByte(CP_UTF8, 0, cls, -1,
                              clsUtf8, (int)sizeof(clsUtf8), nullptr, nullptr);
        Log("[FullscreenDetect] cls=%s pid=%lu "
            "win=(%d,%d,%d,%d) mon=(%d,%d,%d,%d) "
            "style=0x%08lX exStyle=0x%08lX => %s\n",
            clsUtf8, pid,
            wr.left, wr.top, wr.right, wr.bottom,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right, mi.rcMonitor.bottom,
            style, exStyle,
            (winW >= monW && winH >= monH) ? "FULLSCREEN" : "normal");
    }

    // 窗口覆盖整个显示器 → 判定全屏
    if (winW >= monW && winH >= monH) return true;

    // 补充：无标题栏（WS_CAPTION）的窗口，即使尺寸略小于显示器也视为全屏
    // （部分全屏应用使用 borderless 窗口，rect 可能稍小于显示器）
    LONG style = ::GetWindowLongW(fgw, GWL_STYLE);
    constexpr LONG kFullscreenExcludeStyles = WS_CAPTION | WS_THICKFRAME;
    if (!(style & kFullscreenExcludeStyles) && winW >= monW - 8 && winH >= monH - 8) {
        return true;
    }

    return false;
}

// 应用渲染器配置
void ApplyRendererSettings(AppContext& app) {
    if (!app.renderer || !app.config) return;
    moekoe::RendererSettings rs;
    rs.highlightColor    = app.config->Appearance().highlightColor;
    rs.normalColor       = app.config->Appearance().normalColor;
    rs.normalOpacity     = static_cast<float>(app.config->Appearance().normalOpacity);
    rs.fontFamily        = app.config->Appearance().fontFamily;
    rs.fontSize          = app.config->Appearance().fontSize;
    rs.enableKaraoke     = app.config->Appearance().enableKaraoke;
    rs.enableTranslation = app.config->Appearance().enableTranslation;
    rs.enableMarquee     = app.config->Appearance().enableMarquee;
    rs.marqueeMode       = app.config->Appearance().marqueeMode;
    rs.marqueeDelayMs    = app.config->Appearance().marqueeDelayMs;
    rs.marqueePauseMs    = app.config->Appearance().marqueePauseMs;
    rs.marqueeSpeedPxPerSec = app.config->Appearance().marqueeSpeedPxPerSec;

    // 卡片模式配置
    rs.displayMode           = app.config->Appearance().displayMode;
    rs.cardCurrentFontSize   = static_cast<float>(app.config->Appearance().cardFontSizeCurrent);
    rs.cardNextFontSize      = static_cast<float>(app.config->Appearance().cardFontSizeNext);
    rs.cardCurrentColor      = app.config->Appearance().cardCurrentColor;
    rs.cardNextColor         = app.config->Appearance().cardNextColor;

    Log("[CONFIG] ApplyRenderer: hl=%s nl=%s font=%s size=%d opacity=%.2f\n",
        rs.highlightColor.c_str(), rs.normalColor.c_str(),
        rs.fontFamily.c_str(), rs.fontSize, rs.normalOpacity);

    app.renderer->ApplySettings(rs);
}

// 菜单命令处理
void OnTrayCommand(AppContext& app, UINT menuId) {
    using namespace moekoe;
    switch (menuId) {
    case ID_MENU_AUTOSTART: {
        const bool newState = !app.config->IsAutoStart();
        const bool regOk = app.config->SetAutoStart(newState);
        app.config->Save();
        if (app.tray) {
            app.tray->SetMenuCheckedAutoStart(newState);
        }
        // 用 MessageBox 直接弹模态对话框反馈结果（气泡通知在 Win10/11 常被禁用）
        const std::wstring title = L"开机自启";
        std::wstring msg;
        if (regOk) {
            if (newState) {
                msg = L"已启用开机自启。\n\n"
                      L"程序会尝试以下三种方式（按顺序，自动跳过失败的）：\n"
                      L"1) 注册表 Run 键（可能被杀毒软件拦截）\n"
                      L"2) 任务计划程序（推荐）\n"
                      L"3) 启动文件夹快捷方式\n\n"
                      L"查看实际生效方式：\n"
                      L"注册表: reg query HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\n"
                      L"任务计划: schtasks /Query /TN MoeKoeTaskbarLyrics_AutoStart\n"
                      L"启动文件夹: %APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
            } else {
                msg = L"已禁用开机自启，所有方式均已清理。";
            }
        } else {
            msg = L"开机自启设置失败！\n\n"
                  L"三种方式都未生效：\n"
                  L"1. 注册表 Run 键（最可能被拦截）\n"
                  L"2. 任务计划程序 schtasks\n"
                  L"3. 启动文件夹 PowerShell 快捷方式\n\n"
                  L"请查看 debug.log 获取详细错误。";
        }
        ::MessageBoxW(app.hwnd, msg.c_str(), title.c_str(),
            regOk ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
        // 同步设置窗口 UI（如果已打开）
        if (app.settingsWindow && app.settingsWindow->IsWebViewReady()) {
            app.settingsWindow->SendConfigToWebView(*app.config);
        }
        break;
    }
    case ID_MENU_RECONNECT: {
        if (app.wsClient) app.wsClient->RequestReconnect();
        break;
    }
    case ID_MENU_SETTINGS: {
        // 根据上次保存的 UI 模式选择打开哪种设置界面
        const std::string uiMode = app.config->Advanced().settingsUiMode;
        const bool useD2D = (uiMode == "d2d");

        if (useD2D) {
            // ═══════ D2D 原生自绘设置界面 ═══════
            if (!app.d2dSettingsWindow) {
                app.d2dSettingsWindow = std::make_unique<moekoe::D2DSettingsWindow>();
                app.d2dSettingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                    const auto savedPos = app.config->Position();
                    *app.config = cfg;
                    app.config->MutablePosition() = savedPos;
                    ApplyRendererSettings(app);
                    if (app.taskbarWindow) {
                        app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                        app.taskbarWindow->Reposition();
                    }
                    if (app.tray) {
                        app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                    }
                    Log("[SETTINGS] D2D config applied and saved\n");
                });
                // 切换到 WebView2 模式的回调
                app.d2dSettingsWindow->OnSwitchMode([&](const std::string& newMode) {
                    app.config->MutableAdvanced().settingsUiMode = newMode;
                    app.config->Save();
                    Log("[SETTINGS] Switching mode to: %s\n", newMode.c_str());
                    // 关闭 D2D 窗口后立即打开 WebView2 设置
                    if (newMode == "webview") {
                        app.d2dSettingsWindow->Close();
                        app.d2dSettingsWindow.reset();
                        app.d2dSettingsWindow = nullptr;
                        if (!app.settingsWindow) {
                            app.settingsWindow = std::make_unique<moekoe::SettingsWindow>();
                            app.settingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                                const auto savedPos = app.config->Position();
                                *app.config = cfg;
                                app.config->MutablePosition() = savedPos;
                                ApplyRendererSettings(app);
                                if (app.taskbarWindow) {
                                    app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                                    app.taskbarWindow->Reposition();
                                }
                                if (app.tray) {
                                    app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                                }
                                Log("[SETTINGS] WebView2 config applied and saved\n");
                            });
                            app.settingsWindow->OnSwitchMode([&](const std::string& m) {
                                Log("[SETTINGS] WebView2 (from D2D) switching mode to: %s\n", m.c_str());
                                if (m == "d2d") {
                                    app.settingsWindow->Close();
                                    app.settingsWindow.reset();
                                    app.settingsWindow = nullptr;
                                    if (!app.d2dSettingsWindow) {
                                        app.d2dSettingsWindow = std::make_unique<moekoe::D2DSettingsWindow>();
                                        app.d2dSettingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                                            const auto savedPos = app.config->Position();
                                            *app.config = cfg;
                                            app.config->MutablePosition() = savedPos;
                                            ApplyRendererSettings(app);
                                            if (app.taskbarWindow) {
                                                app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                                                app.taskbarWindow->Reposition();
                                            }
                                            if (app.tray) {
                                                app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                                            }
                                            Log("[SETTINGS] D2D config applied and saved\n");
                                        });
                                        app.d2dSettingsWindow->OnSwitchMode([&](const std::string& sm) {
                                            app.config->MutableAdvanced().settingsUiMode = sm;
                                            app.config->Save();
                                            Log("[SETTINGS] D2D (from WebView2) switching mode to: %s\n", sm.c_str());
                                            if (sm == "webview") {
                                                app.d2dSettingsWindow->Close();
                                                app.d2dSettingsWindow.reset();
                                                app.d2dSettingsWindow = nullptr;
                                                if (!app.settingsWindow) {
                                                    app.settingsWindow = std::make_unique<moekoe::SettingsWindow>();
                                                    app.settingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                                                        const auto savedPos = app.config->Position();
                                                        *app.config = cfg;
                                                        app.config->MutablePosition() = savedPos;
                                                        ApplyRendererSettings(app);
                                                        if (app.taskbarWindow) {
                                                            app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                                                            app.taskbarWindow->Reposition();
                                                        }
                                                        if (app.tray) {
                                                            app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                                                        }
                                                        Log("[SETTINGS] WebView2 config applied and saved\n");
                                                    });
                                                    app.settingsWindow->OnSwitchMode([&](const std::string& sm2) {
                                                        app.config->MutableAdvanced().settingsUiMode = sm2;
                                                        app.config->Save();
                                                        Log("[SETTINGS] WebView2 switching mode to: %s\n", sm2.c_str());
                                                        if (sm2 == "d2d") {
                                                            app.settingsWindow->Close();
                                                            app.settingsWindow.reset();
                                                            app.settingsWindow = nullptr;
                                                            if (!app.d2dSettingsWindow) {
                                                                app.d2dSettingsWindow = std::make_unique<moekoe::D2DSettingsWindow>();
                                                                app.d2dSettingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                                                                    const auto savedPos = app.config->Position();
                                                                    *app.config = cfg;
                                                                    app.config->MutablePosition() = savedPos;
                                                                    ApplyRendererSettings(app);
                                                                    if (app.taskbarWindow) {
                                                                        app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                                                                        app.taskbarWindow->Reposition();
                                                                    }
                                                                    if (app.tray) {
                                                                        app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                                                                    }
                                                                    Log("[SETTINGS] D2D config applied and saved\n");
                                                                });
                                                                app.d2dSettingsWindow->OnSwitchMode([&](const std::string& sm3) {
                                                                    app.config->MutableAdvanced().settingsUiMode = sm3;
                                                                    app.config->Save();
                                                                });
                                                            }
                                                            app.d2dSettingsWindow->Show(app.hInstance, app.hwnd, *app.config);
                                                        }
                                                    });
                                                }
                                                app.settingsWindow->Show(app.hInstance, app.hwnd, *app.config);
                                            }
                                        });
                                    }
                                    app.d2dSettingsWindow->Show(app.hInstance, app.hwnd, *app.config);
                                }
                            });
                        }
                        app.settingsWindow->Show(app.hInstance, app.hwnd, *app.config);
                    }
                });
            }

            if (app.d2dSettingsWindow->Show(app.hInstance, app.hwnd, *app.config)) {
                break; // D2D 窗口创建成功
            } else {
                app.d2dSettingsWindow.reset();
                app.d2dSettingsWindow = nullptr;
                // D2D 失败，回退到 WebView2
            }
        }

        // ═══════ WebView2 设置界面（默认 / 回退） ═══════
        if (!app.settingsWindow) {
            app.settingsWindow = std::make_unique<moekoe::SettingsWindow>();
            app.settingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                const auto savedPos = app.config->Position();
                *app.config = cfg;
                app.config->MutablePosition() = savedPos;
                ApplyRendererSettings(app);
                if (app.taskbarWindow) {
                    app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                    app.taskbarWindow->Reposition();
                }
                if (app.tray) {
                    app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                }
                Log("[SETTINGS] WebView2 config applied and saved\n");
            });
            // WebView2 → D2D 切换回调
            app.settingsWindow->OnSwitchMode([&](const std::string& newMode) {
                Log("[SETTINGS] WebView2 switching mode to: %s\n", newMode.c_str());
                // 关闭 WebView2 窗口后立即打开 D2D 原生设置
                if (newMode == "d2d") {
                    app.settingsWindow->Close();
                    app.settingsWindow.reset();
                    app.settingsWindow = nullptr;
                    // 创建并显示 D2D 设置窗口
                    if (!app.d2dSettingsWindow) {
                        app.d2dSettingsWindow = std::make_unique<moekoe::D2DSettingsWindow>();
                        app.d2dSettingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                            const auto savedPos = app.config->Position();
                            *app.config = cfg;
                            app.config->MutablePosition() = savedPos;
                            ApplyRendererSettings(app);
                            if (app.taskbarWindow) {
                                app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                                app.taskbarWindow->Reposition();
                            }
                            if (app.tray) {
                                app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                            }
                            Log("[SETTINGS] D2D config applied and saved\n");
                        });
                        app.d2dSettingsWindow->OnSwitchMode([&](const std::string& m) {
                            app.config->MutableAdvanced().settingsUiMode = m;
                            app.config->Save();
                            Log("[SETTINGS] D2D (from WebView2) switching mode to: %s\n", m.c_str());
                            if (m == "webview") {
                                app.d2dSettingsWindow->Close();
                                app.d2dSettingsWindow.reset();
                                app.d2dSettingsWindow = nullptr;
                                if (!app.settingsWindow) {
                                    app.settingsWindow = std::make_unique<moekoe::SettingsWindow>();
                                    app.settingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                                        const auto savedPos = app.config->Position();
                                        *app.config = cfg;
                                        app.config->MutablePosition() = savedPos;
                                        ApplyRendererSettings(app);
                                        if (app.taskbarWindow) {
                                            app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                                            app.taskbarWindow->Reposition();
                                        }
                                        if (app.tray) {
                                            app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                                        }
                                        Log("[SETTINGS] WebView2 config applied and saved\n");
                                    });
                                    app.settingsWindow->OnSwitchMode([&](const std::string& sm) {
                                        app.config->MutableAdvanced().settingsUiMode = sm;
                                        app.config->Save();
                                        Log("[SETTINGS] WebView2 (from D2D) switching mode to: %s\n", sm.c_str());
                                        if (sm == "d2d") {
                                            app.settingsWindow->Close();
                                            app.settingsWindow.reset();
                                            app.settingsWindow = nullptr;
                                            if (!app.d2dSettingsWindow) {
                                                app.d2dSettingsWindow = std::make_unique<moekoe::D2DSettingsWindow>();
                                                app.d2dSettingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                                                    const auto savedPos = app.config->Position();
                                                    *app.config = cfg;
                                                    app.config->MutablePosition() = savedPos;
                                                    ApplyRendererSettings(app);
                                                    if (app.taskbarWindow) {
                                                        app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                                                        app.taskbarWindow->Reposition();
                                                    }
                                                    if (app.tray) {
                                                        app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                                                    }
                                                    Log("[SETTINGS] D2D config applied and saved\n");
                                                });
                                                app.d2dSettingsWindow->OnSwitchMode([&](const std::string& sm2) {
                                                    app.config->MutableAdvanced().settingsUiMode = sm2;
                                                    app.config->Save();
                                                });
                                            }
                                            app.d2dSettingsWindow->Show(app.hInstance, app.hwnd, *app.config);
                                        }
                                    });
                                }
                                app.settingsWindow->Show(app.hInstance, app.hwnd, *app.config);
                            }
                        });
                    }
                    app.d2dSettingsWindow->Show(app.hInstance, app.hwnd, *app.config);
                }
            });
        }

        if (app.settingsWindow->Show(app.hInstance, app.hwnd, *app.config)) {
            break; // WebView2 窗口创建成功
        } else {
            // WebView2 也失败，回退到 Win32 对话框
            app.settingsWindow.reset();
            app.settingsWindow = nullptr;
        }

        // ═══════ 最终回退：Win32 原生对话框 ═══════
        if (moekoe::ConfigDialog::Show(app.hInstance, app.hwnd, *app.config,
                                       /*boundMode*/ false,
                                       [&app]() {
                                           app.running = false;
                                           ::PostQuitMessage(0);
                                       })) {
            ApplyRendererSettings(app);
            if (app.taskbarWindow) {
                app.taskbarWindow->SetDragOffset(
                    app.config->Position().offsetX, app.config->Position().offsetY);
                app.taskbarWindow->Reposition();
            }
        }
        break;
    }
    case ID_MENU_UNBIND: {
        // 保留兼容：退出程序
        int ret = ::MessageBoxW(app.hwnd,
            L"确定要退出吗？",
            L"退出", MB_YESNO | MB_ICONQUESTION);
        if (ret == IDYES) {
            app.running = false;
            ::PostQuitMessage(0);
        }
        break;
    }
    case ID_MENU_LOCK_POS: {
        const bool newState = !app.config->Position().lockPosition;
        app.config->MutablePosition().lockPosition = newState;
        // 完全锁定隐含位置锁定，取消位置锁定时不影响完全锁定
        if (newState) {
            app.config->MutablePosition().lockFully = false;
        }
        app.config->Save();
        if (app.taskbarWindow) {
            app.taskbarWindow->SetPositionLocked(newState);
            app.taskbarWindow->SetFullyLocked(false);
        }
        if (app.tray) {
            app.tray->SetMenuCheckedLockPos(newState);
            app.tray->SetMenuCheckedLockFull(false);
        }
        break;
    }
    case ID_MENU_LOCK_FULL: {
        const bool newState = !app.config->Position().lockFully;
        app.config->MutablePosition().lockFully = newState;
        // 完全锁定隐含位置锁定
        if (newState) {
            app.config->MutablePosition().lockPosition = true;
        }
        app.config->Save();
        if (app.taskbarWindow) {
            app.taskbarWindow->SetFullyLocked(newState);
            app.taskbarWindow->SetPositionLocked(newState);
        }
        if (app.tray) {
            app.tray->SetMenuCheckedLockFull(newState);
            app.tray->SetMenuCheckedLockPos(newState);
        }
        break;
    }
    case ID_MENU_EXIT: {
        app.running = false;
        ::PostQuitMessage(0);
        break;
    }
    default:
        break;
    }
}

// 隐式消息窗口过程(用于接收托盘消息 + WM_FRAME_TICK)
LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppContext* app = reinterpret_cast<AppContext*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CLOSE:
        app->running = false;
        ::PostQuitMessage(0);
        return 0;

    case WM_TRAY_CALLBACK: { // 托盘回调
        if (app->tray) app->tray->OnTrayMessage(hwnd, wParam, lParam);
        return 0;
    }

    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        OnTrayCommand(*app, id);
        return 0;
    }

    case moekoe::TaskbarWindow::WM_FRAME_TICK:
    case WM_TIMER: {
        try {
            // ═══════ 帧渲染流程 ═══════
            // 1. 检测任务栏尺寸变化（含 APPBAR 自动隐藏鼠标进出检测）
            if (app->taskbarWindow) app->taskbarWindow->CheckResize();

            // 2. APPBAR 自动隐藏：窗口已隐藏时跳过渲染（避免 UpdateLayeredWindow 隐式显示导致闪烁）
            if (app->taskbarWindow && app->taskbarWindow->IsAutoHideHidden()) return 0;

            // 2.5. 全屏检测防抖：连续 N 帧为同一状态后才切换显隐
            if (app->taskbarWindow && app->config && app->config->Advanced().enableFullscreenHide) {
                static bool s_blockEntered = false;
                if (!s_blockEntered) {
                    s_blockEntered = true;
                    if (app->config->Advanced().debugLog) Log("[FullscreenDetect] block entered, enableFullscreenHide=true\n");
                }
                const bool isFullscreen = IsForegroundFullscreen(app->config->Advanced().debugLog);
                if (isFullscreen == app->lastFullscreenState) {
                    if (app->fullscreenDebounceCnt < 99) app->fullscreenDebounceCnt++;
                } else {
                    // 状态翻转：输出日志定位频繁切换原因
                    if (app->config->Advanced().debugLog) Log("[FullscreenDetect] state flip: %d -> %d (debounce reset)\n",
                        (int)app->lastFullscreenState, (int)isFullscreen);
                    app->fullscreenDebounceCnt = 0;
                    app->lastFullscreenState = isFullscreen;
                }

                // 防抖阈值：~120ms（8 帧 × 15ms MIN_FRAME_INTERVAL）
                constexpr int kDebounceThreshold = 8;
                if (app->fullscreenDebounceCnt == kDebounceThreshold) {
                    if (app->config->Advanced().debugLog) Log("[FullscreenDetect] debounce reached -> SetFullscreenHidden(%d)\n",
                        (int)isFullscreen);
                    app->taskbarWindow->SetFullscreenHidden(isFullscreen);
                }

                // Shell 交互（如 Win 键呼出开始菜单）在 ShellMenuWinEventProc 中
                // 直接调用 SetFullscreenHidden(false) 并设置 s_forceDebounceReset_ 标志。
                // 此处消费该标志，将防抖计数器清零且将基线设为"非全屏"，避免下一帧
                // 全屏检测读到 isFullscreen==true 后立即触发状态翻转重新计数隐藏。
                if (moekoe::TaskbarWindow::s_forceDebounceReset_) {
                    moekoe::TaskbarWindow::s_forceDebounceReset_ = false;
                    app->fullscreenDebounceCnt = 0;
                    app->lastFullscreenState = false;
                    if (app->config->Advanced().debugLog) Log("[FullscreenDetect] debounce reset by external trigger (shell interaction)\n");
                }
            }

            // 2.6. 全屏隐藏时跳过渲染（与 APPBAR 同理）
            if (app->taskbarWindow && app->taskbarWindow->IsFullscreenHidden()) return 0;

            // 3. 从歌词解析器获取当前应渲染的状态
            if (app->parser && app->renderer) {
                auto state = app->parser->GetCurrentRenderState();

                // 3. 附加 UI 状态（悬停/拖动，用于判断是否显示控制按钮）
                if (app->taskbarWindow) {
                    state.isHovering = app->taskbarWindow->IsHovering();
                    state.isDragging = app->taskbarWindow->IsDragging();
                }

                // 4. 同步任务栏方向（运行时位置变化适配）+ 执行渲染
                app->renderer->SetVerticalTaskbar(app->taskbarWindow->IsVerticalTaskbar());
                app->renderer->Render(state);

                // 5. 更新托盘提示文本（实时显示当前歌词）
                if (app->tray && state.hasLyrics) {
                    auto tip = ToTooltipWide(state.currentLine);
                    if (!tip.empty()) {
                        app->tray->SetTooltip(tip);
                    }
                }
            }
        } catch (const std::exception& e) {
            Log("[CRASH] WM_TIMER exception: %s\n", e.what());
            // 恢复策略：尝试重置渲染器状态
            if (app->renderer) {
                try {
                    app->renderer->Shutdown();
                    if (app->taskbarWindow)
                        app->renderer->Initialize(app->taskbarWindow->GetHandle());
                    Log("[RECOVER] Renderer reset succeeded\n");
                } catch (...) {
                    Log("[FATAL] Renderer recovery failed, shutting down\n");
                    app->running = false;
                    ::PostQuitMessage(0);
                }
            }
        } catch (...) {
            Log("[CRASH] WM_TIMER unknown exception, shutting down\n");
            app->running = false;
            ::PostQuitMessage(0);
        }
        return 0;
    }

    case WM_RENDER_UPDATE: {
        try {
            // APPBAR 自动隐藏时跳过悬停重绘
            if (app->taskbarWindow && app->taskbarWindow->IsAutoHideHidden()) return 0;

            // 全屏隐藏时跳过悬停重绘
            if (app->taskbarWindow && app->taskbarWindow->IsFullscreenHidden()) return 0;

            if (app->parser && app->renderer && app->taskbarWindow) {
                auto state = app->parser->GetCurrentRenderState();
                state.isHovering = app->taskbarWindow->IsHovering();
                state.isDragging = app->taskbarWindow->IsDragging();
                app->renderer->Render(state);
            }
        } catch (...) {}
        return 0;
    }

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 注册一个不显示的 message-only 类
bool RegisterMessageClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &MsgWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"MoeKoeTaskbarLyricsMsg";
    return ::RegisterClassExW(&wc) != 0;
}

} // namespace

// 全局异常过滤器
static LONG WINAPI GlobalExceptionHandler(EXCEPTION_POINTERS* ep) {
    Log("[CRASH] Unhandled exception code=0x%08lX at address=%p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR /*cmdLine*/, int /*nShow*/) {
    // ═══════ 第 1 阶段：系统初始化 ═══════
    // 目的：为应用提供运行时基础（COM、异常处理、日志、单实例保护）
    moekoe::InitLogger();
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC/Direct2D 需要 COM
    ::SetUnhandledExceptionFilter(GlobalExceptionHandler);

    // 单实例保护：避免多个进程竞争任务栏窗口导致闪烁/消息丢失
    ::CreateMutexW(nullptr, FALSE, L"MoeKoeTaskbarLyrics_Mutex");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    Log("[STARTUP] WinMain entered\n");

    // Winsock 初始化（ixwebsocket 依赖）
    WSADATA wsaData;
    int wsRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    Log("[STARTUP] WSAStartup ret=%d\n", wsRet);

    // DPI 感知：Per-Monitor V2，支持多显示器不同缩放
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ═══════ 第 2 阶段：应用初始化 ═══════
    // 目的：加载配置、创建消息窗口和托盘图标

    if (!RegisterMessageClass(hInstance)) {
        std::fprintf(stderr, "[Error] RegisterClassExW failed\n");
        return 1;
    }

    HWND hMsgWnd = ::CreateWindowExW(
        0, L"MoeKoeTaskbarLyricsMsg", L"",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!hMsgWnd) {
        std::fprintf(stderr, "[Error] Create message window failed\n");
        return 1;
    }

    AppContext app;
    app.hwnd   = hMsgWnd;
    app.hInstance = hInstance;
    ::SetWindowLongPtrW(hMsgWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    app.config = std::make_unique<moekoe::Config>();
    auto& config = *app.config;
    config.Load();
    moekoe::SetLogEnabled(config.Advanced().debugLog);
    Log("[STARTUP] Config loaded\n");
    config.SetAutoStart(config.IsAutoStart());
    Log("[STARTUP] AutoStart=%s\n", config.IsAutoStart() ? "ON" : "OFF");

    // ═══════ 第 3 阶段：业务模块初始化 ═══════
    // 目的：创建核心业务逻辑模块（任务栏窗口、渲染引擎、WebSocket、HTTP服务器）
    // 创建系统托盘
    app.tray = std::make_unique<moekoe::TrayIcon>();
    auto& tray = *app.tray;
    tray.Initialize(hInstance, hMsgWnd);
    tray.SetMenuCheckedAutoStart(config.IsAutoStart());

    // 7) 查找任务栏
    HWND hTaskbar = moekoe::TaskbarWindow::FindTaskbarHandle();
    Log("[STARTUP] FindTaskbar hTaskbar=%p\n", hTaskbar);
    if (!hTaskbar) {
        ::MessageBoxW(nullptr,
                      L"未找到 Windows 任务栏，请确认系统正常运行。",
                      L"MoeKoe Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }

    // 8) 创建嵌入任务栏的歌词窗口
    app.taskbarWindow = std::make_unique<moekoe::TaskbarWindow>();
    auto& taskbarWindow = *app.taskbarWindow;
    if (!taskbarWindow.Create(hInstance, hTaskbar)) {
        ::MessageBoxW(nullptr,
                      L"创建任务栏歌词窗口失败。",
                      L"MoeKoe Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }

    // 应用配置中的位置偏移
    taskbarWindow.SetDragOffset(config.Position().offsetX, config.Position().offsetY);
    taskbarWindow.Reposition();

    // 应用配置中的显示模式
    taskbarWindow.SetDisplayMode(config.Appearance().displayMode);

    // 应用配置中的锁定状态
    taskbarWindow.SetPositionLocked(config.Position().lockPosition);
    taskbarWindow.SetFullyLocked(config.Position().lockFully);
    tray.SetMenuCheckedLockPos(config.Position().lockPosition);
    tray.SetMenuCheckedLockFull(config.Position().lockFully);

    // 拖动结束时保存位置偏移到配置
    taskbarWindow.OnHoverChanged([&]() {
        if (app.taskbarWindow) {
            config.MutablePosition().offsetX = app.taskbarWindow->GetDragOffsetX();
            config.MutablePosition().offsetY = app.taskbarWindow->GetDragOffsetY();
            config.Save();
        }
    });

    // 初始隐藏窗口，收到歌词数据后再显示
    ::ShowWindow(taskbarWindow.GetHandle(), SW_HIDE);

    // 9) 初始化渲染器
    app.renderer = std::make_unique<moekoe::TaskbarRenderer>();
    auto& renderer = *app.renderer;
    ApplyRendererSettings(app);
    if (!renderer.Initialize(taskbarWindow.GetHandle())) {
        ::MessageBoxW(nullptr,
                      L"Direct2D 初始化失败。",
                      L"MoeKoe Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }

    // 同步任务栏方向到渲染器（纵向屏幕/垂直任务栏适配）
    renderer.SetVerticalTaskbar(taskbarWindow.IsVerticalTaskbar());
    renderer.SetDebugLog(config.Advanced().debugLog);

    // 10) 启动 WebSocket 客户端 + 歌词解析
    app.parser = std::make_unique<moekoe::LyricsParser>();
    auto& parser = *app.parser;

    app.wsClient = std::make_unique<moekoe::WebSocketClient>();
    auto& wsClient = *app.wsClient;
    wsClient.SetDebugLog(config.Advanced().debugLog);

    wsClient.OnLyrics([&](const moekoe::LyricsData& data) {
        if (app.config->Advanced().debugLog) Log("[WS] OnLyrics: valid=%d lines=%zu\n", data.valid, data.lines.size());
        parser.UpdateLyrics(data);
        if (app.taskbarWindow && data.valid) {
            HWND h = taskbarWindow.GetHandle();
            ::ShowWindow(h, SW_SHOWNA);
            ::SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    });
    wsClient.OnPlayerState([&](const moekoe::PlayerState& st) {
        if (app.config->Advanced().debugLog) Log("[WS] OnPlayerState: playing=%d time=%.2f song='%s' cover='%s'\n",
            st.isPlaying, st.currentTime, st.songTitle.c_str(),
            st.coverArtUrl.empty() ? "(empty)" : st.coverArtUrl.substr(0, 60).c_str());
        parser.UpdatePlayerState(st);
    });
    wsClient.OnConnectionStatus([&](bool connected) {
        if (app.tray) {
            app.tray->SetTooltip(connected
                ? L"MoeKoe Taskbar Lyrics (已连接)"
                : L"MoeKoe Taskbar Lyrics (等待连接...)");
        }
    });

    // 注册按钮点击回调
    taskbarWindow.OnButtonClicked([&](moekoe::HoverButton btn) {
        if (!app.wsClient) return;
        switch (btn) {
        case moekoe::HoverButton::Prev:
            app.wsClient->SendControl("prev");
            break;
        case moekoe::HoverButton::PlayPause:
            app.wsClient->SendControl("toggle");
            break;
        case moekoe::HoverButton::Next:
            app.wsClient->SendControl("next");
            break;
        default:
            break;
        }
    });

    // 注册悬停状态变化回调
    taskbarWindow.OnHoverChanged([&]() {
        ::PostMessageW(hMsgWnd, WM_RENDER_UPDATE, 0, 0);
    });

    // 主动检测：MoeKoeMusic 在运行但 API 未开启 → 自动开启并重启
    {
        auto apiResult = moekoe::ApiEnabler::TryEnableApi();
        if (apiResult == moekoe::ApiEnableResult::EnabledAndRestarted) {
            Log("[MAIN] API auto-enabled and MoeKoeMusic restarted, waiting for new instance...\n");
            Sleep(2000); // 等待新实例完全启动
        } else {
            Log("[MAIN] API auto-enable check result: %s\n",
                moekoe::ApiEnabler::ResultToString(apiResult).c_str());
        }
    }

    char url[64];
    std::snprintf(url, sizeof(url), "ws://127.0.0.1:%d",
                  config.Advanced().websocketPort);
    wsClient.Connect(url);

    // 10.5) 启动 HTTP 服务器（用于 popup.js 通信：ping / shutdown / lyrics）
    app.httpServer = std::make_unique<moekoe::HttpServer>();
    auto& httpServer = *app.httpServer;
    httpServer.OnCommand([&](const std::string& command) {
        Log("[HTTP] Command received: %s\n", command.c_str());
        if (command == "shutdown") {
            Log("[HTTP] Shutdown via HTTP, exiting...\n");
            app.running = false;
            ::PostQuitMessage(0);
        }
    });
    // HTTP /lyrics 端点：接收外部歌词+封面数据
    httpServer.OnLyrics([&](const std::string& jsonBody) {
        try {
            nlohmann::json j = nlohmann::json::parse(jsonBody);

            // 提取并更新播放器状态（封面 URL、歌曲名等）
            moekoe::PlayerState st;
            if (j.contains("isPlaying") && j["isPlaying"].is_boolean()) {
                st.isPlaying = j["is"].get<bool>();
            }
            if (j.contains("currentTime") && j["currentTime"].is_number()) {
                st.currentTime = j["currentTime"].get<double>();
            }

            // 提取封面 URL（支持多种字段名）
            for (const auto& key : {"coverArtUrl", "pic", "cover", "albumArt", "image", "poster"}) {
                if (j.contains(key) && j[key].is_string()) {
                    st.coverArtUrl = j[key].get<std::string>();
                    break;
                }
            }
            // 从 currentSong 嵌套对象提取封面
            if (st.coverArtUrl.empty() && j.contains("currentSong") && j["currentSong"].is_object()) {
                const auto& cs = j["currentSong"];
                for (const auto& key : {"pic", "cover", "albumArt", "image", "poster"}) {
                    if (cs.contains(key) && cs[key].is_string()) {
                        st.coverArtUrl = cs[key].get<std::string>();
                        break;
                    }
                }
            }

            // 提取歌曲名称
            if (j.contains("songName") && j["songName"].is_string()) {
                st.songName = j["songName"].get<std::string>();
            } else if (j.contains("currentSong") && j["currentSong"].is_object() &&
                       j["currentSong"].contains("name")) {
                st.songName = j["currentSong"]["name"].get<std::string>();
            }

            parser.UpdatePlayerState(st);

            // 提取歌词数据（如果存在）
            if (j.contains("lyricsData") || j.contains("data")) {
                moekoe::LyricsData data;
                auto& ld = j.contains("lyricsData") ? j["lyricsData"] : j["data"];

                if (ld.is_array()) {
                    for (const auto& lineJson : ld) {
                        if (data.lines.size() >= moekoe::constants::MAX_LYRIC_LINES) break;
                        moekoe::LyricLine line;
                        line.text       = lineJson.value("text", "");
                        line.translated = lineJson.value("translated", "");

                        if (lineJson.contains("characters") && lineJson["characters"].is_array()) {
                            for (const auto& c : lineJson["characters"]) {
                                if (line.characters.size() >= moekoe::constants::MAX_CHARS_PER_LINE) break;
                                moekoe::CharacterTiming ct;
                                ct.ch        = c.value("char", "");
                                ct.startTime = c.value("startTime", static_cast<int64_t>(0));
                                ct.endTime   = c.value("endTime",   static_cast<int64_t>(0));
                                if (!ct.ch.empty()) {
                                    line.characters.push_back(std::move(ct));
                                }
                            }
                        }
                        data.lines.push_back(std::move(line));
                    }
                } else if (ld.is_string()) {
                    // KRC 字符串格式：使用公共解析方法
                    data = moekoe::WebSocketClient::ParseKrcString(ld.get<std::string>());
                }

                data.valid = !data.lines.empty();
                if (data.valid) {
                    parser.UpdateLyrics(data);
                    if (app.taskbarWindow) {
                        HWND h = taskbarWindow.GetHandle();
                        ::ShowWindow(h, SW_SHOWNA);
                        ::SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                    }
                }
            }

            Log("[HTTP] Lyrics processed: cover='%s' song='%s'\n",
                st.coverArtUrl.c_str(), st.songName.c_str());
        } catch (const std::exception& e) {
            Log("[HTTP] Failed to parse lyrics JSON: %s\n", e.what());
        } catch (...) {
            Log("[HTTP] Failed to parse lyrics JSON: unknown error\n");
        }
    });
    // HTTP 端口从 config 读取（默认 6523）；异常时退回到 constants.h 中的默认值。
    const int httpPort = (config.Advanced().httpServerPort > 0)
                             ? config.Advanced().httpServerPort
                             : static_cast<int>(moekoe::constants::HTTP_SERVER_PORT);
    if (httpServer.Start(httpPort)) {
        Log("[STARTUP] HTTP server started on port %d\n", httpPort);
    } else {
        Log("[STARTUP] HTTP server failed to start on port %d (non-fatal)\n", httpPort);
    }

    // 11) 启动 Native Host stdin 读取（MoeKoeMusic 托管模式下的生命周期管理）
    // 在后台线程中读取 stdin JSON Lines，收到 shutdown 时通知主线程退出
    app.nativeHost = std::make_unique<moekoe::NativeMessagingHost>();
    auto& nativeHost = *app.nativeHost;
    nativeHost.SetMessageHandler([&app](const moekoe::NativeHostMessage& msg) {
        Log("[NATIVE-HOST] Received message: type=%s\n", msg.type.c_str());
        // 业务消息处理：可根据 payload 中的 action 执行对应操作
        if (msg.payload.contains("action")) {
            std::string action = msg.payload["action"].get<std::string>();
            Log("[NATIVE-HOST] Action: %s\n", action.c_str());
            // 未来可扩展: set-config, get-status 等
        }
    });
    // 在独立线程中运行 stdin 循环（阻塞式 getline）
    std::thread stdinThread([&nativeHost, &app]() {
        Log("[NATIVE-HOST] Stdin reader thread started (managed=%d)\n",
            !nativeHost.IsShutdown());
        bool result = nativeHost.Run();
        if (!result) {
            // 收到 shutdown 指令或读取错误 → 通知主线程退出
            Log("[NATIVE-HOST] Run() returned false, requesting shutdown\n");
            app.running = false;
            ::PostQuitMessage(0);
        } else {
            // stdin EOF（独立运行模式，无托管者）→ 继续运行
            Log("[NATIVE-HOST] Run() returned true (standalone mode, continuing)\n");
        }
    });
    stdinThread.detach();
    Log("[STARTUP] Native Host stdin reader started\n");

    // 启动帧定时器
    const int intervalMs = std::max(MIN_FRAME_INTERVAL_MS, 1000 / std::max(1, config.Advanced().refreshRateHz));
    ::SetTimer(hMsgWnd, /*id*/1, static_cast<UINT>(intervalMs), nullptr);

    // ═══════ 第 4 阶段：业务逻辑循环 ═══════
    // 目的：处理消息和事件，直到应用关闭
    MSG msg{};
    while (app.running && ::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    Log("[SHUTDOWN] Message loop ended\n");

    // ═══════ 第 5 阶段：清理和退出 ═══════
    // 目的：释放所有资源，进行优雅关闭
    // 注意：顺序与初始化相反（后创建的先销毁）

    // 退出前保存最终位置偏移（避免拖动后未触发 hover 变化导致位置丢失）
    if (app.taskbarWindow) {
        config.MutablePosition().offsetX = app.taskbarWindow->GetDragOffsetX();
        config.MutablePosition().offsetY = app.taskbarWindow->GetDragOffsetY();
    }
    config.Save();

    ::KillTimer(hMsgWnd, 1);
    httpServer.Stop();
    wsClient.Disconnect();
    renderer.Shutdown();
    taskbarWindow.Destroy();
    tray.Shutdown();
    ::DestroyWindow(hMsgWnd);

    Log("[SHUTDOWN] Complete\n");
    return 0;
}
