// SPDX-License-Identifier: GPL-3.0
// main.cpp - MoeKoeMusic 任务栏歌词插件入口
//
// 仅包含 WinMain（5 阶段生命周期编排）。
// 应用上下文、菜单命令、消息窗口、崩溃处理等逻辑已拆分至：
//   - app_context.h/.cpp      AppContext + 帧渲染/辅助函数
//   - tray_commands.h/.cpp    托盘/歌词窗口菜单命令
//   - message_window.h/.cpp   MsgWndProc + RegisterMessageClass
//   - crash_handler.h/.cpp    GlobalExceptionHandler
//
#include "app_context.h"
#include "api_enabler.h"
#include "constants.h"
#include "crash_handler.h"
#include "krc_parser.h"
#include "logger.h"
#include "message_window.h"
#include "tray_icon.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace moekoe::constants;

// 日志快捷方式（使用统一日志系统）
using moekoe::Log;

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR /*cmdLine*/, int /*nShow*/) {
    using namespace moekoe;

    // ═══════ 第 1 阶段：系统初始化 ═══════
    // 目的：为应用提供运行时基础（COM、异常处理、日志、单实例保护）
    InitLogger();
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

    app.config = std::make_unique<Config>();
    auto& config = *app.config;
    config.Load();
    SetLogEnabled(config.Advanced().debugLog);
    Log("[STARTUP] Config loaded\n");

    // 启动时直接写入 MoeKoeMusic 的 config.json，确保 apiMode 已开启
    // 不依赖 Native Bridge 链路（content.js → background → bridge → main），
    // 避免链路中任一环节断裂导致 config.json 永远不被写入。
    // WriteApiMode 内部已做幂等检查（apiMode='on' 时直接跳过）。
    {
        const std::string cfgPath = ApiEnabler::GetConfigPath();
        if (!cfgPath.empty()) {
            bool apiOk = ApiEnabler::WriteApiMode(cfgPath);
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
    app.tray = std::make_unique<TrayIcon>();
    auto& tray = *app.tray;
    tray.Initialize(hInstance, hMsgWnd);
    tray.SetMenuCheckedAutoStart(config.IsAutoStart());

    // 7) 查找任务栏（开机时 Explorer 可能尚未就绪，重试等待最多 5 秒）
    HWND hTaskbar = nullptr;
    for (int retry = 0; retry < 10; ++retry) {
        hTaskbar = TaskbarWindow::FindTaskbarHandle();
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
    app.taskbarWindow = std::make_unique<TaskbarWindow>();
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
    app.renderer = std::make_unique<TaskbarRenderer>();
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
    app.parser = std::make_unique<LyricsParser>();
    auto& parser = *app.parser;

    // 10.1) 启动频谱捕获（WASAPI loopback，始终运行，开销极低）
    app.spectrumCapture = std::make_unique<SpectrumCapture>();
    app.spectrumCapture->Start();

    app.wsClient = std::make_unique<WebSocketClient>();
    auto& wsClient = *app.wsClient;
    wsClient.SetDebugLog(config.Advanced().debugLog);

    wsClient.OnLyrics([&](const LyricsData& data) {
        if (app.config->Advanced().debugLog) Log("[WS] OnLyrics: valid=%d lines=%zu\n", data.valid, data.lines.size());
        // 性能监控：累加 WS 消息计数（仅 debug 模式有意义，但累加开销极低，无条件执行）
        app.perfWsMsgCount_.fetch_add(1, std::memory_order_relaxed);
        parser.UpdateLyrics(data);
        if (app.taskbarWindow && data.valid) {
            HWND h = taskbarWindow.GetHandle();
            ::ShowWindow(h, SW_SHOWNA);
            ::SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    });
    wsClient.OnPlayerState([&](const PlayerState& st) {
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
    taskbarWindow.OnButtonClicked([&](HoverButton btn) {
        if (!app.wsClient) return;
        switch (btn) {
        case HoverButton::Prev:
            app.wsClient->SendControl("prev");
            break;
        case HoverButton::PlayPause:
            app.wsClient->SendControl("toggle");
            break;
        case HoverButton::Next:
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
    // 设置消息窗口句柄，供歌词窗口右键菜单作为所有者
    taskbarWindow.SetMessageWindow(hMsgWnd);

    taskbarWindow.OnContextMenuCommand([&app, hMsgWnd](UINT menuId) {
        // 翻译模式子菜单项（根据显示模式区分处理）
        if (menuId >= ID_MENU_TRANSLATION_MODE && menuId <= ID_MENU_TRANSLATION_MODE + 2) {
            const int idx = static_cast<int>(menuId - ID_MENU_TRANSLATION_MODE);
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

        // 退出命令：直接设置标志，不通过 PostMessageW
        // 原因：PostMessageW 投递的 WM_COMMAND 需要主消息循环取出，
        // 但歌词窗口的 TrackPopupMenuEx 可能仍在处理后续消息，
        // 导致退出延迟或与窗口销毁产生竞争
        if (menuId == ID_MENU_EXIT) {
            app.running = false;
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
    app.httpServer = std::make_unique<HttpServer>();
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
            PlayerState st;
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
                LyricsData data;
                auto& ld = j.contains("lyricsData") ? j["lyricsData"] : j["data"];

                if (ld.is_array()) {
                    for (const auto& lineJson : ld) {
                        if (data.lines.size() >= constants::MAX_LYRIC_LINES) break;
                        LyricLine line;
                        line.text       = lineJson.value("text", "");
                        line.translated = lineJson.value("translated", "");

                        if (lineJson.contains("characters") && lineJson["characters"].is_array()) {
                            for (const auto& c : lineJson["characters"]) {
                                if (line.characters.size() >= constants::MAX_CHARS_PER_LINE) break;
                                CharacterTiming ct;
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
                    data = ParseKrcString(ld.get<std::string>());
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
                             : static_cast<int>(HTTP_SERVER_PORT);
    if (httpServer.Start(httpPort)) {
        Log("[STARTUP] HTTP server started on port %d\n", httpPort);
    } else {
        Log("[STARTUP] HTTP server failed to start on port %d (non-fatal)\n", httpPort);
    }

    // 11) 启动 Native Host stdin 读取（MoeKoeMusic 托管模式下的生命周期管理）
    // 在后台线程中读取 stdin JSON Lines，收到 shutdown 时通知主线程退出
    app.nativeHost = std::make_unique<NativeMessagingHost>();
    auto& nativeHost = *app.nativeHost;
    nativeHost.SetMessageHandler([&app](const NativeHostMessage& msg) {
        Log("[NATIVE-HOST] Received message: type=%s\n", msg.type.c_str());
        // 业务消息处理：可根据 payload 中的 action 执行对应操作
        if (msg.payload.contains("action")) {
            std::string action = msg.payload["action"].get<std::string>();
            Log("[NATIVE-HOST] Action: %s\n", action.c_str());

            if (action == "enableApiMode") {
                // content.js 用户授权后触发：写入 config.json 使主进程启动 WebSocket
                const std::string cfgPath = ApiEnabler::GetConfigPath();
                const bool ok = cfgPath.empty() ? false : ApiEnabler::WriteApiMode(cfgPath);
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
    // 注意：不再 detach，改为在关闭阶段 join + 超时
    Log("[STARTUP] Native Host stdin reader started\n");

    // 启动帧定时器
    const int intervalMs = std::max(MIN_FRAME_INTERVAL_MS, 1000 / std::max(1, config.Advanced().refreshRateHz));
    ::SetTimer(hMsgWnd, /*id*/1, static_cast<UINT>(intervalMs), nullptr);

    // ═══════ 第 4 阶段：业务逻辑循环 ═══════
    // 目的：处理消息和事件，直到应用关闭
    // 使用 PeekMessageW 替代 GetMessageW，确保 app.running=false 后循环能立即退出，
    // 不必等待下一条消息到达
    MSG msg{};
    while (app.running) {
        if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                Log("[SHUTDOWN] WM_QUIT received\n");
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        } else {
            // 无消息时短暂让出 CPU，防止忙等待
            ::Sleep(1);
        }
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

    // 关闭 Native Host stdin 线程
    // 注意：getline 在 stdin 上阻塞，无法可靠中断，
    // 设置 running_=false 后给 500ms 优雅退出时间，超时则 detach
    Log("[SHUTDOWN] Stopping Native Host stdin thread...\n");
    nativeHost.RequestShutdown();
    if (stdinThread.joinable()) {
        DWORD waitResult = ::WaitForSingleObject(stdinThread.native_handle(), 500);
        if (waitResult == WAIT_TIMEOUT) {
            Log("[SHUTDOWN] stdin thread did not exit in 500ms, detaching\n");
            stdinThread.detach();
        } else {
            stdinThread.join();
            Log("[SHUTDOWN] stdin thread joined\n");
        }
    }

    ::KillTimer(hMsgWnd, 1);
    if (app.spectrumCapture) {
        Log("[SHUTDOWN] Stopping spectrum capture...\n");
        app.spectrumCapture->Stop();
    }
    Log("[SHUTDOWN] Stopping HTTP server...\n");
    httpServer.Stop();
    Log("[SHUTDOWN] Disconnecting WebSocket...\n");
    wsClient.Disconnect();
    Log("[SHUTDOWN] Shutting down renderer...\n");
    renderer.Shutdown();
    Log("[SHUTDOWN] Destroying taskbar window...\n");
    taskbarWindow.Destroy();
    Log("[SHUTDOWN] Shutting down tray...\n");
    tray.Shutdown();
    ::DestroyWindow(hMsgWnd);

    Log("[SHUTDOWN] Complete\n");
    return 0;
}
