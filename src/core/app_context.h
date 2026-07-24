// SPDX-License-Identifier: GPL-3.0
// app_context.h - 全局应用上下文与应用辅助函数
//
// 从 main.cpp 提取，承载跨模块共享的应用状态与帧渲染逻辑。
//
#pragma once

#include "config/config.h"
#include "ui/d2d_settings_window.h"
#include "net/http_server.h"
#include "lyrics/lyrics_parser.h"
#include "net/native_messaging.h"
#include "render/renderer.h"
#include "render/spectrum_capture.h"
#include "taskbar/taskbar_window.h"
#include "ui/tray_icon.h"
#include "net/websocket_client.h"

#include <windows.h>
#include <atomic>
#include <memory>
#include <string>

namespace moekoe {

// 全局应用上下文（成员按声明顺序构造，按逆序析构）
// 析构顺序设计：被依赖的对象最后析构。
//   config 最先声明 → 最后析构（renderer/tray 等模块可能在其析构期间访问 config）
//   taskbarWindow 须在 renderer 之后声明 → 先于 renderer 析构（释放对 renderer 的引用）
struct AppContext {
    HINSTANCE                hInstance{nullptr};
    HWND                     hwnd{nullptr};
    bool                     running{true};

    // 动态卡片宽度缩回滞回计时器
    ULONGLONG                shrinkStartTick_{0};

    // debug 模式性能监控计数器（仅当 config.debugLog 时启用）
    // 每隔 PERF_STATS_INTERVAL_MS 毫秒汇总一次并写入 debug.log
    ULONGLONG                perfLastTick_{0};      // 上次统计的时间戳（GetTickCount64）
    UINT                     perfFrameCount_{0};    // 区间内 WM_TIMER 触发次数
    std::atomic<UINT>        perfWsMsgCount_{0};    // 区间内 OnLyrics 回调次数（跨线程累加）

    // RAII 管理的组件（按声明顺序构造，按逆序析构）
    std::unique_ptr<Config>               config;          // 最先声明，最后析构
    std::unique_ptr<NativeMessagingHost>  nativeHost;
    std::unique_ptr<D2DSettingsWindow>    d2dSettingsWindow;
    std::unique_ptr<HttpServer>           httpServer;
    std::unique_ptr<WebSocketClient>      wsClient;
    std::unique_ptr<LyricsParser>         parser;
    std::unique_ptr<SpectrumCapture>     spectrumCapture;
    std::unique_ptr<TaskbarRenderer>      renderer;        // 依赖 taskbarWindow 的 HWND
    std::unique_ptr<TaskbarWindow>        taskbarWindow;   // 先于 renderer 析构
    std::unique_ptr<TrayIcon>            tray;            // 最后声明，最先析构
};

// 工具: 把 UTF-8 字符串限制到 Windows Tooltip 长度
std::wstring ToTooltipWide(const std::string& s);

// 应用渲染器配置（直接传递 AppearanceConfig，无需逐字段拷贝）
void ApplyRendererSettings(AppContext& app);

// 帧定时器处理：从 parser 取渲染状态并驱动 renderer
// 从 MsgWndProc 的 WM_TIMER / WM_FRAME_TICK 分支提取
void HandleFrameTick(AppContext& app);

// 悬停/拖动重绘请求处理（WM_RENDER_UPDATE）
void HandleRenderUpdate(AppContext& app);

} // namespace moekoe
