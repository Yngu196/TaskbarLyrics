// SPDX-License-Identifier: GPL-2.0
// main.cpp - MoeKoeMusic 任务栏歌词插件入口
//
#include "config.h"
#include "lyrics_data.h"
#include "lyrics_parser.h"
#include "renderer.h"
#include "taskbar_window.h"
#include "tray_icon.h"
#include "websocket_client.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

// 全局应用上下文
struct AppContext {
    moekoe::Config*         config{nullptr};
    moekoe::TaskbarWindow*  taskbarWindow{nullptr};
    moekoe::TaskbarRenderer* renderer{nullptr};
    moekoe::TrayIcon*       tray{nullptr};
    moekoe::WebSocketClient* wsClient{nullptr};
    moekoe::LyricsParser*   parser{nullptr};
    HWND                    hwnd{nullptr}; // 隐式消息窗口
    bool                    running{true};
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
            // 禁用: 退出进程(下次启动不再创建嵌入窗口)
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
        // 实际注册表操作在外部 RegSet 工具中
        // 这里仅写配置
        break;
    }
    case ID_MENU_RECONNECT: {
        if (app.wsClient) app.wsClient->RequestReconnect();
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
        // 每帧: 任务栏尺寸自适应 + 渲染 + 托盘 Tooltip 更新
        try {
            if (app->taskbarWindow) app->taskbarWindow->CheckResize();
            if (app->parser && app->renderer) {
                const auto state = app->parser->GetCurrentRenderState();
                app->renderer->Render(state);
                if (app->tray && state.hasLyrics) {
                    auto tip = ToTooltipWide(state.currentLine);
                    if (!tip.empty()) {
                        app->tray->SetTooltip(tip);
                    }
                }
            }
        } catch (const std::exception& e) {
            FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
            if (f) { fprintf(f, "[CRASH] WM_TIMER exception: %s\n", e.what()); fclose(f); }
        } catch (...) {
            FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
            if (f) { fprintf(f, "[CRASH] WM_TIMER unknown exception\n"); fclose(f); }
        }
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
    FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
    if (f) {
        fprintf(f, "[CRASH] Unhandled exception code=0x%08lX at address=%p\n",
                ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH; // 仍显示 Windows 错误对话框
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR /*cmdLine*/, int /*nShow*/) {
    // COM 初始化 (WIC 需要)
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 全局异常过滤器
    ::SetUnhandledExceptionFilter(GlobalExceptionHandler);

    // 单实例检查
    ::CreateMutexW(nullptr, FALSE, L"MoeKoeTaskbarLyrics_Mutex");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0; // 已有实例在运行
    }

    // 启动日志 - 定位崩溃位置
    {
        FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
        if (f) { fprintf(f, "[STARTUP] WinMain entered\n"); fclose(f); }
    }

    // 0) 初始化 Winsock（ixwebsocket 依赖）
    {
        FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
        if (f) { fprintf(f, "[STARTUP] Before WSAStartup\n"); fclose(f); }
    }
    WSADATA wsaData;
    int wsRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    {
        FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
        if (f) { fprintf(f, "[STARTUP] After WSAStartup ret=%d\n", wsRet); fclose(f); }
    }

    // 1) 声明 DPI 感知(Per-Monitor V2)
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
        if (f) { fprintf(f, "[STARTUP] After DPI\n"); fclose(f); }
    }

    // 2) 加载配置
    moekoe::Config config;
    config.Load();
    {
        FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
        if (f) { fprintf(f, "[STARTUP] After config.Load\n"); fclose(f); }
    }

    // 同步开机自启注册表（与配置一致）
    config.SetAutoStart(config.IsAutoStart());
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[STARTUP] After SetAutoStart\n");fclose(f);} }

    // 3) 注册消息窗口类
    if (!RegisterMessageClass(hInstance)) {
        std::fprintf(stderr, "[Error] RegisterClassExW failed\n");
        return 1;
    }
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[STARTUP] After RegisterClass\n");fclose(f);} }

    // 4) 创建隐式消息窗口
    HWND hMsgWnd = ::CreateWindowExW(
        0, L"MoeKoeTaskbarLyricsMsg", L"",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!hMsgWnd) {
        std::fprintf(stderr, "[Error] Create message window failed\n");
        return 1;
    }
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[STARTUP] After CreateWindow\n");fclose(f);} }

    AppContext app;
    app.config = &config;
    app.hwnd   = hMsgWnd;
    ::SetWindowLongPtrW(hMsgWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[STARTUP] After AppContext\n");fclose(f);} }

    // 5) 创建系统托盘
    moekoe::TrayIcon tray;
    app.tray = &tray;
    tray.Initialize(hInstance, hMsgWnd);
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[STARTUP] After TrayIcon Init\n");fclose(f);} }
    tray.SetMenuCheckedEnable(config.IsEnabled());
    tray.SetMenuCheckedAutoStart(config.IsAutoStart());

    // 6) 如果用户禁用了插件, 仅保留托盘等待重新启用
    if (!config.IsEnabled()) {
        // 进入简单消息循环
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
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[STARTUP] After FindTaskbar hTaskbar=%p\n",hTaskbar);fclose(f);} }
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
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[STARTUP] After TaskbarWindow Create\n");fclose(f);} }

    // 初始隐藏窗口，收到歌词数据后再显示
    ::ShowWindow(taskbarWindow.GetHandle(), SW_HIDE);

    // 9) 初始化渲染器
    moekoe::TaskbarRenderer renderer;
    moekoe::RendererSettings rs;
    rs.highlightColor    = config.Appearance().highlightColor;
    rs.normalColor       = config.Appearance().normalColor;
    rs.normalOpacity     = static_cast<float>(config.Appearance().normalOpacity);
    rs.fontFamily        = config.Appearance().fontFamily;
    rs.fontSize          = config.Appearance().fontSize;
    rs.enableKaraoke     = config.Appearance().enableKaraoke;
    rs.enableTranslation = config.Appearance().enableTranslation;
    renderer.ApplySettings(rs);
    if (!renderer.Initialize(taskbarWindow.GetHandle())) {
        ::MessageBoxW(nullptr,
                      L"Direct2D 初始化失败。",
                      L"MoeKoe Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }
    app.renderer = &renderer;
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[STARTUP] After Renderer Init\n");fclose(f);} }

    // 10) 启动 WebSocket 客户端 + 歌词解析
    moekoe::LyricsParser parser;
    app.parser = &parser;

    moekoe::WebSocketClient wsClient;
    app.wsClient = &wsClient;

    wsClient.OnLyrics([&](const moekoe::LyricsData& data) {
        parser.UpdateLyrics(data);
        // 收到歌词后显示窗口
        if (app.taskbarWindow && data.valid) {
            HWND h = taskbarWindow.GetHandle();
            ::ShowWindow(h, SW_SHOWNA);
            ::SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            // 调试: 检查窗口状态
            RECT rc{};
            ::GetWindowRect(h, &rc);
            BOOL vis = ::IsWindowVisible(h);
            {
                FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
                if (f) {
                    fprintf(f, "[LYRICS] ShowWindow called: hwnd=%p visible=%d rect=(%ld,%ld,%ld,%ld)\n",
                            h, vis, rc.left, rc.top, rc.right, rc.bottom);
                    fclose(f);
                }
            }
        }
        std::fprintf(stderr, "[lyrics] valid=%d lines=%zu\n", data.valid, data.lines.size());
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

    char url[64];
    std::snprintf(url, sizeof(url), "ws://127.0.0.1:%d",
                  config.Advanced().websocketPort);
    wsClient.Connect(url);

    // 启动 30 FPS 帧定时器(由消息窗口的 WM_TIMER 统一驱动)
    const int intervalMs = std::max(15, 1000 / std::max(1, config.Advanced().refreshRateHz));
    ::SetTimer(hMsgWnd, /*id*/1, static_cast<UINT>(intervalMs), nullptr);

    // 11) 消息循环
    MSG msg{};
    while (app.running && ::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[SHUTDOWN] After message loop\n");fclose(f);} }

    // 12) 清理
    ::KillTimer(hMsgWnd, 1);
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[SHUTDOWN] After KillTimer\n");fclose(f);} }
    wsClient.Disconnect();
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[SHUTDOWN] After WS Disconnect\n");fclose(f);} }
    renderer.Shutdown();
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[SHUTDOWN] After Renderer Shutdown\n");fclose(f);} }
    taskbarWindow.Destroy();
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[SHUTDOWN] After Taskbar Destroy\n");fclose(f);} }
    tray.Shutdown();
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[SHUTDOWN] After Tray Shutdown\n");fclose(f);} }
    ::DestroyWindow(hMsgWnd);
    { FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a"); if(f){fprintf(f,"[SHUTDOWN] After DestroyWindow\n");fclose(f);} }

    return 0;
}
