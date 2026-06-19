// SPDX-License-Identifier: GPL-2.0
// constants.h - 全局命名常量
//
// 目的：消除代码中的魔数，集中管理所有硬编码数值
// 修改常量时只需改此文件，无需在多个源文件中搜索替换
#pragma once

// 注意：不要在此文件包含 <windows.h>！
// <windows.h> 会破坏 Winsock2 的包含顺序（详见 ws2tcpip.h 报错）。

// ═══════════════════════════════════════
// 自定义消息号（WM_USER 基础偏移）
// ═══════════════════════════════════════
// 注意：此处必须使用 #define 宏而非 constexpr/enum，
// 因为 MSVC 不接受嵌套命名空间内的 constexpr/enum 值作为 switch-case 常量表达式。
// Windows 平台 WM_* 消息本身就是宏，此处保持一致。

/// 系统托盘事件回调 — WM_USER(0x0400) + 0x200 = 0x0600
/// 由 TrayIcon 类通过 PostMessage 发送
/// wParam: 托盘图标 ID; lParam: 鼠标事件（WM_LBUTTONDOWN, WM_RBUTTONUP 等）
#define WM_TRAY_CALLBACK   0x0600

/// 实时渲染更新请求 — WM_USER(0x0400) + 0x300 = 0x0700
/// 由 TaskbarWindow 类发送，触发 Render() 调用
#define WM_RENDER_UPDATE    0x0700

/// 绑定模式进程退出通知 — WM_USER(0x0400) + 0x400 = 0x0800
/// 由 ProcessMonitor 发送，表示 MoeKoeMusic.exe 已退出
/// 接收方应自动关闭应用
#define WM_PROCESS_EXITED   0x0800

namespace moekoe::constants {

// ═══════════════════════════════════════
// 网络通信端口
// ═══════════════════════════════════════

/// MoeKoeMusic WebSocket 服务端口（用于接收歌词数据）
/// 参考：MoeKoeMusic 源代码中的 WebSocket 服务器初始化。
/// ⚠️ 作为"默认值"使用；运行时优先从 config.json 读取 advanced.websocket_port。
constexpr int WEBSOCKET_LISTEN_PORT = 6520;

/// 本插件 HTTP 服务器端口（用于 Chrome Extension popup.js 通信）。
/// ⚠️ 作为"默认值"使用；运行时优先从 config.json 读取 advanced.http_server_port。
/// 如需修改，请同时调整 moeKoe-taskbar-lyrics/popup.js 中对 HTTP_PORT 的使用。
constexpr int HTTP_SERVER_PORT = 6523;

/// HTTP / WebSocket 本地鉴权 token（shared-secret）。
/// popup.js / background.js 与 EXE 内的 HTTP 服务器使用同一 token，
/// 在 HTTP 请求头中以 X-MoeKoe-Token: <token> 传递。
/// 目的：阻止其他本地进程在未获 token 的情况下往 HTTP 端口发送 shutdown 等控制命令。
///
/// 注意：此 token 仅为"同源弱鉴权"，不能替代 TLS / 签名；
///       由于两端代码都在同一台机器上，对本地 root 级攻击者无意义。
///
/// Token 现在由 Config::GetAuthToken() 从注册表动态读取（首次自动生成 UUID），
/// 不再硬编码在二进制中。此常量已废弃，仅保留头名称。
/// 旧常量 LOCAL_AUTH_TOKEN 已移除：参见 config.h::Config::GetAuthToken()。

/// HTTP 请求中携带鉴权 token 的头名称
constexpr const char* LOCAL_AUTH_HEADER_NAME = "X-MoeKoe-Token";

// ═══════════════════════════════════════
// 渲染相关常量
// ═══════════════════════════════════════

/// 帧定时器最小间隔（毫秒）
/// 限制原因：过快会导致过度渲染浪费 CPU；过慢会掉帧
/// 计算方式：MAX(MIN_FRAME_INTERVAL_MS, 1000 / 刷新率Hz)
constexpr int MIN_FRAME_INTERVAL_MS = 15;

/// 目标帧率默认值（刷新率，单位 Hz）
/// 推荐值：30 FPS（平衡 CPU 占用和渲染流畅度）
/// 对应 config.h AdvancedConfig::refreshRateHz 的默认值
constexpr int DEFAULT_REFRESH_RATE_HZ = 30;

// ═══════════════════════════════════════
// 歌词窗口尺寸（DPI 缩放前的基础值，96 DPI）
// ═══════════════════════════════════════

/// 歌词区域高度（96 DPI 像素），用于 MulDiv(dpi/96) 缩放
constexpr int LYRIC_HEIGHT_BASE_DP = 28;

/// 歌词窗口最大宽度（96 DPI 像素），水平任务栏使用
constexpr int MAX_LYRIC_WIDTH_BASE_DP = 360;

/// 歌词窗口最小可用宽度（像素），防止窗口过窄
constexpr int MIN_LYRIC_AVAILABLE_WIDTH = 100;

/// 歌词窗口最小总宽度（像素），防止窗口过窄
constexpr int MIN_WINDOW_WIDTH = 50;

/// 垂直任务栏（左/右）的歌词窗口宽度（96 DPI 像素）
constexpr int VERTICAL_TASKBAR_LYRIC_WIDTH_BASE_DP = 180;

// ═══════════════════════════════════════
// 卡片模式（Card Style）尺寸常量
// ═══════════════════════════════════════

/// 卡片模式窗口高度（96 DPI 基础值）—— 双行需要更多高度
constexpr int CARD_HEIGHT_BASE_DP = 42;

/// 卡片模式最小宽度基础值（96 DPI）
constexpr int CARD_MIN_WIDTH_BASE_DP = 180;

/// 卡片封面尺寸（96 DPI）
constexpr int CARD_COVER_SIZE_DP = 34;

/// 卡片封面圆角半径（96 DPI）
constexpr float CARD_COVER_RADIUS_DP = 6.0f;

/// 卡片模式文字阴影偏移 X（像素）
constexpr float CARD_TEXT_SHADOW_OFFSET_X = 0.0f;
/// 卡片模式文字阴影偏移 Y（像素）
constexpr float CARD_TEXT_SHADOW_OFFSET_Y = 1.0f;

// ═══════════════════════════════════════
// UI / 渲染细节
// ═══════════════════════════════════════

/// 文本左右内边距（像素）
constexpr float TEXT_PADDING_X = 20.0f;

/// 翻译文本相对主文本的字号减小量
constexpr float TRANSLATION_FONT_SIZE_DELTA = 3.0f;

/// 悬停控制按钮间距（像素）
constexpr float BUTTON_SPACING = 2.0f;

/// 悬停按钮背景内边距（像素）
constexpr float BUTTON_BG_PADDING_X = 4.0f;
constexpr float BUTTON_BG_PADDING_Y = 2.0f;

/// 悬停按钮背景圆角半径（像素）
constexpr float BUTTON_BG_BORDER_RADIUS = 3.0f;

// ═══════════════════════════════════════
// 跑马灯（长歌词滚动）常量
// ═══════════════════════════════════════

/// 跑马灯默认延迟时间（毫秒）：歌词显示后多久开始滚动
constexpr int MARQUEE_DEFAULT_DELAY_MS = 2000;

/// 跑马灯默认端点暂停时间（毫秒）
constexpr int MARQUEE_DEFAULT_PAUSE_MS = 1000;

/// 跑马灯默认滚动速度（像素/秒）
constexpr float MARQUEE_DEFAULT_SPEED_PX_PER_SEC = 40.0f;

/// 超长歌词判定阈值：文本宽度 > 可用宽度 × 此倍数时自动加速
constexpr float MARQUEE_SPEEDUP_THRESHOLD = 2.0f;

// ═══════════════════════════════════════
// Windows 系统限制
// ═══════════════════════════════════════

/// Windows Tooltip 最大字符数（包含 null terminator）
/// 超过此限制会被系统截断，因此提前截断以保证显示完整
/// 参考：Microsoft 官方文档 WM_SETTEXT
constexpr int WINDOWS_TOOLTIP_MAX_LEN = 127;

// ═══════════════════════════════════════
// 自定义消息号（WM_USER 基础偏移）
// ═══════════════════════════════════════
// 注意：此处必须使用 #define 宏而非 constexpr/enum，
// 因为 MSVC 不接受嵌套命名空间内的 constexpr/enum 值作为 switch-case 常量表达式。
// Windows 平台 WM_* 消息本身就是宏，此处保持一致。

// ═══════════════════════════════════════
// 进程监控
// ═══════════════════════════════════════

/// 进程监控轮询间隔（毫秒）
/// 用于绑定模式检测 MoeKoeMusic.exe 的启动/退出
constexpr int PROCESS_MONITOR_INTERVAL_MS = 2000;

// ═══════════════════════════════════════
// WebSocket 重连策略
// ═══════════════════════════════════════

/// 已连接状态的轮询检查间隔
constexpr int WS_CONNECTED_POLL_MS = 200;

/// 重连退避等待的粒度（每次 sleep 的毫秒数）
constexpr int RECONNECT_WAIT_GRANULARITY_MS = 100;

/// 最大重连尝试次数（之后退避时间封顶为 15 秒）
constexpr int MAX_RECONNECT_ATTEMPTS = 5;

/// 单次连接超时：迭代次数 × 100ms = 总毫秒数
/// 例如 50 次 × 100ms = 5 秒连接窗口
constexpr int WS_CONNECT_TIMEOUT_ITERATIONS = 50;

/// WebSocket 消息最大允许大小（字节）
/// 超过此大小的消息将被丢弃，防止内存耗尽攻击
/// 正常歌词 JSON 数据通常 < 100KB
constexpr size_t MAX_WS_MESSAGE_SIZE = 1024 * 1024; // 1 MB

/// 歌词最大行数限制（防止 DoS）
constexpr size_t MAX_LYRIC_LINES = 10000;

/// 单行歌词最大字符数限制（防止 DoS）
constexpr size_t MAX_CHARS_PER_LINE = 1000;

// ═══════════════════════════════════════
// 线程退出超时
// ═══════════════════════════════════════

/// 后台线程 join 超时时间（毫秒）
/// 超时后使用 TerminateThread 强制终止，避免程序退出卡死
constexpr int THREAD_JOIN_TIMEOUT_MS = 2000;

} // namespace moekoe::constants
