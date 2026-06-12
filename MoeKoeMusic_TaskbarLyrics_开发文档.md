# MoeKoeMusic 任务栏歌词插件 — 开发文档

> **版本：** v0.5（草案）\
> **插件版本：** v0.4.0\
> **目标平台：** Windows 10/11（x64）\
> **开发语言：** C++17\
> **构建系统：** CMake + MSVC\
> **协议：** GPL-2.0（继承自 MoeKoeMusic）

***

## 目录

1. [项目概述](#1-项目概述)
2. [技术架构](#2-技术架构)
3. [模块设计](#3-模块设计)
4. [协议与接口](#4-协议与接口)
5. [构建与部署](#5-构建与部署)
6. [扩展与维护](#6-扩展与维护)
7. [附录](#7-附录)

***

## 1. 项目概述

### 1.1 背景

MoeKoeMusic 是一款基于 Electron + Vue 3 的开源音乐播放器，其 Windows 版本使用 Electron 构建桌面界面。原版软件提供了桌面歌词窗口，**但缺少任务栏歌词显示功能**。

### 1.2 目标

开发一个**独立的任务栏歌词插件**，将歌词以**浮动窗口覆盖在任务栏上方**的方式显示，实现与系统 UI 融合的歌词显示体验。

> **注意：** 原文档规划使用 `SetParent` 将歌词窗口嵌入为任务栏子窗口，但由于 Win11 任务栏结构变化导致该方案不稳定，实际采用独立浮动窗口 + WS\_EX\_TOPMOST 方案（类似 TranslucentTB、TrafficMonitor 等成熟项目）。

### 1.3 设计原则

| 原则        | 说明                                     |
| --------- | -------------------------------------- |
| **零侵入**   | 不修改 MoeKoeMusic 本体任何文件，独立 EXE 运行       |
| **独立维护**  | 插件可脱离主程序版本独立迭代                         |
| **轻量高效**  | CPU 占用 < 2%，内存占用 < 20MB                |
| **用户友好**  | 即开即用，系统托盘右键菜单控制锁定/设置/退出，WebView2 GUI 设置界面 |
| **覆盖任务栏** | 独立浮动窗口 + WS\_EX\_TOPMOST，视觉上与任务栏融合     |

### 1.4 数据获取策略

采用 **WebSocket 监听**（端口 6520）获取歌词和播放状态数据，无需内存 Hook 或文件嗅探。

| 方案                    | 可行性              | 维护成本 | 推荐度 |
| --------------------- | ---------------- | ---- | --- |
| ✅ WebSocket 监听端口 6520 | MoeKoeMusic 原生推送 | 低    | ⭐⭐⭐ |
| ⬜ 歌词缓存文件读取            | 项目无独立缓存文件        | 中    | ⭐⭐  |
| ❌ 内存 Hook             | 兼容性差、易崩溃         | 高    | ⭐   |

### 1.5 项目结构

```
MoeKoeMusic-TaskbarLyrics/
├── CMakeLists.txt              # CMake 构建配置
├── README.md                   # 插件说明
├── src/
│   ├── constants.h             # 全局命名常量（端口、尺寸、消息号、UI参数、跑马灯参数、鉴权Token）
│   ├── main.cpp                # 程序入口（WinMain，含 NativeMessagingHost 后台线程）
│   ├── native_messaging.h/cpp  # Native Host JSON Lines 协议层（v0.4.0 新增）
│   ├── websocket_client.cpp/h  # WebSocket 数据接收（ixwebsocket）+ API 自动开启集成
│   ├── http_server.cpp/h       # HTTP 服务器（端口可配置，默认6523，含本地Token鉴权）
│   ├── lyrics_parser.cpp/h     # 歌词 JSON 解析 & LRC/KRC 同步
│   ├── taskbar_window.cpp/h    # 任务栏浮动窗口管理 + Z-order 三重防护 + APPBAR 自动隐藏适配 + 位置锁定
│   ├── renderer.cpp/h          # Direct2D 渲染引擎 + 跑马灯状态机
│   ├── config.cpp/h            # 配置管理（JSON + 注册表自启，含httpServerPort）
│   ├── api_enabler.cpp/h       # MoeKoeMusic API 模式自动检测与开启（v0.3.1 新增）
│   ├── tray_icon.cpp/h         # 系统托盘图标+菜单（含锁定位置/完全锁定）
│   ├── logger.cpp/h            # 统一日志系统（v0.3.8 新增，替代 6 处分散 DebugLog）
│   ├── settings_window.cpp/h   # WebView2 设置窗口（含 COM 回调）
│   ├── config_dialog.cpp/h     # Win32 回退设置对话框
│   └── app_icon.rc             # EXE 图标资源
├── resources/
│   ├── icon.ico                # 托盘/程序图标
│   ├── settings.html           # WebView2 设置页面（含跑马灯控制 UI）
│   └── icon.png                # Chrome Extension 图标
├── scripts/
│   └── package.ps1             # 发布打包脚本（生成符合 MoeKoeMusic-Plugins 审核规范的 zip）
├── public/                     # 发布包根目录（package.ps1 自动生成）
│   └── icons/
│       └── icon256.png         # 插件中心图标（raw GitHub URL 可直接访问）
├── moeKoe-taskbar-lyrics/      # Chrome Extension 插件目录
│   ├── manifest.json           # v0.4.0（moekoe_native_hosts 声明，minversion: "1.6.6"）
│   ├── background.js           # Service Worker（消息路由 + Native Host Bridge Port 管理）
│   ├── popup.js / popup.html   # 弹出界面（Native Host 状态查询 / WebSocket 连接状态）
│   ├── native-bridge.html      # 隐藏桥接页壳（v0.4.0 新增）
│   ├── native-bridge.js        # Bridge 逻辑：chrome.runtime.Port ↔ electronAPI.nativeHost（v0.4.0 新增）
│   └── icons/
│       └── icon256.png         # Extension 内部图标引用
```

***

## 2. 技术架构

### 2.1 整体架构图（v0.4.0 Native Host 托管模式）

```
┌──────────────────────────────────────────────────────────────┐
│                      MoeKoeMusic 主程序                      │
│  ┌────────────┐    ┌──────────────────┐    ┌──────────────┐  │
│  │  Vue 前端   │───▶│ IPC (主进程)      │───▶│ KuGou API    │  │
│  │ (歌词状态)  │    │                  │    │ (HTTP :6521) │  │
│  └─────┬──────┘    └────────┬─────────┘    └──────────────┘  │
│        │                    │                                 │
│        ▼                    ▼                                 │
│  ┌──────────────────────────────────────────────────────┐     │
│  │              WebSocket Server (:6520)                 │     │
│  │  推送: {type:"lyrics", data:[...]}                   │     │
│  │  推送: {type:"playerState", data:{...}}              │     │
│  └──────────────────────┬───────────────────────────────┘     │
│                           │                                   │
│  ┌────────────────────────────────────────────────────┐      │
│  │         Native Host Manager (nativeHostManager.js)   │      │
│  │  spawn EXE / stdin(stdout JSON Lines) / shutdown     │      │
│  │  auto_start / authorization / treeKill               │      │
│  └────────────────────────┬───────────────────────────┘      │
└───────────────────────────┼─────────────────────────────────────┘
          stdin/stdout (JSON Lines)    chrome-extension:// Bridge
          ▼                            ▼
┌─────────────────────────────────────────────────────────────┐
│                任务栏歌词插件 (MoeKoeTaskbarLyrics.exe)       │
│                                                             │
│  ┌─────────────────────┐    ┌──────────────────┐            │
│  │ NativeMessagingHost │◀──▶│ 歌词解析器        │            │
│  │ (stdin/stdout JSON   │    │ (JSON→时间轴)    │            │
│  │  Lines 协议层)       │    └────────┬─────────┘            │
│  └─────────────────────┘             │ 当前歌词 + 进度      │
│                                     ▼                        │
│  ┌──────────────────────────────────────────────────┐      │
│  │              Direct2D 渲染引擎                     │      │
│  │   逐帧绘制文字 → WIC Bitmap → UpdateLayeredWindow  │      │
│  └──────────────────────┬───────────────────────────┘      │
│                         │                                   │
│                         ▼                                   │
│  ┌──────────────────────────────────────────────────┐      │
│  │     浮动歌词窗口 (WS_EX_TOPMOST + WS_EX_LAYERED)    │      │
│  └──────────────────────────────────────────────────┘      │
│                                                             │
│  ┌──────────────────────┐                                   │
│  │ 系统托盘图标 + 右键菜单│  锁定位置/完全锁定/开机自启/设置/重连/退出       │
│  └──────────────────────┘                                   │
│                                                             │
│  ┌────────────────────────────────────────┐                │
│  │ HTTP Server (:6523)                     │ ← 独立模式回退 │
│  │ WebSocket Client (:6520)                │ ← 歌词数据获取  │
│  └────────────────────────────────────────┘                │
│                                                             │
│  ┌────────────────────────────────────────┐                │
│  │ WebView2 Settings Window                │                │
│  └────────────────────────────────────────┘                │
└─────────────────────────────────────────────────────────────┘

插件通信链路（v0.4.0）:
popup.js → background.js → chrome.runtime.Port → native-bridge.js
    → electronAPI.nativeHost → Electron Main Process → EXE stdin (JSON Lines)
EXE stdout (JSON Lines) → Electron Main Process → native-host-message IPC
    → native-bridge.js → Port → background.js → popup.js
```

### 2.2 核心技术栈

| 层级      | 技术                             | 用途                                  |
| ------- | ------------------------------ | ----------------------------------- |
| 窗口系统    | Win32 API                      | 创建浮动窗口、消息循环、DPI 感知                  |
| 图形渲染    | **Direct2D** + **DirectWrite** | GPU 加速的文字渲染（WIC BitmapRenderTarget） |
| 内嵌浏览器   | **WebView2** (Edge Chromium)   | GUI 设置界面（可回退 Win32 对话框）             |
| 通信协议    | **WebSocket**（RFC 6455）        | 从 MoeKoeMusic 获取实时数据                |
| 托管通信    | **JSON Lines** (stdin/stdout)     | Native Host 与 MoeKoeMusic 主进程通信（v0.4.0） |
| 回退通信    | **HTTP** (:6523)               | 独立模式下 Chrome Extension popup.js 通信        |
| Bridge 通信  | **chrome.runtime.Port**          | background.js ↔ native-bridge.js 双向长连接（v0.4.0）|
| 网络库     | **ixwebsocket**                | 轻量 C++ WebSocket 客户端库               |
| JSON 解析 | **nlohmann/json**              | 配置文件 / WS 消息解析 / Native Host 协议       |
| 配置持久化   | JSON 文件 + Windows 注册表          | 用户首选项 + 开机自启                        |
| 系统托盘    | Win32 Shell API                | 托盘图标 + 右键菜单                         |
| 常量管理    | **constants.h**                | 集中管理所有魔数                            |

### 2.3 关键性能目标

| 指标             | 目标值                                                  |
| -------------- | ---------------------------------------------------- |
| CPU 占用 (空闲/播放) | < 0.5% / < 2%                                        |
| 内存占用           | < 20 MB                                              |
| 帧率 (歌词滚动)      | 60 FPS 可配置（最高 120 FPS，MIN\_FRAME\_INTERVAL\_MS=15ms） |
| 启动延迟           | < 500 ms（从启动到显示歌词）                                   |
| 渲染延迟           | < 5 ms（从数据到画面）                                       |

### 2.4 全局常量体系 (`constants.h`)

所有魔数集中定义在 `src/constants.h`，使用 `namespace moekoe::constants` 组织：

| 分类  | 常量名                         | 值      | 用途            |
| --- | --------------------------- | ------ | ------------- |
| 端口  | `WEBSOCKET_LISTEN_PORT`     | 6520   | WS 歌词数据       |
| 端口  | `HTTP_SERVER_PORT`          | 6523   | Extension 通信（默认值，可通过 config 覆盖） |
| 鉴权  | `LOCAL_AUTH_TOKEN`           | —      | 本地共享密钥（`X-MoeKoe-Token` 头值） |
| 鉴权  | `LOCAL_AUTH_HEADER_NAME`     | `X-MoeKoe-Token` | HTTP 鉴权头名称 |
| 渲染  | `MIN_FRAME_INTERVAL_MS`     | 15     | 最小帧间隔         |
| 尺寸  | `LYRIC_HEIGHT_BASE_DP`      | 28     | 歌词高度(96DPI基准) |
| 尺寸  | `MAX_LYRIC_WIDTH_BASE_DP`   | 360    | 歌词最大宽度        |
| UI  | `TEXT_PADDING_X`            | 20.0f  | 文本左右内边距       |
| UI  | `BUTTON_SPACING`            | 2.0f   | 控制按钮间距        |
| 消息号 | `WM_TRAY_CALLBACK`          | 0x0600 | 托盘回调          |
| 消息号 | `WM_RENDER_UPDATE`          | 0x0700 | 渲染更新请求        |
| 消息号 | `WM_PROCESS_EXITED`         | 0x0800 | 进程退出通知        |
| 安全  | `MAX_WS_MESSAGE_SIZE`       | 1MB    | WS 消息大小上限     |
| 系统  | `WINDOWS_TOOLTIP_MAX_LEN`   | 127    | Tooltip 最大长度  |
| 跑马灯 | `MARQUEE_DELAY_MS`          | 2000   | 滚动前延迟（ms）     |
| 跑马灯 | `MARQUEE_PAUSE_MS`          | 1000   | 滚动后暂停（ms）     |
| 跑马灯 | `MARQUEE_SPEED_PX_PER_SEC`  | 40     | 默认滚动速度（px/s）  |
| 跑马灯 | `MARQUEE_SPEEDUP_THRESHOLD` | 2.0f   | 超长歌词加速阈值（倍数）  |

***

## 3. 模块设计

### 3.1 歌词数据获取模块

**文件：** `src/websocket_client.cpp/h`

#### 职责

- 连接 MoeKoeMusic 的 WebSocket 服务器（`ws://127.0.0.1:6520`）
- 接收并分发 `lyrics` 和 `playerState` 消息
- 自动重连（断线检测 + 指数退避，使用 constants.h 常量）
- 发送控制指令（toggle/next/prev）

#### 关键实现细节

- **协作式线程退出**：`Disconnect()` 设置 `stopRequested_ = true` 后调用 `reconnectThread_.join()`，不再使用 TerminateThread
- **已连接状态轮询间隔**：50ms（`WS_CONNECTED_POLL_MS`），快速响应停止请求
- **消息大小限制**：`DispatchWsMessage()` 入口检查 `raw.size() > MAX_WS_MESSAGE_SIZE`(1MB)，超限丢弃并记录日志
- **KRC 格式解析**：MoeKoeMusic 实际推送 KRC 格式字符串，在 `ParseKrc()` 中内联实现

#### 重连策略（constants.h 定义）

| 尝试次数   | 等待时间（粒度 × 次数）             |
| ------ | ------------------------- |
| 第 1 次  | 1 × 100ms = 100ms（连接超时5s） |
| 第 2 次  | 2 × 100ms = 200ms         |
| ...    | ...                       |
| 第 5+ 次 | 15 秒（上限）                  |

### 3.2 HTTP 服务器模块

**文件：** `src/http_server.cpp/h`

#### 职责

- 监听端口（可配置，默认 6523），提供 Chrome Extension popup.js 通信接口
- **本地 Token 鉴权**：所有非 OPTIONS 请求需携带 `X-MoeKoe-Token` 自定义头，通过 `CheckLocalAuthToken()` 校验
- **OPTIONS 预检处理**：浏览器 CORS 预检请求（不带自定义头）自动跳过鉴权，返回 `204 No Content`
- **CORS 动态端口**：`Access-Control-Allow-Origin` 使用运行时实际监听端口，支持用户自定义端口后仍能正常跨域
- **确定性响应体**：ping 返回 `{service,status}` JSON，shutdown 返回 `{status:"shutting_down"}` JSON
- 静态文件服务（settings.html、icon.png 等）
- 协作式线程退出（同 WebSocket 客户端模式）

#### 鉴权流程

```
收到 HTTP 请求
    │
    ├─ method == "OPTIONS"？
    │   └─ 是 → 跳过鉴权 → 返回 204 No Content + CORS 头
    │
    └─ 否 → CheckLocalAuthToken(client, buffer, len)
        │
        ├─ X-MoeKoe-Token 头缺失或不匹配？
        │   └─ 是 → 返回 403 + 关闭 socket
        │
        └─ 否 → 继续路由处理（GET /ping / POST / ...）
```

#### 端口配置

HTTP 监听端口从 `config.json` 的 `advanced.http_server_port` 字段读取（默认值取自 `constants.h` 的 `HTTP_SERVER_PORT = 6523`）。`SendResponse()` 函数签名包含 `int port` 参数，确保 CORS `Allow-Origin` 始终与实际监听端口一致。

### 3.3 任务栏浮动窗口模块

**文件：** `src/taskbar_window.cpp/h`

#### 3.3.1 职责

- 查找 Windows 任务栏窗口句柄（`Shell_TrayWnd`）
- 在任务栏上方创建**独立浮动 Layered Window**
- 处理窗口消息（`WM_DPICHANGED`、`WM_SETTINGCHANGE` 等）
- 监听任务栏变化并自适应调整
- **APPBAR 自动隐藏适配**：检测 `ABS_AUTOHIDE` 状态，歌词窗口跟随任务栏显隐
- 鼠标悬停检测 + 控制按钮交互
- 拖动调整位置 + 视觉反馈
- **位置锁定 / 完全锁定**：禁止拖动/交互（锁定位置保留按钮操作）

#### 3.3.2 窗口创建方式（实际方案）

```cpp
// 独立浮动窗口（非 SetParent 子窗口）
const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
const DWORD style = WS_POPUP;  // 无 WS_EX_TRANSPARENT（需接收鼠标消息）
HWND hLyrics = CreateWindowEx(exStyle, L"TaskbarLyricsClass",
    L"", style, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
// 通过 PositionWindow() 定位到任务栏上方
```

#### 3.3.3 与 SetParent 方案的关键区别

| 维度          | SetParent 嵌入（原规划）    | 浮动覆盖（实际实现）              |
| ----------- | -------------------- | ----------------------- |
| **父窗口**     | `Shell_TrayWnd`（任务栏） | `nullptr`（顶层窗口）         |
| **定位方式**    | 任务栏客户区坐标             | 屏幕坐标，紧贴任务栏边缘            |
| **Z-order** | 继承任务栏 Z-order        | `WS_EX_TOPMOST` 置顶      |
| **任务栏隐藏**   | 窗口随任务栏一起隐藏           | 需主动监听 WM\_SETTINGCHANGE |
| **鼠标穿透**    | 可选                   | 不使用（需接收鼠标消息）            |
| **稳定性**     | Win11 下不稳定           | ✅ 成熟方案                  |

### 3.4 歌词同步与解析模块

**文件：** `src/lyrics_parser.cpp/h`

#### 数据结构

```cpp
struct CharacterTiming { std::string ch; int64_t startTime; int64_t endTime; };
struct LyricLine { std::string text; std::string translated; std::vector<CharacterTiming> characters; };
struct LyricsData { std::vector<LyricLine> lines; };
struct PlayerState { bool isPlaying; double currentTime; std::string songTitle; };
struct RenderState {
    std::string currentLine, currentTranslated;
    double progress; int currentLineIndex;
    bool isHovering, isDragging;  // 悬停/拖动状态
};
```

#### 核心逻辑

- `UpdateLyrics()` — 线程安全（mutex 保护），更新歌词数据
- `UpdatePlayerState()` — 更新播放状态
- `GetCurrentRenderState()` — 二分查找当前行 + 逐字进度计算
- `ParseLRC()` — LRC 格式备用解析
- `ParseKrc()` — KRC 格式解析（MoeKoeMusic 实际使用的格式）

### 3.5 渲染引擎模块

**文件：** `src/renderer.cpp/h`

#### 3.5.1 职责

- 初始化 Direct2D/DirectWrite/WIC 工厂
- 为 Layered Window 创建 WIC BitmapRenderTarget
- 绘制歌词文本（卡拉 OK 逐字高亮 + PushAxisAlignedClip 裁剪）
- 绘制翻译文本（小号字体居中）
- 绘制悬停控制按钮（⏮ ⏸/▶ ⏭）
- 通过 UpdateLayeredWindow 呈现到屏幕

#### 3.5.2 卡拉 OK 高亮实现（实际方案）

> 文档规划的 SetDrawingEffect 方案未采用。实际使用 **PushAxisAlignedClip 裁剪方案**：
>
> 1. 先绘制整行灰色文字（normalColor）
> 2. 计算当前进度对应的裁剪宽度（基于单个 textLayout 的 metrics + 居中左边缘偏移）
> 3. PushAxisAlignedClip 裁剪后绘制高亮色文字（highlightColor）
> 4. PopAxisAlignedClip 恢复

此方案修复了居中对齐时高亮区域位置错误的 bug。

#### 3.5.3 性能优化

- **按钮文字格式缓存**：`btnFormat_` (IDWriteTextFormat) 作为类成员，Initialize/Resize 时创建一次，不再每帧重复 CreateTextFormat
- **UI 参数常量化**：所有硬编码数值替换为 constants.h 引用（TEXT\_PADDING\_X、BUTTON\_SPACING 等）
- **按需渲染**：仅在歌词或进度变化时触发 UpdateLayeredWindow

#### 3.5.4 异常恢复

WM\_TIMER 渲染循环中捕获异常后：

1. 尝试 Shutdown 当前渲染器
2. 重新 Initialize 渲染器
3. 如果恢复成功继续运行
4. 如果彻底失败则 PostQuitMessage 退出程序

### 3.6 配置管理模块

**文件：** `src/config.cpp/h`

#### 配置项结构

```json
{
  "auto_start": true,
  "appearance": {
    "highlight_color": "#4CC2FF",
    "normal_color": "#333333",
    "normal_opacity": 0.85,
    "font_family": "华文细黑",
    "font_size": 20,
    "enable_karaoke": true,
    "enable_translation": true,
    "enable_marquee": true,
    "marquee_mode": "bounce",
    "marquee_delay_ms": 2000,
    "marquee_pause_ms": 1000,
    "marquee_speed_px_per_sec": 40
  },
  "advanced": {
    "websocket_port": 6520,
    "http_server_port": 6523,
    "refresh_rate_hz": 60,
    "debug_log": false
  },
  "position": { "offset_x": 0, "offset_y": 0, "lock_position": false, "lock_fully": false }
}
```

#### 安全增强

- **范围验证**：Load() 后使用 `std::clamp` 校验所有数值配置项：
  - `normalOpacity`: \[0.0, 1.0]
  - `fontSize`: \[8, 72]
  - `websocketPort`: \[1024, 65535]
  - `refreshRateHz`: \[1, 120]

#### 自启动管理

通过注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\MoeKoeTaskbarLyrics` 管理。包含完整诊断日志输出（RegOpenKeyExW / RegSetValueExW / RegDeleteValueW 各步骤）。

#### 配置文件路径

```
%APPDATA%/MoeKoeTaskbarLyrics/config.json
```

### 3.7 系统托盘模块

**文件：** `src/tray_icon.cpp/h`

#### 菜单结构（独立模式）

```
┌──────────────────────┐
│ 🎵 当前歌词...        │  ← Tooltip 显示当前歌词（截断至 127 字符）
├──────────────────────┤
│ ✅ 开机自动启动       │  ← ID_MENU_AUTOSTART
├──────────────────────┤
│ 重新连接              │  ← ID_MENU_RECONNECT
│ 设置                  │  ← ID_MENU_SETTINGS (WebView2)
├──────────────────────┤
│ ☐ 锁定位置            │  ← ID_MENU_LOCK_POS（禁止拖动，保留按钮）
│ ☐ 完全锁定            │  ← ID_MENU_LOCK_FULL（禁止所有交互）
├──────────────────────┤
│ 退出                  │  ← ID_MENU_EXIT
└──────────────────────┘
```

回调消息号使用 `WM_TRAY_CALLBACK` (0x0600)，定义于 constants.h。

### 3.8 WebView2 设置窗口模块

**文件：** `src/settings_window.cpp/h` + `resources/settings.html`

#### 功能概览

完整的 WebView2 嵌入式设置界面，替代手动编辑 JSON 配置文件和原始 Win32 对话框。

#### 初始化流程

```
CreateWindowEx → WM_NCCREATE(return TRUE)
  → ICoreWebView2Environment_CreateAsync
    → ICoreWebView2Controller_CreateAsync
      → OnControllerReady:
        → 检查 settings.html 存在性
        → put_Bounds + put_IsVisible(TRUE)
        → add_NavigationCompleted → 发送 initConfig
        → add_WebMessageReceived → 处理 saveConfig/fontSelected
```

#### 双向通信协议

| 方向   | 消息类型           | 数据                    |
| ---- | -------------- | --------------------- |
| C→JS | `initConfig`   | 完整配置 JSON             |
| JS→C | `saveConfig`   | 用户修改后的完整配置 JSON       |
| JS→C | `fontSelected` | 字体名称（ChooseFontW 选择后） |

#### 关键技术点

- **字体选择无重入**：通过 `PostMessage(hwnd_, WM_PICK_FONT, ...)` 在 WndProc 中调用 ChooseFontW，避免 WebView2 事件处理器中的模态对话框重入问题
- **UTF-8/UTF-16 转换**：统一使用 `Utf8ToWide()` / `WideToUtf8()` 辅助函数，防止乱码
- **JSON 类型安全**：settings.html 中对 event.data 做 `typeof === 'string'` 检查后再 JSON.parse
- **保存即关闭**：saveConfig 成功后自动 Close() 窗口
- **失败回退**：WebView2 初始化失败时自动回退到 Win32 ConfigDialog

### 3.9 进程监控模块

**文件：** `src/process_monitor.cpp/h`

#### 职责

- 轮询检测目标进程（默认 `MoeKoeMusic.exe`）的启动和退出
- 每 `PROCESS_MONITOR_INTERVAL_MS`(2000ms) 检查一次
- 通过 `WM_PROCESS_EXITED` (0x0800) 消息通知主窗口
- 协作式退出：`running_.store(false)` + `join()`

#### 当前状态

模块代码已完成并可编译，但 main.cpp 中的绑定模式分支尚未接入。目前仅支持独立模式。

### 3.10 主程序入口

**文件：** `src/main.cpp`

#### WinMain 初始化（v0.4.0）

```
阶段1: 系统初始化
  ├── SetProcessDpiAwarenessContext(Per-Monitor V2)
  ├── CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)
  └── SetUnhandledExceptionFilter(全局异常过滤器)

阶段2: 应用初始化
  ├── 单实例检查（Named Mutex）
  ├── Config.Load()
  ├── [STARTUP] AutoStart 日志
  └── TrayIcon 初始化

阶段3: 模块初始化
  ├── TaskbarWindow.Create()
  ├── Renderer.Initialize()
  ├── app.renderer = &renderer  ← 必须在 ApplySettings 之前！
  ├── ApplyRendererSettings(app)
  ├── WebSocketClient.Connect()
  ├── 回调绑定（歌词/状态/连接/悬停/按钮/配置变更）
  └── NativeMessagingHost 启动（v0.4.0 新增，后台线程读取 stdin JSON Lines）

阶段4: 消息循环
  ├── GetMessage / TranslateMessage / DispatchMessage
  ├── WM_TIMER → Render()
  │   ├── try { 正常渲染 }
  │   └── catch → Shutdown + Reinitialize 或 PostQuitMessage
  ├── WM_RENDER_UPDATE → 立即重绘（悬停变化时）
  └── WM_TRAY_CALLBACK → 托盘菜单处理

阶段5: 清理退出
  ├── nativeHost_.Stop()  ← join() 协作式退出（v0.4.0 新增）
  ├── wsClient.Disconnect()  ← join() 协作式退出
  ├── renderer.Shutdown()
  └── taskbarWindow 销毁
```

#### 消息处理（WndProc）

| 消息                           | 处理                              |
| ---------------------------- | ------------------------------- |
| `WM_TIMER`                   | 带异常恢复的渲染循环                      |
| `WM_RENDER_UPDATE` (0x0700)  | 悬停状态变化立即重绘                      |
| `WM_TRAY_CALLBACK` (0x0600)  | 托盘菜单命令分发                        |
| `WM_PICK_FONT`               | ChooseFontW 字体选择（防重入）           |
| `WM_DPICHANGED`              | 重新计算 DPI 并调整窗口                  |
| `WM_SETTINGCHANGE`           | 任务栏可能变化，重新定位                    |
| `WM_DISPLAYCHANGE`           | 分辨率变化，重新定位                      |
| `WM_ACTIVATE`                | Z-order 全状态恢复（激活/失活均断言 TOPMOST） |
| `WM_MOUSEMOVE`               | 悬停检测 + TrackMouseEvent          |
| `WM_MOUSELEAVE`              | 悬停结束                            |
| `WM_LBUTTONDOWN`             | 按钮点击或拖动开始                       |
| `WM_LBUTTONUP`               | 拖动结束，保存位置偏移                     |

#### Z-order 三重防护机制

歌词窗口使用 `WS_EX_TOPMOST` 创建，但 Windows 任务栏（`Shell_TrayWnd`）是系统级 TOPMOST 窗口，优先级更高。点击任务栏按钮或展开托盘时，任务栏会提升到歌词窗口上方。为此实现三重防护：

| 层级         | 触发时机              | 实现位置                                                          | 说明                                                  |
| ---------- | ----------------- | ------------------------------------------------------------- | --------------------------------------------------- |
| **① 创建时**  | `Create()`        | [taskbar\_window.cpp](src/taskbar_window.cpp)                 | `WS_EX_TOPMOST` + `SetWindowPos(HWND_TOPMOST)` 初始置顶 |
| **② 消息响应** | `WM_ACTIVATE`     | [taskbar\_window.cpp](src/taskbar_window.cpp)                 | 无论激活/失活均重新断言 TOPMOST（移除了 WA\_INACTIVE 条件判断）         |
| **③ 定期兜底** | 每 \~30 帧 (\~0.5s) | [taskbar\_window.cpp](src/taskbar_window.cpp) `CheckResize()` | 周期性强制断言 TOPMOST，即使前两层都被绕过也能自动恢复                     |

### 3.10.2 跑马灯状态机（长歌词滚动）

**文件：** `src/renderer.cpp/h`

当歌词文本宽度超过可显示区域宽度时，启动跑马灯滚动动画。

#### 状态机（6 个状态）

```
Idle ──(文本超宽)──▶ Delay(2s) ──▶ ScrollLeft ──▶ PauseRight(1s) ──▶ ScrollRight ──▶ PauseLeft(1s) ──▶ Delay ...
                                        ↑                                    │
                                        └──── bounce 模式：循环 ─────────────┘
                                             loop 模式：直接回到 Delay
```

#### 三种模式

| 模式           | 行为             |
| ------------ | -------------- |
| `bounce`（推荐） | 左右往返滚动，阅读体验自然  |
| `loop`       | 传统跑马灯，到左端后跳回右端 |
| `off`        | 关闭滚动，直接截断      |

#### 超长歌词加速

当歌词宽度 > 可用宽度 × 2 时，自动提高滚动速度（最高 3 倍），确保用户在当前歌词结束前能读完。

#### 高亮跟随滚动

跑马灯滚动时，卡拉 OK 高亮的 `clipRect.left` 同步加上 `textLeft_` 偏移量，确保高亮区域始终与可见文字对齐。

### 3.10.3 API 模式自动开启模块

**文件：** `src/api_enabler.cpp/h` （v0.3.1 新增）

#### 职责

检测 MoeKoeMusic 的 API 模式是否开启。若未开启且 MoeKoeMusic 正在运行，则自动修改其 electron-store 配置文件将 `apiMode` 设为 `"on"`，并可选重启 MoeKoeMusic 使配置立即生效。

#### 工作流程

```
WS 连接失败（第 3 次重试）
    │
    ▼
ApiEnabler::TryEnableApi()
    │
    ├─ FindWindowW + CreateToolhelp32Snapshot 双重检测进程
    │   └─ 不存在 → ProcessNotFound（等待用户启动）
    │
    ├─ 定位 %APPDATA%\moekoemusic\config.json
    │   └─ 不存在 → ConfigNotFound
    │
    ├─ JSON 解析 settings.apiMode
    │   └─ "on" → AlreadyOn
    │
    ├─ 写入 .tmp 临时文件 → MoveFileEx 原子替换（防崩溃损坏）
    │   └─ 失败 → ConfigWriteError
    │
    └─ ShellExecuteW 启动新实例（支持 UAC 提权）
        └─ 成功 → EnabledAndRestarted / 失败 → Enabled（需手动重启）
```

#### 防重复机制

静态标记 `s_attempted` 确保每个运行周期只尝试一次自动开启，避免频繁读写配置文件和重启主程序。

#### 触发时机

集成在 [websocket\_client.cpp](src/websocket_client.cpp) 的 `ReconnectLoop()` 中，当第 3 次连接失败（约已等待 3 秒）时触发，避免首次启动时的正常连接延迟误触。

### 3.11 CMakeLists.txt 构建系统

#### 依赖

| 依赖             | 来源                               | 用途             |
| -------------- | -------------------------------- | -------------- |
| ixwebsocket    | vcpkg                            | WebSocket 客户端  |
| nlohmann\_json | vcpkg                            | JSON 解析        |
| WebView2       | NuGet (`microsoft.web.webview2`) | 设置界面浏览器引擎      |
| zlib           | vcpkg (z.dll)                    | ixwebsocket 依赖 |

#### 构建后操作

- 复制 `resources/icon.ico` → 输出目录
- 复制 `resources/settings.html` → 输出目录
- 复制 `WebView2Loader.dll` → 输出目录
- 复制 `z.dll` → 输出目录（含 find\_library 回退）
- 编译 `app_icon.rc` → 图标资源嵌入 EXE
- 编译 `api_enabler.cpp/h` → API 自动开启模块（v0.3.1 新增）

#### 构建命令

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
# 产物: build/Release/MoeKoeTaskbarLyrics.exe
```

***

## 4. 协议与接口

### 4.1 WebSocket 协议

**地址：** `ws://127.0.0.1:6520`

#### 服务端推送

**歌词数据（JSON 数组格式）：**

```json
{"type":"lyrics","data":[{"text":"你好","translated":"Hello","characters":[{"char":"你","startTime":12345,"endTime":12678},{"char":"好","startTime":12678,"endTime":13000}]}]}
```

**歌词数据（KRC 字符串格式）：**

MoeKoeMusic 也可能推送 KRC 格式的原始字符串，由 `ParseKrc()` 解析。

**播放器状态：**

```json
{"type":"playerState","data":{"isPlaying":true,"currentTime":12.5}}
```

#### 客户端指令

```json
{"type":"control","data":{"command":"toggle"}}
{"type":"control","data":{"command":"next"}}
{"type":"control","data":{"command":"prev"}}
```

#### 安全限制

- 消息大小上限：`MAX_WS_MESSAGE_SIZE` = 1 MB
- 超限消息被丢弃并记录日志

### 4.2 HTTP 接口（Chrome Extension）

**端口：** 可配置（默认 6523，通过 `config.json` 的 `advanced.http_server_port` 设置）

用于 Chrome Extension popup.js 与插件的通信（native\_host 桥接）。

#### 鉴权要求

所有非 OPTIONS 请求必须在 HTTP 头中携带：

```
X-MoeKoe-Token: <LOCAL_AUTH_TOKEN>
```

Token 值定义于 `constants.h` 的 `moekoe::constants::LOCAL_AUTH_TOKEN`。OPTIONS 预检请求自动跳过鉴权（浏览器不带自定义头）。

#### 端点列表

| 方法 | 路径 | 鉴权 | 响应 | 说明 |
|------|------|------|------|------|
| `GET` | `/ping` | 需要 | `200 {"status":"ok","service":"MoeKoeTaskbarLyrics"}` | 存活检测 |
| `POST` | `/` | 需要 | `200 {"status":"shutting_down"}` / `400 {"error":"..."}` | 关闭命令（body: `{"command":"shutdown"}` / `{"command":"toggle"}` / `{"command":"next"}` / `{"command":"prev"}`） |
| `OPTIONS` | `*` | 跳过 | `204 No Content` | CORS 预检 |
| 其他 | — | 需要 | `404 {"error":"not found"}` | |

> **注意：** popup.js 不再仅凭 `r.ok` 判断成功，而是解析 JSON 响应体校验确定性字段（如 `status === "shutting_down"`）。

### 4.3 WebView2 通信协议

**方向：C++ → JavaScript**

```json
{"type":"initConfig","data":{/* 完整配置对象 */}}
```

**方向：JavaScript → C++**

```json
{"type":"saveConfig","data":{/* 修改后的配置 */}}
{"type":"fontSelected","data":{"fontFamily":"Segoe UI"}}
```

### 4.4 字段格式对照

| 字段                              | 类型     | 示例              | 说明        |
| ------------------------------- | ------ | --------------- | --------- |
| `type`                          | string | `"lyrics"`      | 消息类型      |
| `data[].text`                   | string | `"你好世界"`        | 歌词行文本     |
| `data[].translated`             | string | `"Hello World"` | 翻译文本      |
| `data[].characters[].char`      | string | `"你"`           | 单个字符      |
| `data[].characters[].startTime` | int64  | `12345`         | 开始时间 (ms) |
| `data[].characters[].endTime`   | int64  | `12678`         | 结束时间 (ms) |
| `data.isPlaying`                | bool   | `true`          | 播放状态      |
| `data.currentTime`              | double | `12.5`          | 当前进度 (秒)  |

### 4.5 MoeKoeMusic 端口清单

| 端口   | 用途                           | 协议        | 可配置 |
| ---- | ---------------------------- | --------- | ------ |
| 6520 | WebSocket 服务（歌词 + 播放状态 + 控制） | WebSocket | 是（`websocket_port`） |
| 6521 | KuGou Music API（HTTP REST）   | HTTP      | 否     |
| 6523 | 本插件 HTTP 服务（Extension 通信）    | HTTP      | **是**（`http_server_port`） |

***

## 5. 构建与部署

### 5.1 环境要求

| 工具            | 版本要求                         |
| ------------- | ---------------------------- |
| Windows SDK   | 10.0.20348+ (Windows 11 SDK) |
| Visual Studio | 2022 (v143 工具集)              |
| CMake         | 3.20+                        |
| vcpkg         | 最新版                          |
| .NET SDK      | 6.0+ （NuGet WebView2 还原需要）   |

### 5.2 部署流程

**推荐模式：托管模式（v0.4.0）**

1. 将 `moeKoe-taskbar-lyrics` 目录复制到 MoeKoeMusic 的 `plugins/extensions/` 下
2. 在 MoeKoeMusic 插件管理页找到「任务栏歌词」→ 点击「本地程序授权」
3. 授权后，EXE 将随 MoeKoeMusic 自动启动/关闭

**独立模式（回退）**

1. 将 `MoeKoeTaskbarLyrics.exe` 放置到任意目录
2. 双击运行，插件以独立模式启动
3. 常驻系统托盘，右键菜单控制锁定位置/完全锁定/设置/退出
4. 确保 MoeKoeMusic 已启动并在端口 6520（可配置）提供 WebSocket 服务
5. （可选）通过 `%APPDATA%\MoeKoeTaskbarLyrics\config.json` 的 `advanced.http_server_port` 自定义 HTTP 端口

### 5.2.1 发布打包

使用 `scripts/package.ps1` 生成符合 MoeKoeMusic-Plugins 审核规范的 zip：

```powershell
.\scripts\package.ps1
# 输出: moeKoe-taskbar-lyrics.zip
# 内部结构:
#   public/
#     manifest.json
#     background.js / popup.js / popup.html
#     native-bridge.html / native-bridge.js    ← v0.4.0 新增
#     icons/icon256.png
#     MoeKoeTaskbarLyrics.exe (+ DLLs)
```

> **注意：** 审核工作流会自动在路径前加 `public/` 前缀，因此 `iconUrl` 将生成为：
> `https://raw.githubusercontent.com/Yngu196/TaskbarLyrics/v{版本}/public/icons/icon256.png`

### 5.3 用户控制

所有控制通过系统托盘右键菜单完成：

| 操作           | 效果                               |
| ------------ | -------------------------------- |
| 勾选"锁定位置"       | 禁止拖动歌词窗口，但保留按钮（上一首/暂停/下一首）操作 |
| 勾选"完全锁定"       | 禁止所有鼠标交互（含悬停和按钮），同时隐含锁定位置 |
| 取消勾选"开机自动启动" | 删除注册表 Run key                    |
| 点击"重新连接"     | 断开并重新连接 WebSocket                |
| 点击"设置"       | 打开 WebView2 设置窗口（GUI 配置界面）       |
| 点击"退出"       | 进程退出                             |

### 5.4 卸载

1. 右键托盘图标 → 退出
2. 删除 `MoeKoeTaskbarLyrics.exe` 文件
3. （可选）删除 `%APPDATA%\MoeKoeTaskbarLyrics\` 配置目录

***

## 6. 扩展与维护

### 6.1 兼容性保障

| 场景               | 策略                                           |
| ---------------- | -------------------------------------------- |
| MoeKoeMusic 更新   | 只要 WebSocket 协议不变，插件无需更新                     |
| WebSocket 协议变化   | 在插件中做 JSON 字段存在性检查                           |
| 多显示器             | 使用 `MonitorFromWindow` + `GetMonitorInfo` 定位 |
| 任务栏隐藏/自动隐藏       | WM\_SETTINGCHANGE 监听 + 主动轮询 CheckResize()    |
| 任务栏位置变化          | 主动轮询 + WM\_SETTINGCHANGE 双重检测                |
| Windows 深色/浅色主题  | 可通过设置界面自定义颜色                                 |
| Windows 11 任务栏居中 | 浮动窗口方案天然兼容                                   |

### 6.2 代码质量措施

| 措施                     | 说明                                             |
| ---------------------- | ---------------------------------------------- |
| 常量集中管理 (`constants.h`) | 消除魔数，统一修改入口                                    |
| 协作式线程退出                | 所有后台线程移除 TerminateThread，改用 join() + stop flag |
| WS 消息大小限制              | 1MB 上限防止内存耗尽                                   |
| 配置值范围验证                | std::clamp 校验所有数值配置项                           |
| WM\_TIMER 异常恢复         | 渲染器异常时自动重试，彻底失败则安全退出                           |
| UTF 编码安全转换             | Utf8ToWide/WideToUtf8 统一处理                     |
| 编译警告清理                 | 修复 C4100/C2374/C4189/C2660 等警告                 |
| **本地 Token 鉴权**          | **HTTP 端点 X-MoeKoe-Token 头校验，OPTIONS 预检自动跳过**       |
| **响应体确定性校验**          | **shutdown/ping 返回含确定性字段的 JSON，客户端校验 body**      |
| **CORS 动态端口**            | **Allow-Origin 使用运行时实际端口，支持自定义端口**           |
| **onMessage 兜底**           | **Chrome Extension 消息监听器所有分支显式 sendResponse**        |

### 6.3 未来功能扩展

| 功能                                        | 难度 | 优先级 | 状态                     |
| ----------------------------------------- | -- | --- | ---------------------- |
| 绑定模式接入 main.cpp                           | 低  | ⭐⭐  | ⚠️ ProcessMonitor 待接入  |
| 歌词滚动动画（平滑移动）                              | 中  | ⭐⭐⭐ | ✅ 已完成（跑马灯状态机）          |
| 双行歌词显示（上一行+当前行）                           | 低  | ⭐⭐⭐ | ❌ 未实现                  |
| 自定义字体和颜色（配置界面）                            | 低  | ⭐⭐  | ✅ 已完成                  |
| Chrome Extension 开关 UI                    | 中  | ⭐⭐  | ✅ 已实现                  |
| 歌词搜索（当 WebSocket 无数据时）                    | 高  | ⭐   | ❌ 未实现                  |
| 支持其他播放器（Foobar2000 等）                     | 高  | ⭐   | ❌ 未实现                  |
| API 模式自动开启                                | 中  | ⭐⭐  | ✅ 已完成（api\_enabler 模块） |
| 硬编码颜色提取到 constants.h                      | 低  | ⭐   | 🔜 低优先级                |
| 统一 logging 系统（合并 DebugLog/ConfigDebugLog） | 低  | ⭐   | 🔜 低优先级                |

### 6.4 调试与日志

- 使用项目内部 `DebugLog()` 函数输出调试信息
- 日志文件位置：exe 同目录下 `debug.log`（可移植，不依赖 %TEMP%）
- 可通过配置 `"debug_log": true` 启用详细日志
- 配置模块使用独立的 `ConfigDebugLog()` 函数
- 启动时输出 `[STARTUP] AutoStart=%s` 诊断信息
- 自启注册表操作有完整的步骤诊断日志

***

## 7. 附录

### 7.1 参考文档

| 主题                    | 链接                                                                                                          |
| --------------------- | ----------------------------------------------------------------------------------------------------------- |
| MoeKoeMusic 源码        | <https://github.com/MoeKoeMusic/MoeKoeMusic>                                                                |
| Electron WebSocket 服务 | `electron/services/apiService.js`                                                                           |
| Layered Windows 官方文档  | <https://learn.microsoft.com/en-us/windows/win32/winmsg/layered-windows>                                    |
| Direct2D 官方教程         | <https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-tutorial>                                |
| DirectWrite 文字渲染      | <https://learn.microsoft.com/en-us/windows/win32/directwrite/text-formatting-and-layout>                    |
| High DPI 桌面应用         | <https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows> |
| WebView2 官方文档         | <https://learn.microsoft.com/en-us/microsoft-edge/webview2/>                                                |
| ixwebsocket           | <https://github.com/machinezone/IXWebSocket>                                                                |
| nlohmann/json         | <https://github.com/nlohmann/json>                                                                          |

### 7.2 代码审查

项目经过两轮代码审查：

| 报告              | 内容                                                           |
| --------------- | ------------------------------------------------------------ |
| `代码审查报告.md`     | 基础审查：常量提取、注释规范、日志路径等                                         |
| `代码审查报告_进阶版.md` | 深度审查（评分 7.8/10）：TerminateThread 安全、WS 大小检查、按钮格式缓存、配置验证、异常恢复等 |

高/中优先级建议已全部修复完成。

***

> **本文档为开发草案，将在实现过程中持续更新。**\
> 如有疑问，请参考 MoeKoeMusic 源码仓库或提交 Issue 讨论。

