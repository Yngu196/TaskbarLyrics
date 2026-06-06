// SPDX-License-Identifier: GPL-2.0
// main.cpp - MoeKoeMusic 任务栏歌词插件入口
//
#include "config.h"
#include "config_dialog.h"
#include "http_server.h"
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

// 检测是否运行在 Native Messaging 模式
// Chrome 启动 Native Host 时会连接 stdin 为管道
bool IsNativeMessagingMode() {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE || hStdin == NULL) {
        return false;
    }
    // 管道类型 = FILE_TYPE_PIPE
    DWORD type = GetFileType(hStdin);
    return (type == FILE_TYPE_PIPE);
}

// Native Messaging 模式下的命令处理
moekoe::NativeResponse HandleNativeCommand(const moekoe::NativeMessage& msg) {
    moekoe::NativeResponse resp;
    
    if (msg.command == "ping") {
        resp.success = true;
        resp.message = "ok";
        resp.data["version"] = "1.0.0";
        resp.data["running"] = true;
    }
    else if (msg.command == "status") {
        resp.success = true;
        resp.message = "status";
        // TODO: 返回当前歌词状态
        resp.data["connected"] = true;
    }
    else if (msg.command == "start") {
        // 启动歌词显示（如果尚未运行）
        resp.success = true;
        resp.message = "started";
    }
    else if (msg.command == "stop" || msg.command == "shutdown") {
        resp.success = true;
        resp.message = "shutting down";
        // 返回后 Run() 会退出
    }
    else {
        resp.success = false;
        resp.message = "unknown command: " + msg.command;
    }
    
    return resp;
}

} // namespace

namespace {

// 日志文件路径（EXE 所在目录 / debug.log）
std::string g_logPath;

void DebugLog(const char* fmt, ...) {
    if (g_logPath.empty()) return;
    FILE* f = fopen(g_logPath.c_str(), "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

// 初始化日志路径
void InitLogPath() {
    wchar_t exePath[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *slash = L'\0';
    std::wstring dir(exePath);
    // UTF-16 -> UTF-8
    int len = ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(),
                                    static_cast<int>(dir.size()), nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        std::string dirUtf8(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(),
                              static_cast<int>(dir.size()), &dirUtf8[0], len, nullptr, nullptr);
        g_logPath = dirUtf8 + "\\debug.log";
    }
}

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
    HINSTANCE                hInstance{nullptr};
    HWND                     hwnd{nullptr}; // 隐式消息窗口
    bool                     running{true};
    bool                     boundMode{false};
};

// 工具: 把 UTF-8 字符串限制到 Windows Tooltip 长度
std::wstring ToTooltipWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    if (len > 127) len = 127;
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
    app.renderer->ApplySettings(rs);
}

// 菜单命令处理
void OnTrayCommand(AppContext& app, UINT menuId) {
    using namespace moekoe;
    switch (menuId) {
    case ID_MENU_ENABLE: {
        const bool newState = !app.config->IsEnabled();
        app.config->SetEnabled(newState);
        if (newState) {
            app.config->SetAutoStart(true);
        }
        app.config->Save();

        if (app.tray) {
            app.tray->SetMenuCheckedEnable(newState);
            app.tray->SetMenuCheckedAutoStart(app.config->IsAutoStart());
        }
        if (!newState) {
            ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        }
        break;
    }
    case ID_MENU_AUTOSTART: {
        const bool newState = !app.config->IsAutoStart();
        app.config->SetAutoStart(newState);
        app.config->Save();
        if (app.tray) {
            app.tray->SetMenuCheckedAutoStart(newState);
        }
        break;
    }
    case ID_MENU_RECONNECT: {
        if (app.wsClient) app.wsClient->RequestReconnect();
        break;
    }
    case ID_MENU_SETTINGS: {
        // 打开 GUI 配置界面
        if (ConfigDialog::Show(app.hInstance, app.hwnd, *app.config,
                               app.boundMode,
                               [&app]() {
                                   app.running = false;
                                   ::PostQuitMessage(0);
                               })) {
            ApplyRendererSettings(app);
        }
        break;
    }
    case ID_MENU_UNBIND: {
        // 解除绑定：退出程序
        int ret = ::MessageBoxW(app.hwnd,
            L"解除绑定后将退出本程序。\n\n下次启动请以独立模式运行（不放在 MoeKoeMusic 目录下）。\n\n确定要解除绑定吗？",
            L"解除绑定", MB_YESNO | MB_ICONQUESTION);
        if (ret == IDYES) {
            app.running = false;
            ::PostQuitMessage(0);
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

    case WM_USER + 0x200: { // 托盘回调
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
            if (app->taskbarWindow) app->taskbarWindow->CheckResize();
            if (app->parser && app->renderer) {
                auto state = app->parser->GetCurrentRenderState();
                if (app->taskbarWindow) {
                    state.isHovering = app->taskbarWindow->IsHovering();
                }
                app->renderer->Render(state);
                if (app->tray && state.hasLyrics) {
                    auto tip = ToTooltipWide(state.currentLine);
                    if (!tip.empty()) {
                        app->tray->SetTooltip(tip);
                    }
                }
            }
        } catch (const std::exception& e) {
            DebugLog("[CRASH] WM_TIMER exception: %s\n", e.what());
        } catch (...) {
            DebugLog("[CRASH] WM_TIMER unknown exception\n");
        }
        return 0;
    }

    case WM_USER + 0x300: {
        try {
            if (app->parser && app->renderer && app->taskbarWindow) {
                auto state = app->parser->GetCurrentRenderState();
                state.isHovering = app->taskbarWindow->IsHovering();
                app->renderer->Render(state);
            }
        } catch (...) {}
        return 0;
    }

    // 绑定模式：MoeKoeMusic 进程退出时自动退出
    case WM_USER + 0x400: {
        DebugLog("[BOUND] MoeKoeMusic exited, shutting down\n");
        app->running = false;
        ::PostQuitMessage(0);
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
    DebugLog("[CRASH] Unhandled exception code=0x%08lX at address=%p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR /*cmdLine*/, int /*nShow*/) {
    // ── Native Messaging 模式检测 ──────────────────────────────
    // Chrome 启动 Native Host 时会以管道连接 stdin/stdout
    if (IsNativeMessagingMode()) {
        // 进入 Native Messaging 模式（无 GUI，只处理 stdin/stdout）
        moekoe::NativeMessagingHost host;
        host.SetCommandHandler(HandleNativeCommand);
        host.Run();  // 阻塞直到收到 shutdown 或 stdin 关闭
        return 0;
    }

    // ── 正常 GUI 模式 ──────────────────────────────────────────
    // 初始化日志路径（EXE 所在目录）
    InitLogPath();

    // COM 初始化 (WIC 需要)
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 全局异常过滤器
    ::SetUnhandledExceptionFilter(GlobalExceptionHandler);

    // 单实例检查
    ::CreateMutexW(nullptr, FALSE, L"MoeKoeTaskbarLyrics_Mutex");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    DebugLog("[STARTUP] WinMain entered\n");

    // 0) 初始化 Winsock（ixwebsocket 依赖）
    WSADATA wsaData;
    int wsRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    DebugLog("[STARTUP] WSAStartup ret=%d\n", wsRet);

    // 1) 声明 DPI 感知(Per-Monitor V2)
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 2) 加载配置
    moekoe::Config config;
    config.Load();
    DebugLog("[STARTUP] Config loaded\n");

    // 同步开机自启注册表
    config.SetAutoStart(config.IsAutoStart());

    // 3) 注册消息窗口类
    if (!RegisterMessageClass(hInstance)) {
        std::fprintf(stderr, "[Error] RegisterClassExW failed\n");
        return 1;
    }

    // 4) 创建隐式消息窗口
    HWND hMsgWnd = ::CreateWindowExW(
        0, L"MoeKoeTaskbarLyricsMsg", L"",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!hMsgWnd) {
        std::fprintf(stderr, "[Error] Create message window failed\n");
        return 1;
    }

    // 检测绑定模式
    bool boundMode = moekoe::ProcessMonitor::IsBoundMode();
    DebugLog("[STARTUP] Bound mode: %s\n", boundMode ? "YES" : "NO");

    AppContext app;
    app.config = &config;
    app.hwnd   = hMsgWnd;
    app.hInstance = hInstance;
    app.boundMode = boundMode;
    ::SetWindowLongPtrW(hMsgWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    // 5) 创建系统托盘
    moekoe::TrayIcon tray;
    app.tray = &tray;
    tray.Initialize(hInstance, hMsgWnd);
    tray.SetMenuCheckedEnable(config.IsEnabled());
    tray.SetMenuCheckedAutoStart(config.IsAutoStart());
    tray.SetBoundMode(boundMode);

    // 6) 如果用户禁用了插件, 仅保留托盘等待重新启用
    if (!config.IsEnabled()) {
        MSG msg{};
        while (::GetMessageW(&msg, nullptr, 0, 0)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        tray.Shutdown();
        ::DestroyWindow(hMsgWnd);
        return 0;
    }

    // 7) 查找任务栏
    HWND hTaskbar = moekoe::TaskbarWindow::FindTaskbarHandle();
    DebugLog("[STARTUP] FindTaskbar hTaskbar=%p\n", hTaskbar);
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

    // 初始隐藏窗口，收到歌词数据后再显示
    ::ShowWindow(taskbarWindow.GetHandle(), SW_HIDE);

    // 9) 初始化渲染器
    moekoe::TaskbarRenderer renderer;
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
            ::SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
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
        ::PostMessageW(hMsgWnd, WM_USER + 0x300, 0, 0);
    });

    char url[64];
    std::snprintf(url, sizeof(url), "ws://127.0.0.1:%d",
                  config.Advanced().websocketPort);
    wsClient.Connect(url);

    // 10.5) 启动 HTTP 服务器（用于 popup.js 通信：ping / shutdown）
    moekoe::HttpServer httpServer;
    app.httpServer = &httpServer;
    httpServer.OnCommand([&](const std::string& command) {
        DebugLog("[HTTP] Command received: %s\n", command.c_str());
        if (command == "shutdown") {
            DebugLog("[HTTP] Shutdown via HTTP, exiting...\n");
            app.running = false;
            ::PostQuitMessage(0);
        }
    });
    const int httpPort = 6521; // popup.js 固定使用此端口
    if (httpServer.Start(httpPort)) {
        DebugLog("[STARTUP] HTTP server started on port %d\n", httpPort);
    } else {
        DebugLog("[STARTUP] HTTP server failed to start (non-fatal)\n");
    }

    // 11) 进程监控：始终监控 MoeKoeMusic.exe 进程
    //     - 绑定模式(插件部署)：EXE 在 extensions/ 子目录，MoeKoeMusic 退出时自动退出
    //     - 独立模式(单独运行)：检测不到 MoeKoeMusic 进程，不会触发退出，行为不变
    moekoe::ProcessMonitor processMonitor;
    app.processMonitor = &processMonitor;
    processMonitor.Start(L"MoeKoeMusic.exe",
        /*onStarted*/ []() {},
        /*onExited*/ [hMsgWnd]() {
            ::PostMessageW(hMsgWnd, WM_USER + 0x400, 0, 0);
        });
    DebugLog("[STARTUP] Process monitor started (bound=%s)\n", boundMode ? "YES" : "NO");

    // 启动帧定时器
    const int intervalMs = std::max(15, 1000 / std::max(1, config.Advanced().refreshRateHz));
    ::SetTimer(hMsgWnd, /*id*/1, static_cast<UINT>(intervalMs), nullptr);

    // 12) 消息循环
    MSG msg{};
    while (app.running && ::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    DebugLog("[SHUTDOWN] Message loop ended\n");

    // 13) 清理
    ::KillTimer(hMsgWnd, 1);
    httpServer.Stop();
    processMonitor.Stop();
    wsClient.Disconnect();
    renderer.Shutdown();
    taskbarWindow.Destroy();
    tray.Shutdown();
    ::DestroyWindow(hMsgWnd);

    DebugLog("[SHUTDOWN] Complete\n");
    return 0;
}
