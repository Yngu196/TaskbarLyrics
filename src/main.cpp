// SPDX-License-Identifier: GPL-2.0
// main.cpp - MoeKoeMusic 任务栏歌词插件入口
//
#include "config.h"
#include "config_dialog.h"
#include "constants.h"
#include "http_server.h"
#include "logger.h"
#include "settings_window.h"
#include "lyrics_data.h"
#include "lyrics_parser.h"
#include "native_messaging.h"
#include "process_monitor.h"
#include "renderer.h"
#include "taskbar_window.h"
#include "tray_icon.h"
#include "websocket_client.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

// 日志快捷方式（使用统一日志系统）
using moekoe::Log;

// 全局应用上下文
struct AppContext {
    moekoe::Config*          config{nullptr};
    moekoe::TaskbarWindow*   taskbarWindow{nullptr};
    moekoe::TaskbarRenderer* renderer{nullptr};
    moekoe::TrayIcon*        tray{nullptr};
    moekoe::WebSocketClient* wsClient{nullptr};
    moekoe::LyricsParser*    parser{nullptr};
    moekoe::ProcessMonitor*  processMonitor{nullptr};
    moekoe::HttpServer*      httpServer{nullptr};
    moekoe::SettingsWindow*   settingsWindow{nullptr};
    moekoe::NativeMessagingHost* nativeHost{nullptr};
    HINSTANCE                hInstance{nullptr};
    HWND                     hwnd{nullptr}; // 隐式消息窗口
    bool                     running{true};
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
        // 优先尝试 WebView2 设置界面
        bool useWebView2 = true;
        if (!app.settingsWindow) {
            app.settingsWindow = new moekoe::SettingsWindow();
            app.settingsWindow->OnConfigChanged([&](const moekoe::Config& cfg) {
                // 将 WebView2 设置界面更新的配置同步到主 config 对象
                *app.config = cfg;
                ApplyRendererSettings(app);
                if (app.taskbarWindow) {
                    app.taskbarWindow->SetDragOffset(
                        cfg.Position().offsetX, cfg.Position().offsetY);
                    app.taskbarWindow->Reposition();
                }
                // 同步托盘菜单状态
                if (app.tray) {
                    app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                }
                Log("[SETTINGS] Config applied and saved\n");
            });
        }

        if (useWebView2) {
            if (app.settingsWindow->Show(app.hInstance, app.hwnd, *app.config)) {
                // WebView2 窗口创建成功（异步初始化中）
                break;
            } else {
                // WebView2 完全失败，回退到 Win32 对话框
                delete app.settingsWindow;
                app.settingsWindow = nullptr;
            }
        }

        // 回退: 使用旧的 Win32 配置对话框（始终以独立模式显示）
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

            // 3. 从歌词解析器获取当前应渲染的状态
            if (app->parser && app->renderer) {
                auto state = app->parser->GetCurrentRenderState();

                // 3. 附加 UI 状态（悬停/拖动，用于判断是否显示控制按钮）
                if (app->taskbarWindow) {
                    state.isHovering = app->taskbarWindow->IsHovering();
                    state.isDragging = app->taskbarWindow->IsDragging();
                }

                // 4. 执行渲染（Direct2D 绘制）
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
    moekoe::Config config;
    config.Load();
    moekoe::SetLogEnabled(config.Advanced().debugLog);
    Log("[STARTUP] Config loaded\n");
    config.SetAutoStart(config.IsAutoStart());  // 同步开机自启注册表
    Log("[STARTUP] AutoStart=%s\n", config.IsAutoStart() ? "ON" : "OFF");

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
    app.config = &config;
    app.hwnd   = hMsgWnd;
    app.hInstance = hInstance;
    ::SetWindowLongPtrW(hMsgWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    // ═══════ 第 3 阶段：业务模块初始化 ═══════
    // 目的：创建核心业务逻辑模块（任务栏窗口、渲染引擎、WebSocket、HTTP服务器）
    // 创建系统托盘
    moekoe::TrayIcon tray;
    app.tray = &tray;
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
    moekoe::TaskbarWindow taskbarWindow;
    if (!taskbarWindow.Create(hInstance, hTaskbar)) {
        ::MessageBoxW(nullptr,
                      L"创建任务栏歌词窗口失败。",
                      L"MoeKoe Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }
    app.taskbarWindow = &taskbarWindow;

    // 应用配置中的位置偏移
    taskbarWindow.SetDragOffset(config.Position().offsetX, config.Position().offsetY);

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
    moekoe::TaskbarRenderer renderer;
    app.renderer = &renderer;
    ApplyRendererSettings(app);
    if (!renderer.Initialize(taskbarWindow.GetHandle())) {
        ::MessageBoxW(nullptr,
                      L"Direct2D 初始化失败。",
                      L"MoeKoe Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }
    app.renderer = &renderer;

    // 10) 启动 WebSocket 客户端 + 歌词解析
    moekoe::LyricsParser parser;
    app.parser = &parser;

    moekoe::WebSocketClient wsClient;
    app.wsClient = &wsClient;

    wsClient.OnLyrics([&](const moekoe::LyricsData& data) {
        parser.UpdateLyrics(data);
        if (app.taskbarWindow && data.valid) {
            HWND h = taskbarWindow.GetHandle();
            ::ShowWindow(h, SW_SHOWNA);
            ::SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    });
    wsClient.OnPlayerState([&](const moekoe::PlayerState& st) {
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

    char url[64];
    std::snprintf(url, sizeof(url), "ws://127.0.0.1:%d",
                  config.Advanced().websocketPort);
    wsClient.Connect(url);

    // 10.5) 启动 HTTP 服务器（用于 popup.js 通信：ping / shutdown）
    moekoe::HttpServer httpServer;
    app.httpServer = &httpServer;
    httpServer.OnCommand([&](const std::string& command) {
        Log("[HTTP] Command received: %s\n", command.c_str());
        if (command == "shutdown") {
            Log("[HTTP] Shutdown via HTTP, exiting...\n");
            app.running = false;
            ::PostQuitMessage(0);
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
    moekoe::NativeMessagingHost nativeHost;
    app.nativeHost = &nativeHost;
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
