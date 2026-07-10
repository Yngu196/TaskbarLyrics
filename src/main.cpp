// SPDX-License-Identifier: GPL-3.0
// main.cpp - MoeKoeMusic 任务栏歌词插件入口
//
#include "config.h"
#include "api_enabler.h"
#include "config_dialog.h"
#include "constants.h"
#include "fullscreen_detector.h"
#include "http_server.h"
#include "logger.h"

#include "d2d_settings_window.h"
#include "lyrics_data.h"
#include "lyrics_parser.h"
#include "native_messaging.h"
#include "renderer.h"
#include "spectrum_capture.h"
#include "taskbar_window.h"
#include "tray_icon.h"
#include "websocket_client.h"


#include <nlohmann/json.hpp>
#include <windows.h>
#include <psapi.h>
#include <shobjidl.h>

#pragma comment(lib, "psapi.lib")

#include <algorithm>
#include <atomic>
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

    // 动态卡片宽度缩回滞回计时器
    ULONGLONG                shrinkStartTick_{0};

    // RAII 管理的组件（按依赖顺序声明：先声明后析构）
    std::unique_ptr<moekoe::NativeMessagingHost> nativeHost;
    std::unique_ptr<moekoe::D2DSettingsWindow>   d2dSettingsWindow;
    std::unique_ptr<moekoe::HttpServer>           httpServer;
    std::unique_ptr<moekoe::WebSocketClient>      wsClient;
    std::unique_ptr<moekoe::LyricsParser>         parser;
    std::unique_ptr<moekoe::SpectrumCapture>      spectrumCapture;
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

// 应用渲染器配置（直接传递 AppearanceConfig，无需逐字段拷贝）
void ApplyRendererSettings(AppContext& app) {
    if (!app.renderer || !app.config) return;
    Log("[CONFIG] ApplyRenderer: hl=%s nl=%s font=%s size=%d opacity=%.2f\n",
        app.config->Appearance().highlightColor.c_str(), app.config->Appearance().normalColor.c_str(),
        app.config->Appearance().fontFamily.c_str(), app.config->Appearance().fontSize,
        app.config->Appearance().normalOpacity);
    app.renderer->ApplySettings(app.config->Appearance());
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
        break;
    }
    case ID_MENU_RECONNECT: {
        if (app.wsClient) app.wsClient->RequestReconnect();
        break;
    }
    case ID_MENU_SETTINGS: {
        // D2D 原生自绘设置界面
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
        }

        if (app.d2dSettingsWindow->Show(app.hInstance, app.hwnd, *app.config)) {
            break;
        }
        app.d2dSettingsWindow.reset();
        app.d2dSettingsWindow = nullptr;

        // ═══════ 回退：Win32 原生对话框 ═══════
        if (moekoe::ConfigDialog::Show(app.hInstance, app.hwnd, *app.config,
                                       /*boundMode*/ false,
                                       [&app]() {
                                           // 同 ID_MENU_EXIT：ConfigDialog 的模态循环
                                           // 嵌套在 TrackPopupMenuEx 内，WM_QUIT 会被吞掉
                                           app.running = false;
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
            // 同 ID_MENU_EXIT：当前仍在 TrackPopupMenuEx 模态循环内，
            // PostQuitMessage 发出的 WM_QUIT 会被吞掉，仅设 running=false
            app.running = false;
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
        // 不能在此调用 PostQuitMessage(0)：当前处在 TrackPopupMenuEx 的
        // 模态消息循环中，WM_QUIT 会被其内部 GetMessage 吞掉，导致回到
        // 主消息循环后 GetMessage 永久阻塞，程序卡死。
        // 仅设置 running=false，由主循环的 while 条件 (app.running && GetMessage)
        // 短路跳出即可。
        app.running = false;
        break;
    }
    case ID_MENU_OPEN_MOEKOE: {
        // 尝试查找并激活 MoeKoeMusic 主窗口
        // MoeKoeMusic 是 Electron 应用，窗口类名为 Chrome_WidgetWin_1
        HWND hMoeKoe = ::FindWindowW(L"Chrome_WidgetWin_1", nullptr);
        if (hMoeKoe) {
            // 检查进程名是否为 MoeKoeMusic
            DWORD pid = 0;
            ::GetWindowThreadProcessId(hMoeKoe, &pid);
            bool isMoeKoe = false;
            HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                wchar_t procName[MAX_PATH] = {};
                if (::GetModuleFileNameExW(hProc, nullptr, procName, MAX_PATH)) {
                    std::wstring name(procName);
                    // 不区分大小写检查进程名是否包含 MoeKoeMusic
                    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                    isMoeKoe = (name.find(L"moekoemusic") != std::wstring::npos);
                }
                ::CloseHandle(hProc);
            }
            if (isMoeKoe) {
                if (::IsIconic(hMoeKoe)) {
                    ::ShowWindow(hMoeKoe, SW_RESTORE);
                }
                ::SetForegroundWindow(hMoeKoe);
            } else {
                // 找到的不是 MoeKoeMusic，继续枚举
                hMoeKoe = nullptr;
                // 使用 EnumWindows 枚举所有 Chrome_WidgetWin_1 窗口
                struct EnumData { HWND result; } data{nullptr};
                ::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                    auto* d = reinterpret_cast<EnumData*>(lParam);
                    wchar_t cls[256] = {};
                    ::GetClassNameW(hwnd, cls, 256);
                    if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0) return TRUE;
                    if (!::IsWindowVisible(hwnd)) return TRUE;
                    DWORD pid2 = 0;
                    ::GetWindowThreadProcessId(hwnd, &pid2);
                    HANDLE hP = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid2);
                    if (hP) {
                        wchar_t pn[MAX_PATH] = {};
                        if (::GetModuleFileNameExW(hP, nullptr, pn, MAX_PATH)) {
                            std::wstring nm(pn);
                            std::transform(nm.begin(), nm.end(), nm.begin(), ::towlower);
                            if (nm.find(L"moekoemusic") != std::wstring::npos) {
                                d->result = hwnd;
                                ::CloseHandle(hP);
                                return FALSE;
                            }
                        }
                        ::CloseHandle(hP);
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&data));
                if (data.result) {
                    if (::IsIconic(data.result)) ::ShowWindow(data.result, SW_RESTORE);
                    ::SetForegroundWindow(data.result);
                } else {
                    Log("[OPEN-MOEKOE] MoeKoeMusic window not found\n");
                }
            }
        } else {
            Log("[OPEN-MOEKOE] MoeKoeMusic window not found\n");
        }
        break;
    }
    case ID_MENU_EXPORT_LOG: {
        // 生成默认文件名：MoeKoeTaskbarLyrics_debug_YYYYMMDD_HHMMSS.log
        SYSTEMTIME st;
        ::GetLocalTime(&st);
        wchar_t defaultName[128];
        _snwprintf_s(defaultName, _TRUNCATE,
            L"MoeKoeTaskbarLyrics_debug_%04d%02d%02d_%02d%02d%02d.log",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        // 获取 debug.log 路径
        std::string logPath = moekoe::GetLogPath();
        if (logPath.empty()) {
            ::MessageBoxW(app.hwnd,
                L"日志文件路径未知，无法导出。",
                L"导出日志", MB_OK | MB_ICONWARNING);
            break;
        }

        // 检查 debug.log 是否存在
        DWORD attr = ::GetFileAttributesA(logPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            ::MessageBoxW(app.hwnd,
                L"debug.log 文件不存在，可能尚未产生日志。",
                L"导出日志", MB_OK | MB_ICONWARNING);
            break;
        }

        // 使用 IFileSaveDialog 弹出"另存为"对话框
        IFileSaveDialog* pSaveDlg = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_FileSaveDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pSaveDlg));
        if (FAILED(hr) || !pSaveDlg) {
            ::MessageBoxW(app.hwnd,
                L"无法创建文件保存对话框。",
                L"导出日志", MB_OK | MB_ICONERROR);
            break;
        }

        pSaveDlg->SetFileName(defaultName);
        pSaveDlg->SetDefaultExtension(L"log");

        COMDLG_FILTERSPEC rgSpec[] = {
            {L"日志文件 (*.log)", L"*.log"},
            {L"所有文件 (*.*)", L"*.*"}
        };
        pSaveDlg->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
        pSaveDlg->SetFileTypeIndex(0);

        hr = pSaveDlg->Show(app.hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem* pItem = nullptr;
            hr = pSaveDlg->GetResult(&pItem);
            if (SUCCEEDED(hr) && pItem) {
                PWSTR pszPath = nullptr;
                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                if (SUCCEEDED(hr) && pszPath) {
                    // UTF-8 → 宽字符，供 CopyFileW 使用
                    int srcLen = ::MultiByteToWideChar(CP_UTF8, 0,
                        logPath.c_str(), -1, nullptr, 0);
                    std::wstring srcWide(static_cast<size_t>(srcLen), L'\0');
                    ::MultiByteToWideChar(CP_UTF8, 0,
                        logPath.c_str(), -1, &srcWide[0], srcLen);

                    if (::CopyFileW(srcWide.c_str(), pszPath, FALSE)) {
                        std::wstring msg = L"日志已成功导出到：\n";
                        msg += pszPath;
                        ::MessageBoxW(app.hwnd, msg.c_str(),
                            L"导出日志", MB_OK | MB_ICONINFORMATION);
                        Log("[EXPORT] Log exported to: %ls\n", pszPath);
                    } else {
                        wchar_t errBuf[320];
                        _snwprintf_s(errBuf, _TRUNCATE,
                            L"导出失败（错误码：%lu）。\n目标路径：%s",
                            ::GetLastError(), pszPath);
                        ::MessageBoxW(app.hwnd, errBuf,
                            L"导出日志", MB_OK | MB_ICONERROR);
                        Log("[EXPORT] CopyFile failed: error=%lu\n", ::GetLastError());
                    }
                    ::CoTaskMemFree(pszPath);
                }
                pItem->Release();
            }
        }
        // 用户取消无需提示
        pSaveDlg->Release();
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
        // 翻译模式子菜单命令（根据显示模式区分处理）
        if (id >= moekoe::ID_MENU_TRANSLATION_MODE && id <= moekoe::ID_MENU_TRANSLATION_MODE + 2) {
            const int idx = static_cast<int>(id - moekoe::ID_MENU_TRANSLATION_MODE);
            const auto& dispMode = app->config->Appearance().displayMode;
            if (dispMode == "card") {
                // 卡片模式：ID+0=原(off), ID+1=译(replace), ID+2=原-译(dual)
                const char* modes[] = {"off", "replace", "dual"};
                app->config->MutableAppearance().cardTranslationMode = modes[idx];
                app->config->MutableAppearance().enableTranslation = (idx != 0);
            } else {
                // 卡拉OK模式：ID+0=原(off), ID+1=译(replace)
                const char* modes[] = {"off", "replace"};
                app->config->MutableAppearance().translationMode = modes[idx];
                app->config->MutableAppearance().enableTranslation = (idx == 1);
            }
            app->config->Save();
            ApplyRendererSettings(*app);
            // 刷新托盘菜单以反映翻译模式变更
            app->tray->SetDisplayMode(dispMode);
            app->tray->SetCardTranslationMode(app->config->Appearance().cardTranslationMode);
            return 0;
        }
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

            // 2.5. 全屏检测防抖：由 FullscreenDetector 独立处理
            if (app->taskbarWindow && app->config && app->config->Advanced().enableFullscreenHide) {
                bool shouldHide = false;
                if (app->taskbarWindow->Fullscreen().Update(
                        app->config->Advanced().enableFullscreenHide,
                        app->config->Advanced().debugLog,
                        shouldHide)) {
                    app->taskbarWindow->SetFullscreenHidden(shouldHide);
                    if (!shouldHide) {
                        // 退出全屏恢复时重定位，确保使用用户拖动的 dragOffset 而非清零
                        app->taskbarWindow->InvalidatePositionCache();
                        app->taskbarWindow->Reposition();
                    }
                }

                // Shell 交互即时恢复：消费 forceDebounceReset 标志
                if (app->taskbarWindow->Fullscreen().ConsumeForceDebounceReset()) {
                    if (app->config->Advanced().debugLog)
                        Log("[FullscreenDetect] debounce reset by external trigger (shell interaction)\n");
                }
            }

            // 2.6. 全屏隐藏时跳过渲染（与 APPBAR 同理）
            if (app->taskbarWindow && app->taskbarWindow->IsFullscreenHidden()) return 0;

            // 3. 从歌词解析器获取当前应渲染的状态
            if (app->parser && app->renderer) {
                auto state = app->parser->GetCurrentRenderState();

                // 3.1 集成频谱数据（纯音乐播放时使用）
                if (app->spectrumCapture && app->spectrumCapture->IsRunning()) {
                    state.spectrumBands = app->spectrumCapture->GetSpectrum(SPECTRUM_NUM_BANDS);
                }

                // 3. 附加 UI 状态（悬停/拖动，用于判断是否显示控制按钮）
                if (app->taskbarWindow) {
                    state.isHovering = app->taskbarWindow->IsHovering();
                    state.isDragging = app->taskbarWindow->IsDragging();
                }

                // 4. 同步任务栏方向（运行时位置变化适配）+ 执行渲染
                app->renderer->SetVerticalTaskbar(app->taskbarWindow->IsVerticalTaskbar());
                app->renderer->Render(state);

                // 4.5. 卡片模式动态宽度：长歌词扩展，短歌词滞回缩回
                if (app->config->Appearance().displayMode == "card" &&
                    state.hasLyrics && !state.currentLine.empty()) {
                    const float newWidthDip = app->renderer->MeasureCardLyricsWidth(
                        state.currentLine, state.nextLine);
                    const int newWidthInt = static_cast<int>(std::ceil(newWidthDip));
                    const int currentDip = app->taskbarWindow->GetDynamicCardWidthDip();

                    // 扩展：测量值更大时立即生效
                    if (newWidthInt > currentDip) {
                        app->taskbarWindow->SetDynamicCardWidthDip(newWidthInt);
                        app->taskbarWindow->Reposition();
                        app->shrinkStartTick_ = 0;
                    }
                    // 缩回：测量值 ≤ 默认宽度（360 DIPs）且当前处于扩展状态
                    // 连续 2 秒无长歌词后才缩回，避免短暂切换导致抖动
                    else if (newWidthInt <= CARD_MIN_WIDTH_BASE_DP * 2 &&
                             currentDip > CARD_MIN_WIDTH_BASE_DP * 2) {
                        if (app->shrinkStartTick_ == 0) {
                            app->shrinkStartTick_ = ::GetTickCount64();
                        } else if (::GetTickCount64() - app->shrinkStartTick_ >= 2000) {
                            app->taskbarWindow->SetDynamicCardWidthDip(0);
                            app->taskbarWindow->Reposition();
                            app->shrinkStartTick_ = 0;
                        }
                    } else {
                        app->shrinkStartTick_ = 0;
                    }
                }

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

    // 输出故障模块名
    if (ep->ExceptionRecord->ExceptionAddress) {
        HMODULE hMod = nullptr;
        if (::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress),
                &hMod)) {
            wchar_t modName[MAX_PATH] = {};
            ::GetModuleFileNameW(hMod, modName, MAX_PATH);
            Log("[CRASH] Faulting module: %ls\n", modName);
        }
    }

    // 输出调用栈（WalkHelperStack64）
    static const int kMaxFrames = 32;
    void* frames[kMaxFrames];
    USHORT nFrames = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    Log("[CRASH] Stack trace (%hu frames):\n", nFrames);
    for (USHORT i = 0; i < nFrames; ++i) {
        HMODULE hMod = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(frames[i]), &hMod);
        wchar_t modName[MAX_PATH] = {};
        if (hMod) ::GetModuleFileNameW(hMod, modName, MAX_PATH);
        DWORD offset = hMod
            ? static_cast<DWORD>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(hMod))
            : 0;
        Log("[CRASH]   #%02u %ls + 0x%08lX (%p)\n", i, hMod ? modName : L"<unknown>", offset, frames[i]);
    }

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

    // 启动时直接写入 MoeKoeMusic 的 config.json，确保 apiMode 已开启
    // 不依赖 Native Bridge 链路（content.js → background → bridge → main），
    // 避免链路中任一环节断裂导致 config.json 永远不被写入。
    // WriteApiMode 内部已做幂等检查（apiMode='on' 时直接跳过）。
    {
        const std::string cfgPath = moekoe::ApiEnabler::GetConfigPath();
        if (!cfgPath.empty()) {
            bool apiOk = moekoe::ApiEnabler::WriteApiMode(cfgPath);
            Log("[STARTUP] ApiEnabler::WriteApiMode %s (path=%s)\n",
                apiOk ? "OK" : "FAILED", cfgPath.c_str());
        } else {
            Log("[STARTUP] ApiEnabler::GetConfigPath returned empty, skipping\n");
        }
    }

    config.SetAutoStart(config.IsAutoStart());
    Log("[STARTUP] AutoStart=%s\n", config.IsAutoStart() ? "ON" : "OFF");

    // ═══════ 第 3 阶段：业务模块初始化 ═══════
    // 目的：创建核心业务逻辑模块（任务栏窗口、渲染引擎、WebSocket、HTTP服务器）
    // 创建系统托盘
    app.tray = std::make_unique<moekoe::TrayIcon>();
    auto& tray = *app.tray;
    tray.Initialize(hInstance, hMsgWnd);
    tray.SetMenuCheckedAutoStart(config.IsAutoStart());

    // 7) 查找任务栏（开机时 Explorer 可能尚未就绪，重试等待最多 5 秒）
    HWND hTaskbar = nullptr;
    for (int retry = 0; retry < 10; ++retry) {
        hTaskbar = moekoe::TaskbarWindow::FindTaskbarHandle();
        if (hTaskbar) break;
        wchar_t dbg[64];
        _snwprintf_s(dbg, _TRUNCATE, L"[TaskbarLyrics] FindTaskbarHandle retry %d/10: not found, waiting 500ms\n", retry + 1);
        ::OutputDebugStringW(dbg);
        ::Sleep(500);
    }
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

    // 应用配置中的显示模式（必须在 Reposition 之前，影响宽度计算）
    taskbarWindow.SetDisplayMode(config.Appearance().displayMode);

    // 应用配置中的位置偏移
    taskbarWindow.SetDragOffset(config.Position().offsetX, config.Position().offsetY);

    // 首次定位：全部配置就绪后统一触发（TaskbarWindow::Create 不再内部定位）
    taskbarWindow.Reposition();

    // 应用配置中的锁定状态
    taskbarWindow.SetPositionLocked(config.Position().lockPosition);
    taskbarWindow.SetFullyLocked(config.Position().lockFully);
    tray.SetMenuCheckedLockPos(config.Position().lockPosition);
    tray.SetMenuCheckedLockFull(config.Position().lockFully);

    // 拖动结束时保存位置偏移到配置
    taskbarWindow.OnDragEnd([&]() {
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

    // 10.1) 启动频谱捕获（WASAPI loopback，始终运行，开销极低）
    app.spectrumCapture = std::make_unique<moekoe::SpectrumCapture>();
    app.spectrumCapture->Start();

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
    bool firstConnected = true;
    wsClient.OnConnectionStatus([&](bool connected) {
        if (app.tray) {
            app.tray->SetTooltip(connected
                ? L"MoeKoe Taskbar Lyrics (已连接)"
                : L"MoeKoe Taskbar Lyrics (等待连接...)");
            if (connected && firstConnected) {
                firstConnected = false;
                app.tray->ShowBalloon(
                    L"MoeKoe Taskbar Lyrics",
                    L"已连接到 MoeKoeMusic API，歌词将在播放时自动显示");
            }
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

    // 注册歌词窗口右键菜单回调
    taskbarWindow.OnContextMenuCommand([&app, hMsgWnd](UINT menuId) {
        // 翻译模式子菜单项（根据显示模式区分处理）
        if (menuId >= moekoe::ID_MENU_TRANSLATION_MODE && menuId <= moekoe::ID_MENU_TRANSLATION_MODE + 2) {
            const int idx = static_cast<int>(menuId - moekoe::ID_MENU_TRANSLATION_MODE);
            const auto& dispMode = app.config->Appearance().displayMode;
            if (dispMode == "card") {
                const char* modes[] = {"off", "replace", "dual"};
                app.config->MutableAppearance().cardTranslationMode = modes[idx];
                app.config->MutableAppearance().enableTranslation = (idx != 0);
            } else {
                const char* modes[] = {"off", "replace"};
                app.config->MutableAppearance().translationMode = modes[idx];
                app.config->MutableAppearance().enableTranslation = (idx == 1);
            }
            app.config->Save();
            ApplyRendererSettings(app);
            return;
        }

        // 其他命令复用托盘菜单处理逻辑
        ::PostMessageW(hMsgWnd, WM_COMMAND, menuId, 0);
    });


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
                st.isPlaying = j["isPlaying"].get<bool>();
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

            if (action == "enableApiMode") {
                // content.js 用户授权后触发：写入 config.json 使主进程启动 WebSocket
                const std::string cfgPath = moekoe::ApiEnabler::GetConfigPath();
                const bool ok = cfgPath.empty() ? false : moekoe::ApiEnabler::WriteApiMode(cfgPath);
                Log("[NATIVE-HOST] enableApiMode: %s (path=%s)\n",
                    ok ? "success" : "failed", cfgPath.c_str());

                nlohmann::json response;
                response["action"] = "enableApiMode";
                response["result"] = ok ? "ok" : "fail";
                response["path"] = cfgPath;
                app.nativeHost->SendPayloadEvent(response);
            }
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
    if (app.spectrumCapture) {
        app.spectrumCapture->Stop();
    }
    httpServer.Stop();
    wsClient.Disconnect();
    renderer.Shutdown();
    taskbarWindow.Destroy();
    tray.Shutdown();
    ::DestroyWindow(hMsgWnd);

    Log("[SHUTDOWN] Complete\n");
    return 0;
}
