# MoeKoeMusic 任务栏歌词插件 — 开发文档
> **版本：** v1.0.5 | **语言：** C++17 | **平台：** Windows 10/11 (x64) | **协议：** GPL-3.0

***

## 1. 项目概述

### 1.1 背景

MoeKoeMusic 是 Electron + Vue 3 开源音乐播放器，**缺少任务栏歌词显示功能**。本项目为独立 EXE 插件，以浮动窗口覆盖任务栏方式显示歌词。

> **方案变更：** 原规划 `SetParent` 嵌入任务栏子窗口 → 实际采用独立浮动窗口 + `WS_EX_TOPMOST`（与 TranslucentTB 等成熟项目一致）。

### 1.2 设计原则

| 原则    | 说明                  |
| ----- | ------------------- |
| 零侵入   | 不修改 MoeKoeMusic 本体  |
| 轻量高效  | CPU < 2%，内存 < 20MB  |
| 用户友好  | 托盘右键菜单 + D2D 原生设置界面 |
| 覆盖任务栏 | 浮动窗口 + TOPMOST，视觉融合 |

### 1.3 数据获取

采用 **WebSocket 监听**（端口 6520）获取歌词和播放状态，无需内存 Hook。

### 1.4 项目结构

源码按功能拆分为 8 个模块子目录（共 64 个源文件）：

```
MoeKoeMusic-TaskbarLyrics/
├── src/
│   ├── config/                # 配置管理 (2)
│   │   ├── config.h/cpp       #   JSON 配置 + 注册表自启 + 鉴权 Token
│   ├── core/                  # 核心生命周期 (7)
│   │   ├── main.cpp           #   WinMain 入口（五阶段生命周期）
│   │   ├── app_context.h/cpp  #   RAII 组件容器，析构顺序受控
│   │   ├── crash_handler.h/cpp#   全局异常过滤器 + 崩溃日志
│   │   ├── message_window.h/cpp # 隐式消息窗口（托盘/帧定时）
│   │   └── constants.h        #   全局常量（端口/尺寸/消息号/频谱）
│   ├── lyrics/                # 歌词数据与解析 (6)
│   │   ├── lyrics_data.h      #   LyricLine/PlayerState/RenderState 结构
│   │   ├── lyrics_parser.h/cpp#   LRC/KRC 解析 + 二分查找当前行
│   │   └── krc_parser.h/cpp   #   KRC 字符级时间轴 + 翻译提取
│   ├── net/                   # 网络通信 (6)
│   │   ├── websocket_client.h/cpp # WebSocket 接收 (ixwebsocket)
│   │   ├── http_server.h/cpp  #   HTTP 服务 (默认6523, Token鉴权)
│   │   ├── native_messaging.h/cpp # Native Host JSON Lines 协议
│   │   └── api_enabler.h/cpp  #   API 模式自动检测/开启
│   ├── render/                # Direct2D 渲染 (9)
│   │   ├── renderer.h/cpp     #   渲染主循环 + 卡拉OK裁剪
│   │   ├── renderer_utils.h   #   D2D 工具（画刷/文本/位图）
│   │   ├── lyric_renderer.cpp #   卡拉OK逐字裁剪渲染
│   │   ├── card_renderer.cpp  #   卡片模式（毛玻璃封面+双行布局）
│   │   ├── marquee_engine.cpp #   跑马灯滚动状态机
│   │   ├── spring_animation.cpp # 缓动 + 行切换淡入淡出
│   │   └── spectrum_capture.h/cpp # WASAPI 进程级频谱采集 (PIMPL)
│   ├── taskbar/               # 任务栏交互 (8)
│   │   ├── taskbar_window.h/cpp   # 浮动窗口门面类
│   │   ├── taskbar_embedder.h/cpp # Layered 窗口创建/定位/拖动/锁定
│   │   ├── taskbar_geometry.h/cpp # 任务栏几何查询 + UIA 枚举
│   │   ├── fullscreen_detector.h/cpp # 全屏检测（8帧防抖）
│   │   └── shell_companion.h/cpp   # Shell 事件钩子/定位/生命周期
│   ├── ui/                    # 界面 (18)
│   │   ├── d2d_settings_window.h/cpp # Settings 2.0 主窗口（左导航+右内容）
│   │   ├── nav_view.h/cpp     #   左侧导航视图
│   │   ├── settings_page.h/cpp#   页面基类 + 7 个页面
│   │   ├── ui_element.h/cpp   #   UI 元素基类
│   │   ├── ui_elements.h/cpp  #   10 类控件（Toggle/Slider/ColorRow...）
│   │   ├── color_picker.h/cpp #   颜色选择器弹窗 + 预设色板
│   │   ├── color_utils.h      #   颜色转换工具
│   │   ├── settings_draw_utils.h # 设置页绘制工具
│   │   ├── tray_icon.h/cpp    #   系统托盘图标 + 菜单
│   │   ├── tray_commands.h/cpp #  托盘命令分发
│   │   └── config_dialog.h/cpp  # 备用配置对话框
│   └── util/                  # 通用工具 (8)
│       ├── logger.h/cpp       #   统一日志（5级 + 5MB轮转）
│       ├── process_monitor.h/cpp # 绑定模式进程监控
│       ├── environment_check.h/cpp # 系统环境检查（最低 Build 19041）
│       ├── compat_mode.h/cpp  #   安全模式/兼容标志
│       └── diagnostic_exporter.h/cpp # 诊断报告导出
├── moeKoe-taskbar-lyrics/     # Chrome Extension（manifest/background/native-bridge/icons）
├── scripts/pack_zip.py        # 发布打包脚本
├── tests/                     # Catch2 单元测试
│   ├── test_lyrics_parser.cpp
│   └── test_krc_parser.cpp
├── third_party/               # 内置第三方库（httplib / concurrentqueue / ixwebsocket 源码）
├── CMakeLists.txt             # v1.0.5（手动配置 vcpkg，绕过 toolchain）
├── CMakePresets.json          # VS2022 x64-Debug/x64-Release
├── vcpkg.json                 # 依赖声明（ixwebsocket/nlohmann-json/zlib/mbedtls/kissfft）
└── public/icons/icon256.png   # 插件中心图标
```

***

## 2. 技术架构

### 2.1 整体架构

```
┌────────────────── MoeKoeMusic 主程序 ──────────────────┐
│  Vue 前端 → IPC → KuGou API (HTTP:6521)                │
│                    ↓                                    │
│           WebSocket Server (:6520)                      │
│           推送: lyrics / playerState                    │
│                    ↓                                    │
│           Native Host Manager (spawn EXE)               │
└────────────────────┬────────────────────────────────────┘
              stdin/stdout (JSON Lines)
                     ↓
┌───────────── 任务栏歌词插件 ─────────────┐
│  NativeMessagingHost ↔ 歌词解析器         │
│                         ↓                 │
│              Direct2D 渲染引擎            │
│              (文字→WIC Bitmap→窗口)       │
│                         ↓                 │
│         浮动歌词窗口 (TOPMOST+LAYERED)    │
│                                          │
│  托盘图标+菜单 │ HTTP:6523 │ D2D 设置界面   │
└──────────────────────────────────────────┘
         chrome-extension:// Bridge (popup 通信)
```

### 2.2 核心技术栈

| 层级      | 技术                        | 用途                                |
| ------- | ------------------------- | --------------------------------- |
| 窗口系统    | Win32 API                 | 浮动窗口、消息循环、DPI 感知                  |
| 图形渲染    | Direct2D + DirectWrite    | GPU 文字渲染 (WIC BitmapRenderTarget) |
| 通信协议    | WebSocket (RFC 6455)      | 实时数据获取                            |
| 托管通信    | JSON Lines (stdin/stdout) | Native Host 协议                    |
| 回退通信    | HTTP (:6523)              | 独立模式 Extension 通信                 |
| Bridge  | chrome.runtime.Port       | 双向长连接                             |
| 网络库     | ixwebsocket               | 轻量 C++ WebSocket                  |
| JSON 解析 | nlohmann/json             | 配置/消息/协议解析                        |
| 频谱分析    | kissfft                   | 频谱 FFT 变换                         |
| TLS/压缩  | mbedtls + zlib            | ixwebsocket 安全/压缩依赖               |
| 系统集成    | C++/WinRT (`/await /winrt`)| SMTC 媒体键/媒体会话集成                  |
| 音频采集    | WASAPI Process Loopback   | 进程级频谱音频捕获                        |
| 辅助技术    | UIAutomation              | 任务栏几何查询（UIA 枚举）                  |

### 2.3 性能目标

| 指标     | 目标值                     |
| ------ | ----------------------- |
| CPU 占用 | < 0.5% (空闲) / < 2% (播放) |
| 内存     | < 20 MB                 |
| 帧率     | 60 FPS (最高 120 FPS)     |
| 启动延迟   | < 500 ms                |
| 渲染延迟   | < 5 ms                  |

### 2.4 全局常量 (`src/core/constants.h`)

| 分类  | 常量                           | 值              | 用途                |
| --- | ---------------------------- | -------------- | ----------------- |
| 端口  | WEBSOCKET\_LISTEN\_PORT      | 6520           | WS 歌词数据（可覆盖）      |
| 端口  | HTTP\_SERVER\_PORT           | 6523           | Extension 通信(可覆盖) |
| 鉴权  | LOCAL\_AUTH\_HEADER\_NAME    | X-MoeKoe-Token | HTTP 鉴权头          |
| 鉴权  | Config::GetAuthToken()       | 注册表 UUID     | Token 不再硬编码，从注册表读取 |
| 渲染  | MIN\_FRAME\_INTERVAL\_MS     | 15             | 最小帧间隔             |
| 渲染  | DEFAULT\_REFRESH\_RATE\_HZ   | 60             | 默认刷新率             |
| 尺寸  | LYRIC\_HEIGHT\_BASE\_DP      | 28             | 歌词高度(96DPI)       |
| 尺寸  | MAX\_LYRIC\_WIDTH\_BASE\_DP  | 360            | 默认最大宽度            |
| 尺寸  | WINDOW\_WIDTH\_OVERRIDE\_MAX\_DP | 600        | 手动宽度上限            |
| 尺寸  | VERTICAL\_TASKBAR\_LYRIC\_WIDTH\_BASE\_DP | 180 | 垂直任务栏歌词宽度   |
| 卡片  | CARD\_HEIGHT\_BASE\_DP       | 42             | 卡片模式高度            |
| 卡片  | COVER\_SIZE\_DP              | 34             | 封面默认尺寸            |
| 卡片  | COVER\_FADE\_DURATION\_MS    | 350            | 封面淡入时长            |
| 消息号 | WM\_TRAY\_CALLBACK           | 0x0600         | 托盘回调              |
| 消息号 | WM\_RENDER\_UPDATE           | 0x0700         | 渲染更新              |
| 消息号 | WM\_PROCESS\_EXITED          | 0x0800         | 进程退出              |
| 安全  | MAX\_WS\_MESSAGE\_SIZE       | 1MB            | WS 消息上限           |
| 安全  | MAX\_LYRIC\_LINES / MAX\_CHARS\_PER\_LINE | 10000 / 1000 | 防 DoS 歌词限制 |
| 频谱  | SPECTRUM\_NUM\_BANDS         | 32             | 默认频段数             |
| 频谱  | SPECTRUM\_MIN\_FREQ / MAX\_FREQ | 30 / 16000  | 频率映射范围(Hz)       |
| 频谱  | SPECTRUM\_DB\_FLOOR / CEIL   | -62 / -10      | dB 映射范围(dBFS)     |
| 频谱  | SPECTRUM\_SMOOTH\_ATTACK / RELEASE | 0.50 / 0.88 | 上升/下降平滑系数   |
| 设置  | SETTINGS\_WIN\_WIDTH\_BASE\_DP | 720          | 设置窗口基准宽          |
| 设置  | SETTINGS\_NAV\_WIDTH\_BASE\_DP | 180          | 左侧导航宽度            |
| 动画  | LYRIC\_FADE\_DURATION\_MS    | 200            | 行切换淡入淡出时长        |
| 动画  | KARAOKE\_PROGRESS\_SPRING\_STIFFNESS / DAMPING | 120 / 14.0 | 进度弹簧刚度/阻尼 |
| 跑马灯 | MARQUEE\_DELAY/PAUSE\_MS     | 2000/1000      | 滚动延迟/暂停           |
| 跑马灯 | MARQUEE\_SPEED\_PX\_PER\_SEC | 40             | 默认速度              |

***

## 3. 模块设计

### 3.1 歌词数据获取模块

**文件：** `src/net/websocket_client.h/cpp`

- 连接 `ws://127.0.0.1:6520`，接收 `lyrics`/`playerState` 消息
- **重连策略：** 指数退避 + ±30% 随机抖动（jitter），退避上限 15 秒，防多实例同步重连
- **协作式退出：** `Disconnect()` 设置 `stopRequested_` + `join()`，超时（2s）后 `TerminateThread` 兜底
- **消息限制：** 入口检查 `raw.size() > 1MB`，超限丢弃；歌词行数 / 行内字符数分别限 10000 / 1000
- **回调安全：** 回调中检查 `stopRequested_`，防止 detach 后访问悬挂指针
- **API 自动开启：** 第 3 次连接失败时触发 `ApiEnabler::TryEnableApi()`

### 3.2 HTTP 服务器模块

**文件：** `src/net/http_server.h/cpp`

- 端口可配置（默认 6523），仅绑定 `127.0.0.1`，用于 Chrome Extension 通信
- **鉴权流程：**
  ```
  请求 → method == "OPTIONS"? → 跳过鉴权 → 204 No Content
       → CheckLocalAuthToken() → Token 缺失 → 403
       → 路由处理 (GET/ping, POST/lyrics, POST/shutdown 等)
  ```
- **鉴权 Token：** 由 `Config::GetAuthToken()` 从注册表读取（首次自动生成 UUID），不再硬编码
- **CORS 动态端口：** `Allow-Origin` 使用运行时实际端口
- **确定性响应体：** ping → `{status, service}`，shutdown → `{status:"shutting_down"}`

### 3.3 任务栏浮动窗口模块

**文件：** `src/taskbar/taskbar_window.h/cpp`

`TaskbarWindow` 为门面类，组合任务栏交互的多个子组件：

| 子组件                          | 职责                          |
| ----------------------------- | --------------------------- |
| `taskbar_embedder.h/cpp`      | Layered 窗口创建 / 定位 / 拖动 / 锁定 / 全屏隐藏 |
| `shell_companion.h/cpp`       | Shell 事件钩子 / 任务栏定位 / 生命周期管理       |
| `fullscreen_detector.h/cpp`   | 全屏检测（8 帧防抖 + Shell 菜单抑制）          |
| `taskbar_geometry.h/cpp`      | 任务栏几何查询（UIA 枚举 + 方位判断）           |

- 查找 `Shell_TrayWnd`，创建独立浮动 `Layered Window`
- **窗口样式：** `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE` + `WS_POPUP`
- **嵌入方式对比：**

| 维度      | SetParent (原规划) | 浮动覆盖 (实际)       |
| ------- | --------------- | --------------- |
| 父窗口     | Shell\_TrayWnd  | nullptr (顶层)    |
| Z-order | 继承任务栏           | WS\_EX\_TOPMOST |
| 稳定性     | Win11 不稳定       | ✅ 成熟方案          |

- **APPBAR 自动隐藏：** 检测 `ABS_AUTOHIDE` 状态，窗口跟随任务栏显隐（鼠标进出任务栏区域触发）
- **拖动+锁定：** 支持拖动调整位置、锁定位置（保留按钮）、完全锁定（禁止所有交互）
- **全屏隐藏：** 全屏程序启动后自动隐藏歌词窗口，退出全屏恢复

### 3.4 歌词同步与解析模块

**文件：** `src/lyrics/lyrics_parser.h/cpp`、`src/lyrics/lyrics_data.h`、`src/lyrics/krc_parser.h/cpp`

#### 数据结构

```cpp
struct CharacterTiming { string ch; int64_t startTime, endTime; };
struct LyricLine { string text, translated; vector<CharacterTiming> characters; };
struct LyricsData { vector<LyricLine> lines; };
struct PlayerState { bool isPlaying; double currentTime; };
struct RenderState {
    string currentLine, currentTranslated;
    double progress; int currentLineIndex;
    bool isHovering, isDragging;
};
```

#### 核心逻辑

- `UpdateLyrics()`/`UpdatePlayerState()` — mutex 线程安全
- `GetCurrentRenderState()` — 二分查找当前行 + 逐字进度
- `ParseLRC()`/`ParseKrc()` — LRC/KRC 格式解析
- **KRC 解析器**（独立模块 + 单元测试）：支持字符级时间轴 `[00:01.23]` 与翻译提取（`[language:...]` 标签 / 同时间戳配对）

### 3.5 渲染引擎模块

**文件：** `src/render/renderer.h/cpp`，辅助模块：

| 模块                         | 职责                        |
| -------------------------- | ------------------------- |
| `renderer_utils.h`         | D2D 画刷 / 文本布局 / WIC 位图工具 |
| `lyric_renderer.cpp`       | 卡拉 OK 逐字裁剪渲染             |
| `card_renderer.cpp`        | 卡片模式（毛玻璃封面 + 双行布局）     |
| `marquee_engine.cpp`       | 跑马灯滚动状态机                 |
| `spring_animation.cpp`     | 进度弹簧 + 行切换淡入淡出动画        |

- 初始化 D2D/DW/WIC 工厂，通过 `UpdateLayeredWindow` 呈现
- **卡拉 OK 高亮：** PushAxisAlignedClip 裁剪方案（非 SetDrawingEffect）
  1. 绘制灰色文字 → 2. 计算裁剪宽度 → 3. Push Clip 绘制高亮 → 4. Pop Clip
- **歌词进度动画：** `KARAOKE_PROGRESS_SPRING_*` 弹簧物理（刚度 120 / 阻尼 14.0），进度平滑跟随而非跳变
- **行切换动画：** 歌词行切换时 `LYRIC_FADE_DURATION_MS=200` 淡入淡出过渡
- **卡片模式：** 毛玻璃封面图（WIC 缩放 + 高斯模糊）淡入切换（350ms），封面/歌词/按钮分区布局
- **翻译文本：** 小号字体居中对齐
- **悬停控制按钮：** ⏮ ⏸/▶ ⏭ + 半透明背景
- **频谱绘制：** 与频谱捕获数据联动，柱状条 + 平滑系数
- **性能优化：**
  - `btnFormat_` 类成员缓存，避免每帧重建
  - UI 参数常量引用
  - 按需渲染（变化时才 UpdateLayeredWindow）
- **异常恢复：** WM\_TIMER catch → Shutdown + Reinitialize → 失败则 PostQuitMessage

### 3.6 跑马灯状态机

**文件：** `src/render/marquee_engine.cpp`

- **触发条件：** 歌词宽度 > 可用区域宽度
- **状态机：** Idle → Delay(2s) → ScrollLeft → PauseRight(1s) → ScrollRight → PauseLeft(1s) → ...
- **三种模式：**

| 模式     | 行为         |
| ------ | ---------- |
| bounce | 左右往返（推荐）   |
| loop   | 传统跑马灯，跳回右端 |
| off    | 关闭滚动，直接截断  |

- **超长加速：** 宽度 > 2×可用宽度时最高 3 倍速
- **高亮跟随：** `clipRect.left` 同步 `textLeft_` 偏移
- 引擎与渲染解耦，支持按可用宽度 / 歌词宽度独立驱动不同文本段

### 3.7 配置管理模块

**文件：** `src/config/config.h/cpp`

配置拆分为三个子结构：`AppearanceConfig` / `AdvancedConfig` / `PositionConfig`，由 `Config` 类统一持有。

#### 配置结构（示意，含 v1.0.5 新增字段）

```json
{
  "auto_start": true,
  "appearance": {
    "highlight_color": "#4CC2FF", "normal_color": "#333333",
    "normal_opacity": 0.85, "font_family": "华文细黑",
    "font_size": 20, "enable_karaoke": true,
    "enable_translation": true, "enable_marquee": true,
    "marquee_mode": "bounce", "marquee_delay_ms": 2000,
    "marquee_pause_ms": 1000, "marquee_speed_px_per_sec": 40,
    "window_width": 360,
    "cover_scale": 1.0, "cover_roundness": 8,
    "spectrum_enabled": true, "spectrum_mode": "bar",
    "spectrum_bands": 32, "spectrum_band_width": 8,
    "spectrum_gap": 2, "spectrum_min_db": -62, "spectrum_max_db": -10,
    "spectrum_high_freq": 16000, "spectrum_smoothing": 0.8,
    "spectrum_opacity": 1.0, "spectrum_color": "#4CC2FF"
  },
  "advanced": {
    "websocket_port": 6520, "http_server_port": 6523,
    "refresh_rate_hz": 60, "debug_log": false,
    "enable_cover": true, "use_cover_as_background": false,
    "use_cover_as_background_blur": 30
  },
  "position": { "offset_x": 0, "offset_y": 0, "lock_position": false, "lock_fully": false }
}
```

- **范围验证：** `std::clamp` 校验 opacity\[0,1], fontSize\[8,72], port\[1024,65535], refreshRate\[1,120], 频谱 dB / 频率范围
- **配置路径：** `%APPDATA%/MoeKoeTaskbarLyrics/config.json`
- **自启动：** 注册表 `HKCU\...\Run\MoeKoeTaskbarLyrics`
- **鉴权 Token：** 首次启动生成 UUID 写入注册表，`GetAuthToken()` 读取

### 3.8 系统托盘模块

**文件：** `src/ui/tray_icon.h/cpp`、`src/ui/tray_commands.h/cpp`

- 托盘图标 + 右键菜单由 `tray_icon` 构建，命令统一由 `tray_commands` 分发执行

#### 菜单结构

```
┌──────────────────────────┐
│ 🎵 当前歌词...             │ ← Tooltip (截断至127字符)
├──────────────────────────┤
│ ✅ 开机自动启动             │ ← ID_MENU_AUTOSTART
│ 重新连接                   │ ← ID_MENU_RECONNECT
│ 设置 (D2D原生)             │ ← ID_MENU_SETTINGS
│ ☐ 锁定位置                 │ ← ID_MENU_LOCK_POS
│ ☐ 完全锁定                 │ ← ID_MENU_LOCK_FULL
│ 退出                      │ ← ID_MENU_EXIT
└──────────────────────────┘
```

### 3.9 Settings 2.0 设置窗口

**文件：** `src/ui/d2d_settings_window.h/cpp`，配套 `nav_view` / `settings_page` / `ui_elements`

- 纯 D2D + DW 自绘，零外部依赖，无 Win32 原生控件回退
- **布局：** 720×580（基准 DPI）左导航 + 右内容，窗口可缩放
- **七个设置页（左导航）：** 歌词 / 外观 / 频谱 / 窗口 / 行为 / 高级 / 关于
- **控件体系（10 类）：** TextBlock / Button / Toggle / Slider / ComboBox / Card / NavItem / ColorRow / LabelRow / ThemePresets
- **自绘标题栏：** 拖动/关闭/最小化，支持任务栏点击最小化/恢复
- **暗/亮检测：** 注册表读取系统主题，自动切换配色
- **配色方案：** 蓝（默认）/深色/浅色/白色，仿 Visual Studio 风格；各方案含悬停/按下/描边等完整色板
- **圆角体系：** 窗口 / 卡片 / 控件统一圆角绘制（`RoundRect`）
- **歌词位置偏移：** 歌词页支持单行/双行歌词独立偏移，适配多显示器/高 DPI
- **DWM 标题栏跟随：** 系统标题栏颜色随亮暗主题自动切换
- **实时应用：** 配置修改即时生效并持久化，无需重启
- **延迟操作：** PostMessage 异步执行防崩溃
- **老式边框防护：** WM_NCACTIVATE + DwmExtendFrameIntoClientArea 防止遮挡恢复时闪现经典边框

### 3.10 API 自动开启模块

**文件：** `src/net/api_enabler.h/cpp`

- **触发时机：** 启动时主动检测 + WS 第 3 次重连失败
- **工作流程：**
  ```
  检测进程 → 定位 config.json → 读取 apiMode
  → 写入 .tmp → MoveFileEx 原子替换 → ShellExecuteW 重启
  ```
- **防重复：** 静态 `s_attempted` 每周期只尝试一次

### 3.11 主程序入口

**文件：** `src/core/main.cpp`，配合 `src/core/app_context.h/cpp`（RAII 组件容器）

#### 初始化流程

```
阶段1: 系统初始化
  SetProcessDpiAwarenessContext(Per-Monitor V2)
  CoInitializeEx(STA)
  SetUnhandledExceptionFilter

阶段2: 应用初始化
  单实例检查 (Named Mutex) → Config.Load() → TrayIcon 初始化

阶段3: 模块初始化
  TaskbarWindow.Create() → Renderer.Initialize()
  app.renderer = &renderer → ApplyRendererSettings()
  WebSocketClient.Connect() → NativeMessagingHost 启动

阶段4: 消息循环
  WM_TIMER → Render() [try/catch异常恢复]
  WM_RENDER_UPDATE → 立即重绘
  WM_TRAY_CALLBACK → 菜单处理

阶段5: 清理退出
  nativeHost_.Stop() → wsClient.Disconnect() → renderer.Shutdown()
```

- **生命周期：** `AppContext` 以 RAII 持有各组件，析构顺序受控（逆序释放依赖）
- **频谱采集：** 独立于播放状态的 WASAPI loopback 捕获，进程启动即运行
- **线程管理：** HTTP 服务器、Native Host、stdin 监听等线程统一创建/清理

#### Z-order 三重防护

| 层级     | 触发            | 实现                                             |
| ------ | ------------- | ---------------------------------------------- |
| ① 创建时  | `Create()`    | `WS_EX_TOPMOST` + `SetWindowPos(HWND_TOPMOST)` |
| ② 消息响应 | `WM_ACTIVATE` | 激活/失活均断言 TOPMOST                               |
| ③ 定期兜底 | \~30帧         | 周期性强制断言 TOPMOST                                |

### 3.12 统一日志模块

**文件：** `src/util/logger.h/cpp`

- `moekoe::Log(fmt, ...)` + `moekoe::Log(string)` 两种重载
- **5 级日志：** Debug / Info / Warn / Error / Fatal
- **日志轮转：** 单文件上限 5MB，超出自动重命名并新建（多代归档）
- 路径：exe 同级 `debug.log`（可移植）
- 开关：`config.debugLog` 驱动
- 线程安全：`std::mutex` 保护
- **崩溃日志：** `crash_handler` 全局异常过滤器将异常信息写入日志后结束进程

### 3.13 频谱捕获模块

**文件：** `src/render/spectrum_capture.h/cpp`

- **采集方式：** WASAPI Process Loopback（进程级回环采集），PIMPL 隐藏实现细节
- **处理链路：** 音频采集 → 下采样 → FFT（kissfft）→ 频段能量映射 → 平滑（attack/release 非对称系数）
- **频段映射：** 默认 32 频段，30Hz–16kHz 指数分布，dB 映射范围 `[-62, -10]`
- **使用场景：** 纯音乐（无歌词）时以频谱条填充歌词区域
- **生命周期：** 启动即开始捕获，随插件退出释放

### 3.14 系统集成与诊断模块

**文件：** `src/util/environment_check.h/cpp`、`src/util/compat_mode.h/cpp`、`src/util/diagnostic_exporter.h/cpp`

- **环境检查：** 启动时校验系统版本（最低 Build 19041，推荐 19045），版本不满足时给出提示
- **兼容模式：** `CompatFlag` 三种兼容标志，处理旧版系统 / 异常任务栏等环境问题
- **诊断导出：** 收集系统版本、任务栏状态、配置、日志等运行时信息，一键导出为诊断报告，便于 Issue 排查

### 3.15 进程监控与崩溃处理

**文件：** `src/util/process_monitor.h/cpp`、`src/core/crash_handler.h/cpp`、`src/core/message_window.h/cpp`

- **进程监控：** `ProcessMonitor` 监听绑定模式生命周期，主进程退出时联动退出插件
- **崩溃处理：** `crash_handler` 注册全局异常过滤器（`SetUnhandledExceptionFilter`），记录异常地址/代码并写日志
- **消息窗口：** 隐式消息窗口（不可见）承载托盘回调与帧定时器，避免依赖主窗口消息
- **单实例：** Named Mutex 防止多开

### 3.16 渲染辅助模块

**文件：** `src/render/renderer_utils.h`

- 统一封装 D2D 画刷创建、文本布局、WIC 位图加载/缩放/模糊等底层操作
- 供主渲染器、卡片渲染、频谱绘制共用，避免重复代码

***

## 4. 协议与接口

### 4.1 WebSocket 协议

**地址：** `ws://127.0.0.1:6520`

#### 服务端推送

```json
// 歌词数据 (JSON数组)
{"type":"lyrics","data":[{"text":"你好","translated":"Hello",
  "characters":[{"char":"你","startTime":12345,"endTime":12678}]}]}

// 播放器状态
{"type":"playerState","data":{"isPlaying":true,"currentTime":12.5}}
```

#### 客户端指令

```json
{"type":"control","data":{"command":"toggle"}}
{"type":"control","data":{"command":"next"}}
{"type":"control","data":{"command":"prev"}}
```

**安全限制：** 消息上限 1MB，超限丢弃

### 4.2 HTTP 接口

**端口：** 可配置（默认 6523）

| 方法      | 路径    | 鉴权 | 响应                         | 说明      |
| ------- | ----- | -- | -------------------------- | ------- |
| GET     | /ping | 需要 | `{status, service}`        | 存活检测    |
| POST    | /     | 需要 | `{status:"shutting_down"}` | 关闭/控制   |
| OPTIONS | \*    | 跳过 | 204 No Content             | CORS 预检 |
| 其他      | —     | 需要 | 404                        | —       |

**鉴权头：** `X-MoeKoe-Token: <LOCAL_AUTH_TOKEN>`

### 4.3 字段格式

| 字段                         | 类型     | 示例            | 说明       |
| -------------------------- | ------ | ------------- | -------- |
| data\[].text               | string | "你好世界"        | 歌词行文本    |
| data\[].translated         | string | "Hello World" | 翻译文本     |
| data\[].characters\[].char | string | "你"           | 单个字符     |
| data\[].startTime          | int64  | 12345         | 开始时间(ms) |
| data\[].isPlaying          | bool   | true          | 播放状态     |

### 4.4 端口清单

| 端口   | 用途                   | 协议        | 可配置 |
| ---- | -------------------- | --------- | --- |
| 6520 | WebSocket (歌词+状态+控制) | WebSocket | 是   |
| 6521 | KuGou Music API      | HTTP      | 否   |
| 6523 | 本插件 HTTP 服务          | HTTP      | 是   |

***

## 5. 构建与部署

### 5.1 环境要求

| 工具            | 版本          |
| ------------- | ----------- |
| Windows SDK   | 10.0.20348+ |
| Visual Studio | 2022 (v143) |
| CMake         | 3.20+       |
| vcpkg         | 最新版（manifest 模式） |

**依赖（`vcpkg.json` 声明，共 5 项）：** `ixwebsocket`、`nlohmann-json`、`zlib`、`mbedtls`、`kissfft`

### 5.2 构建命令

```bash
# 1. 安装依赖（vcpkg manifest 模式，按 vcpkg.json 自动解析）
vcpkg install

# 2. 配置（CMakeLists 已内联处理 vcpkg 路径，绕过 toolchain；也可用 CMakePresets）
cmake --preset x64-Release       # 或: cmake -B build -S .
# 若 vcpkg 路径不同，需在 CMakeLists 的 VCPKG_ROOT 处调整

# 3. 构建
cmake --build build --config Release

# 4. 单元测试（Catch2）
ctest --test-dir build -C Release

# 产物: build/Release/MoeKoeTaskbarLyrics.exe（含 Release 依赖 DLL 复制）
```

> **说明：** `CMakeLists.txt` 手动配置 vcpkg 依赖路径（不再依赖 `-DCMAKE_TOOLCHAIN_FILE`）；依赖优先走 vcpkg 安装，缺失时 `ixwebsocket` 自动回退为 `third_party` 内置源码编译。

### 5.3 部署流程

**托管模式（推荐）：**

1. 复制 `moeKoe-taskbar-lyrics` 到 MoeKoeMusic `plugins/extensions/`
2. 插件管理页 → 点击「本地程序授权」
3. EXE 随 MoeKoeMusic 自动启动/关闭

**独立模式（回退）：**

1. 双击 `MoeKoeTaskbarLyrics.exe` 运行
2. 托盘右键菜单控制所有功能

### 5.4 发布打包

```bash
python scripts/pack_zip.py moeKoe-taskbar-lyrics/ moeKoe-taskbar-lyrics.zip
# 内部结构: moeKoe-taskbar-lyrics/{manifest.json, *.js, *.html, icons/, *.exe}
```

### 5.5 卸载

1. 托盘图标 → 退出
2. 删除 EXE 所在文件夹
3. （可选）删除 `%APPDATA%\MoeKoeTaskbarLyrics\`

***

## 6. 扩展与维护

### 6.1 兼容性保障

| 场景             | 策略                                      |
| -------------- | --------------------------------------- |
| MoeKoeMusic 更新 | WebSocket 协议不变则无需更新                     |
| 多显示器           | `MonitorFromWindow` + `GetMonitorInfo`  |
| 任务栏自动隐藏        | `WM_SETTINGCHANGE` + 轮询 `CheckResize()` |
| Win11 任务栏居中    | 浮动窗口天然兼容                                |

### 6.2 代码质量

| 措施          | 说明                                    |
| ----------- | ------------------------------------- |
| 常量集中        | `constants.h` 消除魔数                    |
| 协作式退出       | join() + stop flag 替代 TerminateThread |
| 消息大小限制      | 1MB 上限防内存耗尽                           |
| 配置值校验       | `std::clamp` 范围验证                     |
| 异常恢复        | WM\_TIMER catch 自动重试                  |
| UTF 安全转换    | `Utf8ToWide`/`WideToUtf8` 统一处理        |
| 本地 Token 鉴权 | `X-MoeKoe-Token` 头校验                  |
| CORS 动态端口   | Allow-Origin 运行时端口                    |

### 6.3 调试与日志

- 日志位置：exe 同目录 `debug.log`
- 启用方式：配置 `"debug_log": true`
- 启动输出：`[STARTUP] AutoStart=%s` 诊断信息

### 6.4 未来扩展

| 功能      | 难度 | 优先级 | 状态      |
| ------- | -- | --- | ------- |
| 绑定模式接入  | 低  | ⭐⭐  | ✅ 已实现（v1.0.3） |
| 绑定模式增强  | 中  | ⭐⭐  | 🔜 待评估（进程生命周期联动优化） |
| 支持其他播放器 | 高  | ⭐   | ❌ 新立项   |
| 颜色提取到常量 | 低  | ⭐   | 🔜 低优先级 |

***

## 7. 附录

### 7.1 参考文档

| 主题             | 链接                                                                                                    |
| -------------- | ----------------------------------------------------------------------------------------------------- |
| MoeKoeMusic 源码 | <https://github.com/MoeKoeMusic/MoeKoeMusic>                                                          |
| DirectWrite    | <https://learn.microsoft.com/windows/win32/directwrite/text-formatting-and-layout>                    |
| High DPI       | <https://learn.microsoft.com/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows> |
| ixwebsocket    | <https://github.com/machinezone/IXWebSocket>                                                          |
| nlohmann/json  | <https://github.com/nlohmann/json>                                                                    |

