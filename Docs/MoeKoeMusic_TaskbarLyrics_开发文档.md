# MoeKoeMusic 任务栏歌词插件 — 开发文档

> **对应代码版本：** v1.0.8（`CMakeLists.txt` 的 `project(VERSION)`、`constants.h::PLUGIN_VERSION`、插件 `manifest.json` 三者一致；README 的发布徽章指向 v1.0.9 tag，属发布标记与代码版本字段未同步，见 7.2）
> **语言：** C++17 | **平台：** Windows 10/11 (x64 / ARM64) | **协议：** GPL-3.0 | **最后校对：** 2026-11-12

> 本文说明**设计取舍、模块边界与协议契约**；面向使用者和开发者的可执行命令以仓库根目录 [README](../README.md) 为准。两者不一致时，以代码与 README 为准，并请顺手修正本文。

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

无内存 Hook，歌词/播放状态/控制指令走三条本地通道，任一通道失效时其余通道仍可工作：

| 通道 | 端点 | 用途 |
| --- | --- | --- |
| WebSocket 客户端 | `ws://127.0.0.1:6520/moekoe` | 订阅宿主的歌词与播放状态，发送 `control` / `seek` |
| HTTP 服务 | `http://127.0.0.1:6523` | 扩展 `content.js` 推送歌词/封面，popup 探活与 seek 兜底轮询 |
| Native Host | stdin/stdout JSON Lines | 宿主管理 EXE 生命周期、转发 `enableApiMode` / `seek` 事件 |

WebSocket 端点路径随 MoeKoeMusic 版本变化（新版 `/moekoe`，旧版根路径），`WebSocketClient` 在失败重连时交替尝试两种形式，扩展侧 `background.js` 同样按重连次数交替切换，避免单一路径 404 导致永久连不上。

### 1.4 项目结构

源码按功能拆分为 9 个模块子目录。下表只列**稳定存在的角色**，不写文件数量——数量会随迭代失效，需要精确清单时以 `CMakeLists.txt` 的 `SOURCES` / `HEADERS` 为准。

```
MoeKoeMusic-TaskbarLyrics/
├── src/
│   ├── core/                  # main.cpp（五阶段生命周期）、app_context（RAII 容器）、
│   │                          # crash_handler、message_window、constants.h、
│   │                          # single_instance.h/cpp（单实例锁 + 宿主重载接管，见 3.18）
│   ├── config/config.h/cpp    # JSON 配置 + 注册表自启 + 鉴权 Token
│   ├── launcher/launcher.cpp  # 架构感知启动器，固定 x64 编译（见 3.17）
│   ├── lyrics/                # lyrics_data.h / lyrics_parser / krc_parser
│   ├── net/                   # websocket_client / http_server（cpp-httplib）/
│   │                          # native_messaging / api_enabler
│   ├── render/                # renderer + renderer_utils / lyric_renderer / card_renderer /
│   │                          # media_card_renderer / media_card_layout / marquee_engine /
│   │                          # spring_animation / spectrum_capture（WASAPI loopback）
│   ├── taskbar/               # taskbar_window / media_card_window / taskbar_embedder /
│   │                          # taskbar_geometry / fullscreen_detector / shell_companion /
│   │                          # taskbar_selector
│   ├── ui/                    # D2D 设置窗口、托盘图标与命令、控件体系、颜色工具
│   └── util/                  # logger / process_monitor / environment_check /
│                              # compat_mode / diagnostic_exporter
├── moeKoe-taskbar-lyrics/     # MV3 扩展 + 双架构产物目录
│   ├── manifest.json          #   moekoe_native_hosts / auto_start / bridge 声明
│   ├── background.js          #   Service Worker：WS 订阅 + Native Host Bridge 转发
│   ├── popup.* / content.js / audio-bridge.js
│   ├── native-bridge.html/js  #   background ↔ Electron preload ↔ main 的中转
│   ├── MoeKoeTaskbarLyrics.exe # 根目录 x64 Launcher（构建产物）
│   └── x64/  arm64/  icons/  resources/   # 各架构主程序 + 运行时 DLL（构建产物）
├── scripts/
│   ├── bootstrap.ps1          # 干净克隆 → 依赖就位（vcpkg classic + submodule）
│   ├── vcpkg_contract.ps1     # 依赖契约：读 vcpkg.json 的包名 + 唯一一份 12 文件清单
│   ├── check_build_env.ps1    # 只读构建环境自检（与 bootstrap 共用契约模块）
│   ├── copy_vc_runtime.ps1    # CRT 两级来源补齐 + 逐文件 PE 架构校验
│   ├── pack_zip.py / verify_package.py  # 打包 / 双架构完整性 + PE 校验
│   └── build.bat              # 已废弃的独立构建逻辑，仅转发参数给根目录 build.cmd
├── tests/                     # Catch2 v3 单元测试（5 个目标，见 5.4）
│   └── test_lyrics_parser / test_krc_parser / test_spectrum_math /
│       test_http_lifecycle / test_single_instance (.cpp)
├── third_party/               # httplib / concurrentqueue（内置）、ixwebsocket（git submodule）
├── .github/workflows/         # build.yml（push/PR 双架构 + 测试）、release.yml（v* tag 发布）
├── CMakeLists.txt             # 手动定位 vcpkg、triplet 强绑定、POST_BUILD CRT、测试目标
├── CMakePresets.json          # VS2022 x64/ARM64 × Debug/Release 四个 preset（schema version 3）
├── build.cmd                  # configure / build / clean / release all 统一入口
├── vcpkg.json                 # 依赖清单唯一来源；构建不使用 manifest 模式（见 5.1）
├── TaskbarLyrics-config.json  # 示例配置
├── .vcpkgroot                 # bootstrap 写下的 vcpkg 根路径（git 忽略，不入库）
└── README.md / Docs/ / icons/ / resources/ / public/ / Samples/
```

***

## 2. 技术架构

### 2.1 整体架构

```
┌──────────────────── MoeKoeMusic 主程序 (Electron) ────────────────────┐
│  Vue 前端 → IPC → apiService                                          │
│                     ├─ WebSocket Server :6520 (/moekoe)                 │
│                     │    推送 lyrics / playerState，接收 control / seek  │
│                     └─ Native Host Manager (spawn / stopAll / tree-kill) │
│  extensions.js 加载 MV3 扩展 → background.js (Service Worker)            │
│                     ├─ WebSocket 订阅 → 转发 popup                       │
│                     └─ chrome.runtime.Port ↔ native-bridge.html/js      │
└───────┬───────────────────────────┬────────────────────────────────────┘
        │ stdin/stdout JSON Lines    │ WS :6520 / HTTP :6523
        ↓                           ↓
┌──────────────── 插件包 moeKoe-taskbar-lyrics/ ────────────────────────┐
│  根目录 MoeKoeTaskbarLyrics.exe  ← x64 Launcher（固定 x64，静态 CRT）     │
│        ↓ 按系统原生架构拉起 x64/ 或 arm64/ 的真实主程序                    │
│  ┌──────────────── 真实 EXE ────────────────┐                         │
│  │ NativeMessagingHost │ WebSocketClient     │                         │
│  │ HttpServer(:6523)   │ LyricsParser/KRC    │                         │
│  │        ↓ 数据合并 ↓                        │                         │
│  │ Direct2D/DWrite 渲染 → WIC Bitmap → 窗口   │                         │
│  │        ↓                                  │                         │
│  │ 浮动歌词窗口 (TOPMOST + LAYERED) + 媒体卡片 │                         │
│  │ 托盘图标 / D2D 设置界面 / WASAPI 频谱采集   │                         │
│  └───────────────────────────────────────────┘                         │
│  popup.js / content.js ↔ HTTP :6523（歌词推送、探活、seek 兜底）          │
└───────────────────────────────────────────────────────────────────────┘
```

> 关键边界：宿主不扫描进程来决定是否启动 EXE，它只看自己维护的 `processes` 映射。因此「抢单实例锁失败后秒退」会被宿主当作正常退出，见 3.18。

### 2.2 核心技术栈

| 层级      | 技术                        | 用途                                |
| ------- | ------------------------- | --------------------------------- |
| 窗口系统    | Win32 API                 | 浮动窗口、消息循环、DPI 感知                  |
| 图形渲染    | Direct2D + DirectWrite    | GPU 文字渲染 (WIC BitmapRenderTarget) |
| 通信协议    | WebSocket (RFC 6455)      | 实时数据获取（ixwebsocket）               |
| 本地 HTTP  | cpp-httplib（header-only） | `:6523` 服务，替代早期手写 HTTP 解析          |
| 托管通信    | JSON Lines (stdin/stdout) | Native Host 协议（非 4 字节长度前缀）         |
| Bridge  | chrome.runtime.Port       | background ↔ native-bridge 双向长连接     |
| JSON 解析 | nlohmann/json             | 配置/消息/协议解析                        |
| 频谱分析    | kissfft                   | 频谱 FFT 变换                         |
| TLS/压缩  | mbedtls + zlib            | ixwebsocket 安全/压缩依赖               |
| 系统集成    | C++/WinRT (`/await /winrt`)| SMTC 媒体键/媒体会话集成                  |
| 音频采集    | WASAPI Process Loopback   | 进程级频谱音频捕获                        |
| 辅助技术    | UIAutomation              | 任务栏几何查询（UIA 枚举）                  |
| 构建系统    | CMake 3.21+ Presets       | VS2022 生成器；schema `version: 3`      |
| 依赖管理    | vcpkg **classic 模式**     | 按 triplet 安装到 `installed/<triplet>` |
| 单元测试    | Catch2 v3.7.1 + CTest     | 5 个测试目标，默认 `BUILD_TESTING=OFF`    |
| CRT 分发   | VS Redist / 官方 vc_redist | app-local DLL，逐文件校验 PE 架构           |

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
| 版本  | PLUGIN\_VERSION              | 1.0.8          | 与 `CMakeLists.txt` 的 project VERSION 同步 |
| 端口  | WEBSOCKET\_LISTEN\_PORT      | 6520           | WS 歌词数据（运行时可被 config 覆盖） |
| 端口  | HTTP\_SERVER\_PORT           | 6523           | Extension 通信（运行时可被 config 覆盖） |
| 鉴权  | LOCAL\_AUTH\_HEADER\_NAME    | X-MoeKoe-Token | HTTP 鉴权头；Token 由 `Config::GetAuthToken()` 从注册表读取（首次生成 UUID） |
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
| 消息号 | WM\_PROCESS\_EXITED          | 0x0800         | 进程退出（绑定模式遗留，见 3.15） |
| 消息号 | WM\_SHOW\_LYRICS\_WINDOW     | 0x0900         | 网络线程 → UI 线程：显示/置顶窗口 |
| 消息号 | WM\_TRAY\_STATUS\_UPDATE     | 0x0A00         | 网络线程 → UI 线程：托盘连接状态 |
| 安全  | MAX\_WS\_MESSAGE\_SIZE       | 1 MB           | WS 消息上限           |
| 安全  | MAX\_LYRIC\_LINES / MAX\_CHARS\_PER\_LINE | 10000 / 1000 | 防 DoS 歌词限制 |
| 安全  | THREAD\_JOIN\_TIMEOUT\_MS    | 2000           | 封面下载 worker 退出等待上限，超时 detach |
| 频谱  | SPECTRUM\_NUM\_BANDS         | 32             | 默认频段数（允许 8–64）    |
| 频谱  | SPECTRUM\_MIN\_FREQ / MAX\_FREQ | 30 / 16000  | 频率映射范围(Hz)       |
| 频谱  | SPECTRUM\_DB\_FLOOR / CEIL   | -62 / -10      | dB 映射范围(dBFS)     |
| 频谱  | SPECTRUM\_SMOOTH\_ATTACK / RELEASE | 0.50 / 0.88 | 上升/下降平滑系数   |
| 设置  | SETTINGS\_WIN\_WIDTH\_BASE\_DP | 720          | 设置窗口基准宽          |
| 设置  | SETTINGS\_NAV\_WIDTH\_BASE\_DP | 180          | 左侧导航宽度            |
| 动画  | LYRIC\_FADE\_DURATION\_MS    | 200            | 行切换淡入淡出时长        |
| 动画  | KARAOKE\_PROGRESS\_SPRING\_STIFFNESS / DAMPING | 120 / 14.0 | 进度弹簧刚度/阻尼 |
| 跑马灯 | MARQUEE\_DEFAULT\_DELAY\_MS / PAUSE\_MS | 2000 / 1000 | 滚动延迟 / 端点暂停 |
| 跑马灯 | MARQUEE\_DEFAULT\_SPEED\_PX\_PER\_SEC | 40      | 默认速度              |
> 以下几项**不在 `constants.h`**，但同属全局约定：`http_server.cpp` 的 `set_payload_max_length(4 MB)`、`native_messaging.cpp` 的单行 4 MB 上限、`environment_check.h` 的 `MIN_SUPPORTED_BUILD = 19041`、`single_instance.h` 的 `kInstanceTakeoverTimeoutMs = 10000` / `kInstanceTakeoverPollMs = 50`（见 3.18）。

***

## 3. 模块设计

### 3.1 歌词数据获取模块

**文件：** `src/net/websocket_client.h/cpp`

- 连接 `ws://127.0.0.1:<websocketPort>/moekoe`，接收 `lyrics` / `playerState` 消息；`ReconnectLoop` 在失败重连时交替去掉/保留 `/moekoe`，兼容新旧宿主端点
- **重连策略：** 退避基数 1/1/2/4/8/15 秒 + ±30% 随机抖动，`MAX_RECONNECT_ATTEMPTS = 5` 后封顶 15 秒，防多实例同步重连
- **协作式退出：** `Disconnect()` 置 `stopRequested_`、令 `aliveToken_` 失效，先 `join()` 重连线程，再把 `ix::WebSocket` 所有权移出锁、交给独立线程异步 `stop()`。**不使用 `TerminateThread`**；`THREAD_JOIN_TIMEOUT_MS` 现在只用于封面下载 worker 的限时等待，超时是 `detach()` 而不是强杀
- **消息限制：** 入口检查 `raw.size() > 1 MB` 丢弃；歌词行数 / 行内字符数分别限 10000 / 1000
- **回调安全：** 回调按值持有 `aliveToken_` 并双检 `stopRequested_`，异步清理线程晚到的回调不会访问已析构对象
- **API 开启不再挂在这里：** 旧版「第 3 次重连失败触发 `TryEnableApi()`」已移除。现在是 `main.cpp` 启动时直接调用 `ApiEnabler::WriteApiMode()`（不依赖扩展链路），并通过 Native Host 的 `enableApiMode` 消息提供第二条入口，见 3.10

### 3.2 HTTP 服务器模块

**文件：** `src/net/http_server.h/cpp`（基于 cpp-httplib，取代早期手写 HTTP 解析）

- 端口可配置（默认 6523），仅绑定 `127.0.0.1`，`SO_EXCLUSIVEADDRUSE`，读写超时 5 秒，请求体上限 4 MB
- **鉴权流程（`pre_routing_handler`）：**
  ```
  OPTIONS *                  → 跳过鉴权 → 204 No Content
  GET /ping                  → 免鉴权 → {"status":"ok","service":...}
  GET /seek/pending          → 免鉴权 → 读后即清的 seek 兜底通道
  其余请求                    → X-MoeKoe-Token 缺失/不符 → 403（恒定时间比较）
                             → 通过 → POST /lyrics、POST /、POST /shutdown、404
  ```
  `/ping` 与 `/seek/pending` 免鉴权是有意的：前者是 popup 在桥接链路失效时的探活回退，后者供 `file://` 页面里的 content script 轮询（拿不到注册表 token）。两者响应都不含敏感数据。
- **鉴权 Token：** `Config::GetAuthToken()` 从注册表读取（首次自动生成 UUID）；检测到 fallback token 时 `Start()` 直接拒绝启动，避免鉴权形同虚设
- **CORS：** 默认 `Allow-Origin: http://127.0.0.1:<实际端口>`；两个免鉴权 GET 放宽为 `*`
- **生命周期：** `Stop()` 把工作线程所有权移出临界区后再 `join()`；`ServerLoop()` 内另起 stopper 线程反复 `svr.stop()` 直到 `listenDone`。这套设计修复了早期「持锁 join + 端口 connect 唤醒」造成的锁序反转死锁，见 5.4

### 3.3 任务栏浮动窗口模块

**文件：** `src/taskbar/taskbar_window.h/cpp`

`TaskbarWindow` 为门面类，组合任务栏交互的多个子组件：

| 子组件                          | 职责                          |
| ----------------------------- | --------------------------- |
| `taskbar_embedder.h/cpp`      | Layered 窗口创建 / 定位 / 拖动 / 锁定 / 全屏隐藏 |
| `shell_companion.h/cpp`       | Shell 事件钩子 / 任务栏定位 / 生命周期管理       |
| `fullscreen_detector.h/cpp`   | 全屏检测（8 帧防抖 + Shell 菜单抑制）          |
| `taskbar_geometry.h/cpp`      | 任务栏几何查询（UIA 枚举 + 方位判断）           |
| `taskbar_selector.h/cpp`      | 多任务栏选择（枚举主/副屏任务栏，按显示器决策切换；支持手动指定锁定某台显示器） |

- 查找 `Shell_TrayWnd`，创建独立浮动 `Layered Window`
- **窗口样式：** `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE` + `WS_POPUP`
- **多任务栏选择：** 每帧由 `CheckResize()` 触发 `UpdateTaskbarSelection()`，`TaskbarSelector` 枚举主任务栏 `Shell_TrayWnd` 与各副屏任务栏 `Shell_SecondaryTrayWnd`（EnumWindows 按类名收集 + MonitorFromWindow 建立显示器映射，1s 节流），按「活动窗口显示器 → 鼠标显示器 → 主任务栏」优先级决策，经 300ms 防抖确认后通过 `SetWindowLongPtr(GWLP_HWNDPARENT)` + `ShellCompanion::Rebind()` 切换绑定；显示器布局变化（`WM_DISPLAYCHANGE`）、休眠唤醒（`WM_POWERBROADCAST`）与 Explorer 重启恢复路径均会重置选择器枚举缓存。支持两种显示位置模式（设置 → 窗口页「显示位置」，仅多显示器时出现）：「自动跟随」（默认，即上述按活动窗口/鼠标决策逻辑）与「手动指定」（`SetManualMode(true)` + `SetManualIndex(n)` 锁定到所选显示器的任务栏，直接返回目标任务栏句柄、不参与自动决策与防抖；目标显示器无任务栏时回退主任务栏）。`displaySelectionMode` / `manualDisplayIndex` 字段经 config 的 Load/Save/Export/Import 持久化，启动时由 `main.cpp` 恢复，设置变更经 `tray_commands` 热更新回调即时生效。
- **嵌入方式对比：**

| 维度      | SetParent (原规划) | 浮动覆盖 (实际)       |
| ------- | --------------- | --------------- |
| 父窗口     | Shell\_TrayWnd  | nullptr (顶层)    |
| Z-order | 继承任务栏           | WS\_EX\_TOPMOST |
| 稳定性     | Win11 不稳定       | ✅ 成熟方案          |

- **APPBAR 自动隐藏：** 检测 `ABS_AUTOHIDE` 状态，窗口跟随任务栏显隐（鼠标进出任务栏区域触发）；**该功能已停止维护**，不保证覆盖新版任务栏行为
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
| `media_card_renderer.cpp` / `media_card_layout.h` | 点击歌词弹出的歌曲卡片绘制与布局 |
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
| loop   | 旧版本配置值，读取后自动回退为 bounce |
| off    | 关闭滚动，直接截断  |

- **超长加速：** 宽度 > 2×可用宽度时最高 3 倍速
- **高亮跟随：** `clipRect.left` 同步 `textLeft_` 偏移
- 引擎与渲染解耦，支持按可用宽度 / 歌词宽度独立驱动不同文本段

### 3.7 配置管理模块

**文件：** `src/config/config.h/cpp`

配置拆分为三个子结构：`AppearanceConfig` / `AdvancedConfig` / `PositionConfig`，由 `Config` 类统一持有。

#### 配置结构（节选示意）

> 下面只示意**字段命名与分组**，不是完整 schema。当前默认值与新增字段以 `src/config/config.h` 的 `AppearanceConfig` / `AdvancedConfig` / `PositionConfig` 为准，可导入的完整示例见根目录 `TaskbarLyrics-config.json`。此后新增的重要字段包括：`display_mode`（karaoke/card）、`card_max_expand_ratio`、`window_width_override`、`translation_mode` / `card_translation_mode`、显示位置模式 `display_selection_mode` + `manual_display_index`、卡片分行垂直偏移、频谱颜色与柱宽。配置与鉴权 Token 分开存储：配置在 `%APPDATA%\MoeKoeTaskbarLyrics\config.json`，Token 在 `HKCU\Software\MoeKoeMusic\TaskbarLyrics\authToken`。

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

### 3.10 API 模式开启模块（极简版）

**文件：** `src/net/api_enabler.h/cpp`

- **职责边界：** 只把 `%APPDATA%\moekoemusic\config.json` 的 `settings.apiMode` 写成 `"on"`。**不检测进程、不 Kill、不重启宿主**；重启由用户在 MoeKoeMusic UI 手动完成（README 亦要求修改 API 模式后重启）
- **触发时机：** ① `main.cpp` 启动阶段直接调用 `WriteApiMode()`，不依赖 `content.js → background → bridge → main` 链路，避免任一环节断裂导致配置永远不被写入；② 扩展用户授权后经 Native Host 的 `{"payload":{"action":"enableApiMode"}}` 触发，并回传 `enableApiMode` 结果事件
- **工作流程：** 读取 JSON → `apiMode` 已是 `on` 则跳过 → 写 `<path>.tmp` → `MoveFileExA(REPLACE_EXISTING | WRITE_THROUGH)` 原子替换 → 失败回退直接写 → 异常统一捕获返回 `false`
- **幂等性：** 靠「已是 on 就跳过」实现；旧版的进程检测、`ShellExecuteW` 重启与 `s_attempted` 静态标志均已移除

### 3.11 主程序入口

**文件：** `src/core/main.cpp`，配合 `src/core/app_context.h/cpp`（RAII 组件容器）

#### 初始化流程（按 `main.cpp` 实际顺序）

```
阶段1: 系统初始化
  InitLogger → CoInitializeEx(STA)（失败即退出）→ SetUnhandledExceptionFilter
  → CheckEnvironment（最低 Build 19041 / 推荐 19045）
  → IsHostManagedLaunch() + AcquireInstanceLock()（命名互斥体，见 3.18）
  → WSAStartup → SetProcessDpiAwarenessContext(Per-Monitor V2)

阶段2: 应用初始化
  RegisterMessageClass → CreateWindowExW(HWND_MESSAGE) 隐式消息窗口
  → AppContext / Config.Load + 日志级别 → ApiEnabler::WriteApiMode
  → 自启动注册表同步 → TrayIcon.Initialize

阶段3: 业务模块初始化
  DetectCompatMode → FindTaskbarHandle（最多 10×500ms）
  → TaskbarWindow.Create + 应用配置（方向/显示模式/宽度/显示位置/偏移/锁定）
  → 初始 SW_HIDE，收到有效歌词后再显示
  → TaskbarRenderer.Initialize + ApplyRendererSettings
  → MediaCardWindow.Create（失败非致命）
  → LyricsParser → SpectrumCapture.Start
  → WebSocketClient.Connect(ws://127.0.0.1:<port>/moekoe) + 三类回调
  → HttpServer.Start(<httpPort>)（启动失败非致命）
  → NativeMessagingHost + stdin 读取线程 → SetTimer(帧定时)

阶段4: 消息循环
  PeekMessageW 轮询 + app.running 原子标志（不用 GetMessageW，关停标志能立即生效）
  WM_TIMER → Render() [try/catch 异常恢复]
  WM_RENDER_UPDATE / WM_TRAY_CALLBACK → 重绘 / 菜单
  WM_SHOW_LYRICS_WINDOW / WM_TRAY_STATUS_UPDATE → 网络线程转交 UI 线程
  空闲时 Sleep(1) 让出 CPU

阶段5: 清理退出（与初始化逆序）
  保存最终位置偏移 + Config.Save
  → nativeHost.RequestShutdown() + CancelSynchronousIo 循环（6×500ms；
    超时则 detach 并故意泄漏 nativeHost，防止线程访问已析构对象）
  → KillTimer → SpectrumCapture.Stop → HttpServer.Stop
  → WebSocketClient.Disconnect → MediaCardWindow.Destroy
  → Renderer.Shutdown → TaskbarWindow.Destroy → TrayIcon.Shutdown
  → DestroyWindow → "[SHUTDOWN] Complete"
```

- **生命周期：** `AppContext` 以 RAII 持有各组件，析构顺序受控（逆序释放依赖）
- **频谱采集：** 独立于播放状态的 WASAPI loopback 捕获，进程启动即运行
- **线程管理：** HTTP 服务、Native Host stdin、WebSocket 重连等线程统一创建/清理；网络线程**禁止直接操作窗口**，一律 `PostMessageW` 到 UI 线程
- **关闭耗时：** 从收到 `shutdown` 到 `[SHUTDOWN] Complete` 实测约 2 秒（HTTP 工作线程收尾 + WebSocket 异步清理）。这正是宿主重载时新 EXE 提前 spawn 的窗口，见 3.18
- **退出触发源：** Native Host `shutdown` 指令、HTTP `shutdown` 命令、托盘/菜单退出，三者都只是把 `app.running` 置 `false`；`PostQuitMessage` 在工作线程中无效，故不用它跨线程停主循环

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

- **进程监控（遗留）：** `ProcessMonitor` 与 `WM_PROCESS_EXITED` 仍保留在源码中，但**当前 `main.cpp` 不再创建它**。托管模式下的退出由 Native Host 的 `shutdown` 指令驱动，独立模式由托盘/HTTP `shutdown` 驱动；不要按旧描述假设「宿主进程退出会自动联动」
- **崩溃处理：** `crash_handler` 注册全局异常过滤器（`SetUnhandledExceptionFilter`），记录异常地址/代码并写日志
- **消息窗口：** 隐式消息窗口（不可见）承载托盘回调与帧定时器，避免依赖主窗口消息
- **单实例：** Named Mutex + 托管启动下的等待接管，见 3.18

### 3.16 渲染辅助模块

**文件：** `src/render/renderer_utils.h`

- 统一封装 D2D 画刷创建、文本布局、WIC 位图加载/缩放/模糊等底层操作
- 供主渲染器、卡片渲染、频谱绘制共用，避免重复代码

### 3.17 架构感知启动器

**文件：** `src/launcher/launcher.cpp`

- **定位：** 发布包根目录的 `MoeKoeTaskbarLyrics.exe` 为启动器（Launcher），真正的插件主程序按架构放在 `x64/` 与 `arm64/` 子目录
- **固定 x64 编译：** 启动器始终以 x64 目标编译（静态链接 CRT，自包含）；ARM64 Windows 内置 x64 仿真层可运行 x64 程序，因此同一入口在两种架构设备上都可用
- **架构路由：** 启动时检测系统原生架构，原生 ARM64 → 拉起 `arm64/` 主程序，否则 → 拉起 `x64/` 主程序；检测优先 `IsWow64Process2`（其 `nativeMachine` 输出即宿主真实架构），失败回退 `GetNativeSystemInfo`
- **禁用进程环境 API（关键）：** Launcher 固定 x64，在 ARM64 Windows 上经 x64 仿真层运行，因此判断系统架构**必须**用 `IsWow64Process2` 的 `nativeMachine` 或 `GetNativeSystemInfo` 这类返回宿主原生架构的 API；**严禁**用 `GetSystemInfo`、`PROCESSOR_ARCHITECTURE` 环境变量等反映「当前进程环境」的 API——它们在仿真环境下会误报 x64，导致 ARM64 设备被错误地拉起 x64 子程序
- **stdio 透传：** `STARTF_USESTDHANDLES` + `CreateProcessW(bInheritHandles=TRUE)` 把宿主的 stdin/stdout/stderr 管道交给真实程序，再 `WaitForSingleObject(pi.hProcess, INFINITE)` 等待其退出。宿主因此看到的是「一个持续存活到真实程序结束的进程」，这也是 3.18 判定托管启动的依据
- **无降级 fallback：** ARM64 设备上找不到 `arm64/` 目录时直接失败并提示，而非偷偷启动 x64 模拟版，避免用户误判运行架构
- **打包约束：** 因主程序按架构拆分，发布包校验（`verify_package.py::check_structure`）要求根目录 Launcher、`x64/`、`arm64/` 三者必须同时存在，且 `arm64/` 内至少一个 `.exe`

### 3.18 单实例保护与宿主重载接管

**文件：** `src/core/single_instance.h/.cpp`；测试 `tests/test_single_instance.cpp`

**问题背景：** 宿主 `nativeHostManager` 重载插件时先给旧 EXE 发 `{"type":"shutdown"}`，但旧 EXE 收尾约 2 秒；`extensions.js` 的 `restartExtensions()` 未 `await cleanupExtensions()`（且对 Promise 检查 `.success`，恒为 `undefined`），清理与初始化并发，新 EXE 常在旧 EXE 真正退出前被 spawn。旧逻辑遇到 `ERROR_ALREADY_EXISTS` 直接 `return 0`，宿主把这次 `exit(0)` 记作「本地程序已退出」、删掉 `processes` 记录且**不再自动拉起**，最终插件已加载却没有任何 EXE 存活。

> 宿主并不是扫 OS 进程列表来决定是否启动 EXE，`startHost()` 只看自己维护的 `processes` 映射。所以「抢锁失败后秒退」本身就是故障点。

| 启动方式 | 识别依据 | 遇到重复实例 |
| --- | --- | --- |
| 独立运行 | stdin 与 stdout **不同时**为 `FILE_TYPE_PIPE` | 保持原语义：秒退 `exit(0)` |
| 宿主托管 | 两者同时为管道（宿主 `spawn(..., stdio:['pipe','pipe','pipe'])` + Launcher 透传） | 轮询 `CreateMutexW` 等旧实例释放名字，成功后接管并继续启动 |

- **为什么轮询互斥体就够：** 命名内核对象在最后一个句柄关闭（旧进程完全退出）时销毁，重试成功即精确接管，不需要 PID 共享、跨进程事件或轮询进程列表。**每次探测必须立刻 `CloseHandle`**，否则自己的引用会让名字永不消失（测试专门覆盖这一点）。
- **参数：** 等待上限 10 秒、轮询 50 毫秒；成功时故意不关闭句柄，由进程退出时内核统一回收。
- **超时策略：** 超时**仍继续启动并持有句柄**，只记 `LogWarn`——直接退出会重现「exit(0) 被注销记录」。代价如实说明：极端情况下两实例短暂共存，争抢任务栏窗口与 6523 端口（HTTP 启动失败是非致命的）。
- **测试覆盖：** 首次获取与释放、独立重复秒退、等待接管、超时仍继续、托管识别安全调用，以及一个跨进程用例——测试 exe 以 `--hold` 启动只占锁的 helper 子进程，helper 不显式 `CloseHandle` 直接退出，父进程等真实进程消失后成功接管。
- **残留边界（插件侧拦不住）：** 宿主 `stopAll()` 若在并发窗口把新进程也纳入遍历，2 秒后的 tree-kill 仍可能杀掉正在等待接管的新进程；`restartExtensions()` 未 await 属 MoeKoeMusic 自身 app.asar 代码，只能提示上游修复。等待期间 stdin 消息会积压在管道里，接管后由 `NativeMessagingHost::Run()` 正常消费。

***

## 4. 协议与接口

### 4.1 WebSocket 协议

**地址：** `ws://127.0.0.1:<websocketPort>/moekoe`（默认 6520）。旧版宿主用根路径，EXE 与 `background.js` 都在重连时交替尝试两种形式；端口可在 `advanced.websocket_port` 覆盖，协议可由扩展 `wsProtocol` 配置为 `wss`（需宿主支持 TLS，默认 `ws`）。

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
{"type":"auth","token":"<扩展生成的随机 token>"}
```

`seek` 指令由媒体卡片进度条拖动触发；`background.js` 连接建立后主动发 `auth`（服务端可选校验）。

**安全限制：** 消息上限 1 MB，超限丢弃；歌词行数 / 行内字符数分别限 10000 / 1000。

### 4.2 HTTP 接口

**端口：** 可配置（默认 6523），仅监听 `127.0.0.1`；请求体上限 4 MB；基于 cpp-httplib。

| 方法 | 路径 | 鉴权 | 响应 | 说明 |
| --- | --- | --- | --- | --- |
| GET | `/ping` | 免 | `{"status":"ok","service":"MoeKoeTaskbarLyrics"}` | popup 在桥接链路失效时的存活探测回退 |
| GET | `/seek/pending` | 免 | `{"pending":true,"position":..,"duration":..,"seq":..}` | seek 兜底轮询，**读后即清** |
| POST | `/lyrics` | 需要 | `{"status":"accepted"}` | 扩展推送歌词/封面/播放状态；空体 400 |
| POST | `/`、`/shutdown` | 需要 | `{"status":"shutting_down"}` | body 含 `command:"shutdown"` 或嵌套 `data.command:"shutdown"`；其余 400 |
| OPTIONS | `*` | 免 | 204 No Content | CORS 预检 |
| 其他 | — | 需要 | 404 `{"error":"not found"}` | — |

**鉴权头：** `X-MoeKoe-Token: <Config::GetAuthToken() 生成的注册表 UUID>`，恒定时间比较；缺失或不匹配均 403。若 Token 退化为内置 fallback 值，`HttpServer::Start()` 直接拒绝启动。

> 两个 GET 免鉴权是刻意的：它们只暴露布尔/数字，且要保证「扩展 SW 休眠、桥接页未重连、`file://` 页面拿不到注册表 token」时仍能工作。

### 4.3 字段格式

| 字段                         | 类型     | 示例            | 说明       |
| -------------------------- | ------ | ------------- | -------- |
| data\[].text               | string | "你好世界"        | 歌词行文本    |
| data\[].translated         | string | "Hello World" | 翻译文本     |
| data\[].characters\[].char | string | "你"           | 单个字符     |
| data\[].startTime          | int64  | 12345         | 开始时间(ms) |
| data\[].isPlaying          | bool   | true          | 播放状态     |

`POST /lyrics` 对封面与歌名字段做多别名兼容（`coverArtUrl` / `pic` / `cover` …、`artist` / `author` …），也接受 `currentSong` 嵌套对象与 KRC 字符串，详见 `main.cpp` 的 `OnLyrics`。

### 4.4 端口清单

| 端口   | 用途                   | 协议        | 可配置 |
| ---- | -------------------- | --------- | --- |
| 6520 | 宿主 WebSocket (歌词+状态+控制) | WebSocket | 是   |
| 6521 | 宿主 KuGou Music API  | HTTP      | 否（由 MoeKoeMusic 主进程提供，**非本插件监听**） |
| 6523 | 本插件 HTTP 服务          | HTTP      | 是   |

### 4.5 Native Host 协议（JSON Lines）

**传输：** stdin/stdout，每行一个 UTF-8 JSON 对象、以 `\n` 结束。**不是** Chrome 原生的「4 字节长度前缀 + JSON」二进制协议。单行上限 4 MB，超限丢弃；非法 JSON 只跳过该行，不终止读取循环。

| 方向 | 报文 | 行为 |
| --- | --- | --- |
| 宿主 → EXE | `{"type":"shutdown"}` | 回 `shutdown_ack`，`Run()` 返回 `false`，主循环退出 |
| 宿主 → EXE | `{"type":"message","payload":{"action":"enableApiMode"}}` | 写宿主 `config.json` 并回结果事件 |
| EXE → 宿主 | `{"type":"message","payload":{"event":"shutdown_ack"}}` | shutdown 确认 |
| EXE → 宿主 | `{"type":"message","payload":{"event":"seek","position":..,"duration":..}}` | 卡片进度条拖动，由 `background.js` 转发到宿主页面 |
| EXE → 宿主 | `{"type":"message","payload":{"action":"enableApiMode","result":"ok","path":..}}` | 授权结果 |

- **stdin EOF：** 独立运行时没有写入者，`Run()` 返回 `true`、程序继续运行；这是「独立模式不因 stdin 关闭而退出」的关键。
- **宿主侧生命周期：** `stopAll()` / `stopHostByKey()` 发 shutdown 后等待退出，1.5 秒超时再留 500ms 宽限并 `tree-kill` 兜底；插件侧关闭 stdin 线程用 `CancelSynchronousIo` 循环（见 3.11）。
- **Bridge 层：** `native-bridge.html/js` 把 `window.electronAPI.nativeHost.*` 包装成 `chrome.runtime.Port` 消息；`background.js` 用 `requestId` 配对请求/响应、`seq` 防重放、10 秒超时，并在端口断开时立即拒绝所有未决请求。

***

## 5. 构建与部署

> 可执行命令以 [README「构建与测试」](../README.md) 为准；本节解释**契约与取舍**，并给出最小可用命令集。

### 5.1 环境要求

| 工具 | 版本 | 说明 |
| --- | --- | --- |
| Windows SDK | 10.0.20348+ | — |
| Visual Studio | 2022 (v143) | 需「使用 C++ 的桌面开发」+ CMake 工具；构建 ARM64 另需 ARM64 工具链 |
| MSVC 工具集 | 14.44+ | **默认不锁定版本**，由 VS 自行选择；链接不兼容时可用 `MKK_TOOLSET_VERSION` 显式指定 |
| CMake | **3.21+** | `CMakePresets.json` 用 schema `version: 3`，该 schema 自 3.21 起才支持（3.20 只到 version 2）。`cmake_minimum_required`、README、本文、`check_build_env.ps1` 阈值必须同值 |
| vcpkg | 由 `scripts\bootstrap.ps1` 准备 | **classic 模式**，按 triplet 装到 `installed\x64-windows` / `installed\arm64-windows`；`-VcpkgCommit <sha>` 可锁定全新 clone 的版本 |
| Python | 3.x | 仅 `verify_package.py` / `pack_zip.py` 与 `build.cmd release all` 需要 |

**依赖清单唯一来源：`vcpkg.json` 的 `dependencies`**（当前 5 项：`ixwebsocket`、`nlohmann-json`、`zlib`、`mbedtls`、`kissfft`）。`bootstrap.ps1` 不再手写包名；两个 PowerShell 脚本都 dot-source `scripts\vcpkg_contract.ps1`，由它读取 `vcpkg.json`，并持有**唯一一份**「CMake 实际会读的 12 个文件」清单——`bootstrap.ps1` 装完后的校验与 `check_build_env.ps1` 的只读自检都消费同一份定义。增删依赖只需改 `vcpkg.json` 一处；只有当新依赖会产出新的必需文件时，才需要同步 `vcpkg_contract.ps1` 的文件清单（文件路径无法从端口名推导，这一处无法消除）。

`bootstrap.ps1` 的 `--classic` **必须保留**：脚本 cwd 就是含 `vcpkg.json` 的项目根，去掉它 vcpkg 会切到 manifest 模式、把包装进 `vcpkg_installed\`，而 `CMakeLists.txt` 的 `find_library` 不读该目录。将来若真的切 manifest 模式，必须先从 `vcpkg.json` 移除 `ixwebsocket`——`CMakeLists.txt` 已通过 `add_subdirectory(third_party/ixwebsocket)` 定义 `ixwebsocket::ixwebsocket` target，两者会重名冲突。

项目并**不使用 vcpkg manifest 模式**。即使本机残留 `vcpkg_installed\<triplet>\`（旧 manifest 产物，已被 `.gitignore` 忽略），`build.cmd` / `CMakeLists.txt` 也不会解析它，`check_build_env.ps1` 会明确报 `manifest tree unused`；清理与否都不影响构建。`vcpkg.json` 的 `version` 字段与 `project(VERSION)` 属另一条版本同步链，见 7.2。

### 5.2 构建契约（为什么这样设计）

早期构建把依赖路径绑在开发者机器的环境变量和固定盘符上，换台机器就 configure 失败。现在的约定是：

1. **单一准备入口**：`bootstrap.ps1` 复用/克隆 vcpkg → 按 commit 定位版本 → 从 `vcpkg.json`（经 `vcpkg_contract.ps1`）取包名并 `--classic` 安装 → 按契约模块的 12 文件清单校验 → 补 `git submodule update --init --recursive` → 把选定的根写进 `.vcpkgroot`（git 忽略）。
2. **四条链同源**：`bootstrap.ps1` 是唯一做「选根」决策的一方，把结果写进 `.vcpkgroot`；`build.cmd`、`CMakeLists.txt`、`check_build_env.ps1` 都只读 pin，不再各自重新决策。四条链解析的是同一个量 `installedDir = <root>\installed\<triplet>`。优先级：
   显式覆盖（`VCPKG_INSTALLED_DIR` env / `cmake -D...` / bootstrap 的 `-VcpkgRoot`）→ `.vcpkgroot` → `VCPKG_INSTALLATION_ROOT` / `VCPKG_ROOT` → `D:\vcpkg` / `C:\vcpkg`（旧环境兜底）。
   **pin 必须排在机器级环境变量之前**：否则 bootstrap 装进 A、构建却从环境变量指向的 B 取库，而自检还会误报通过。CI 也因此刻意不设 `VCPKG_INSTALLED_DIR`，让每次构建都在验证 pin 链路。
3. **冲突 fail-fast**：`bootstrap.ps1` 安装前检查 `VCPKG_INSTALLED_DIR`，若它反推出的 vcpkg 根与本次选定根不同（或末级目录不是目标 triplet），直接抛错并给出修复建议；`check_build_env.ps1` 则列出被忽略的其他完整依赖树。不要把该变量设成用户级常量：它指向某一个 triplet，切架构时会把错误的库喂给 CMake。
4. **架构强绑定**：`CMakeLists.txt` 校验 `VCPKG_INSTALLED_DIR` 的最后一级目录必须等于目标 `VCPKG_TRIPLET`，禁止跨 triplet（含 `-142` 历史变体）fallback——ARM64 混入 x64 库往往到运行期才暴露为 `Bad Image`。
5. **不走 `CMAKE_TOOLCHAIN_FILE`**：vcpkg 工具链会带来 MSVC 工具集不匹配的链接错误；项目改用 `find_library` / `find_path` 手写导入目标。`ixwebsocket` 的取值顺序是：`third_party/ixwebsocket`（git submodule，源码编译，CI 与本地一致）→ vcpkg `buildtrees` 源码 → 目标 triplet 下的预编译库。所以**优先源码、不是优先 vcpkg 预编译库**。
6. **CRT 不入库**，见 5.5。
7. **依赖数据与路径解析分离**：`vcpkg_contract.ps1` 只提供包名和必需文件清单，不参与选根。四条链保留各自的薄解析实现，是为了不让 `build.cmd` / `cmake configure` 为读一个路径额外起 PowerShell 进程；解析顺序的一致性靠独立契约测试断言，而不是靠跨语言共享一份实现。

### 5.3 常用命令（PowerShell）

```powershell
git clone --recurse-submodules https://github.com/Yngu196/TaskbarLyrics.git
cd TaskbarLyrics

# 干净克隆 → 依赖就位 → 直接构建（clone 时漏了 --recurse-submodules 也会被 bootstrap 补上）
.\scripts\bootstrap.ps1 -Arch x64 -Build
.\build.cmd release x64

# ARM64 必须再准备一次：两个架构共用一个 vcpkg 根，但依赖分落在不同 triplet
.\scripts\bootstrap.ps1 -Arch arm64 -Build

# Visual Studio / preset 路径（同样先按目标架构 bootstrap）
.\scripts\bootstrap.ps1 -Arch x64
cmake --preset x64-Release
cmake --build --preset x64-Release

# 只读自检：按统一解析契约逐项检查依赖、submodule、CRT、工具集
.\scripts\check_build_env.ps1 -Arch x64

# 全量发布包：x64 → ARM64 → 架构校验 → 打包（需两个架构都已 bootstrap、且装了 Python 3）
# 默认产出 moeKoe-taskbar-lyrics.zip
.\build.cmd release all
.\build.cmd clean x64
```

`build.cmd` 支持 `debug|release|clean` × `x64|arm64|all`；`scripts\build.bat` 只是转发参数给它的兼容代理，不要再维护独立逻辑。

### 5.4 测试与超时护栏

`BUILD_TESTING` 默认 **OFF**（避免每次 configure 联网探测 Catch2）；首次要显式开启，已有 `out\build\<preset>` 缓存会保留该值。Catch2 v3.7.1 的取值优先级：`-DCATCH2_SOURCE_DIR=<path>` → 复用 `build/_deps/catch2-src` / `third_party\catch2\` → GitHub/gitee 克隆；两者都不可达且无本地源码时，`CMakeLists.txt` 会 `set(BUILD_TESTING OFF ... FORCE)` 跳过测试。

```powershell
cmake --preset x64-Release -DBUILD_TESTING=ON
cmake --build --preset x64-Release
ctest --test-dir out\build\x64-Release -C Release --output-on-failure --timeout 30
```

| 测试目标 | 覆盖 |
| --- | --- |
| `LyricsParser` / `KrcParser` | LRC/KRC 解析、字符级时间轴、翻译提取 |
| `SpectrumMath` | 频谱纯数学映射 |
| `HttpLifecycle` | HTTP 服务 Start/Stop 线程生命周期（曾因持锁 join + 端口 connect 唤醒死锁，见 3.2） |
| `SingleInstance` | 单实例获取、独立秒退、托管等待接管、超时继续、跨进程接管（见 3.18） |

三层超时护栏：CMake 给 `HttpLifecycle` / `SingleInstance` 设 `TIMEOUT 30`、CI `ctest --timeout 30`、`build.yml` job `timeout-minutes: 45`。WASAPI、Direct2D 设备丢失、Explorer 重启属真机集成测试范围，未自动化。

### 5.5 VC++ 运行时 DLL（不入库）

`msvcp140.dll` / `vcruntime140.dll`（x64 另含 `vcruntime140_1.dll`）**不随仓库分发**，本地与 CI 补齐方式不同：

- **本地构建**：`CMakeLists.txt` 的 POST_BUILD 从本机 VS 的 `VC\Redist\MSVC\<版本>\<arch>\Microsoft.VC143.CRT` 复制；找不到只发 WARNING，可手动跑 `copy_vc_runtime.ps1` 补齐。ARM64 跳过 `vcruntime140_1.dll`——VS 的 arm64 Redist 目录里那个文件实为 x64 二进制，混入会触发架构校验失败。
- **CI**：`build.yml` / `release.yml` 调同一个脚本，按两级来源补齐：① 用 `vswhere` 定位本机 VS Redist（不依赖 `VCToolsRedistDir` 等机器级环境变量）；② 本机没有对应架构的 retail CRT 时（典型是 x64 runner 编 ARM64），下载微软官方 `vc_redist.<arch>.exe`，用 Windows 自带 `expand.exe` 从 Burn bundle 内嵌 CAB 提取，不引入 7-Zip / WiX。

脚本逐文件校验 PE Machine，仅在缺失或错架构时覆盖；两级来源都拿不到就明确失败，不静默产出缺 CRT 的包。常用参数：`-Arch x64|arm64`、`-Destination <目录>`（必填）、`-VerifyOnly`（只检查不写不下载）、`-RedistPackage <path>`（离线包）、`-OfficialOnly`（跳过本机 VS，强制统一官方包版本）。官方 permalink 总指向最新 v14 可再发行包，与 v143～v145 工具集二进制兼容，但 CRT 小版本不锁定。

> `Microsoft.VC143.CRT` 只是目录名，不代表编译器工具集版本；CI runner 可用 VS2026 / MSVC 14.51（v145），运行时仍是 140 系列 DLL。

### 5.6 部署流程

**托管模式（推荐）**：插件目录是 `%APPDATA%\moekoemusic\extensions`（**不是** `plugins/extensions/`）。把 `moeKoe-taskbar-lyrics` 整个目录放进去，使其下直接包含 `manifest.json`、`x64/`、`arm64/`；在插件管理页点击「本地程序授权」，重启 MoeKoeMusic。前置条件：MoeKoeMusic ≥ `manifest.json` 的 `minversion`（当前 1.6.6）、设置中开启 **API 模式**并重启。`auto_start: true` 使 EXE 随宿主自动启动/退出。

**独立模式（回退）**：双击根目录 `MoeKoeTaskbarLyrics.exe`（Launcher）或架构子目录内的真实程序，托盘菜单控制全部功能；建议开启开机自启。

### 5.7 发布打包

```powershell
# 双架构完整性 + 逐文件 PE 架构校验，任一不符即非零退出
python scripts\verify_package.py moeKoe-taskbar-lyrics\
# 打包（输出名可自定义；build.cmd release all 默认产出 moeKoe-taskbar-lyrics.zip）
python scripts\pack_zip.py moeKoe-taskbar-lyrics\ moeKoe-taskbar-lyrics-windows-x64-arm64.zip
# 内部结构: moeKoe-taskbar-lyrics/{manifest.json, *.js, *.html, icons/,
#           MoeKoeTaskbarLyrics.exe(Launcher, x64), x64/**, arm64/**}
```

`verify_package.py::check_structure()` 是**前置硬检查**：根目录 Launcher、`x64/`、`arm64/` 必须同时存在，`arm64/` 内至少一个 `.exe`。只扫 PE Machine 无法发现「arm64 artifact 丢失却仍然 PASS」的情况。根启动器固定 x64（静态 CRT 自包含），ARM64 Windows 经 x64 仿真层运行后拉起 `arm64/` 原生主程序。

### 5.8 CI

| Workflow | 触发 | 内容 |
| --- | --- | --- |
| `build.yml` | push / PR / `workflow_dispatch` | `check` 当天限流（**只认 `conclusion == "success"`**，cancelled/failure 不锁死后续 push）→ `build` 矩阵 `x64` + `arm64`：bootstrap → CMake → 编译 → CRT → artifact |
| `release.yml` | `v*` tag | 同一套 bootstrap/CRT 逻辑，双架构 artifact 合并 → `verify_package.py` → changelog → GitHub Release |

两个 workflow 都 `submodules: recursive`、每个 matrix job 各自 bootstrap；`build.yml` 显式 `-DBUILD_TESTING=ON` 并只在 x64 job 跑 `ctest`（ARM64 二进制无法在 x64 runner 执行），`release.yml` 为 `OFF`。CI 与本地共用 `bootstrap.ps1` / `copy_vc_runtime.ps1`，避免两条链路行为分叉。

### 5.9 卸载

1. 托盘图标 → 退出
2. 删除 `%APPDATA%\moekoemusic\extensions\moeKoe-taskbar-lyrics\`
3. （可选）删除 `%APPDATA%\MoeKoeTaskbarLyrics\`（配置）与注册表自启项 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\MoeKoeTaskbarLyrics`

***

## 6. 扩展与维护

### 6.1 兼容性保障

| 场景             | 策略                                      |
| -------------- | --------------------------------------- |
| MoeKoeMusic 更新 | WebSocket 协议不变则无需更新                     |
| 多显示器           | `EnumWindows` 枚举主/副屏任务栏 + `MonitorFromWindow` 按显示器绑定，活动窗口/鼠标位置优先决策；窗口页支持「自动跟随 / 手动指定」两种显示位置模式，手动模式锁定到所选显示器 |
| 任务栏自动隐藏        | `WM_SETTINGCHANGE` + 轮询 `CheckResize()` |
| Win11 任务栏居中    | 浮动窗口天然兼容                                |

### 6.2 代码质量

| 措施 | 说明 |
| --- | --- |
| 常量集中 | `constants.h` 消除魔数；少量模块内硬编码见 2.4 |
| 协作式退出 | stop flag + `join()`；超时是 `detach()`（封面下载 worker），**不再使用 `TerminateThread`** |
| 消息大小限制 | WS 1 MB / HTTP 请求体 4 MB / Native Host 单行 4 MB |
| 配置值校验 | `std::clamp` 范围验证 |
| 异常恢复 | `WM_TIMER` catch 自动重试 |
| UTF 安全转换 | `Utf8ToWide` / `WideToUtf8` 统一处理 |
| 本地 Token 鉴权 | `X-MoeKoe-Token` + 恒定时间比较；fallback token 时 HTTP 服务拒绝启动 |
| CORS 动态端口 | 默认 `Allow-Origin` 用运行时端口；两个免鉴权 GET 为 `*` |
| 跨线程窗口操作 | 网络线程一律 `PostMessageW` 到 UI 线程，避免跨线程窗口调用死锁 |
| 架构强绑定 | vcpkg triplet 严格校验 + 发布包逐文件 PE Machine 校验 + 双架构完整性前置检查 |
| 测试超时护栏 | `TIMEOUT 30` 测试属性 / `ctest --timeout 30` / job `timeout-minutes: 45` |
| 构建契约可复现 | `bootstrap.ps1` + `.vcpkgroot` + CI 同一入口，CRT 不入库 |

### 6.3 调试与日志

- 日志位置：exe 同目录 `debug.log`（5 级 + 5 MB 轮转）
- 启用方式：配置 `advanced.debug_log = true`；也可从设置界面一键导出诊断报告（`diagnostic_exporter`）
- 启动/关闭关键行：

  | 日志前缀 | 含义 |
  | --- | --- |
  | `[STARTUP] WinMain entered (pid=…, launch=host-managed\|standalone)` | 托管识别结果，排查单实例问题先看这行 |
  | `[STARTUP] AutoStart=…` | 注册表自启状态 |
  | `[STARTUP] Took over after previous instance exited (waited …)` | 宿主重载后成功接管（3.18） |
  | `[NATIVE-HOST] Received shutdown command` | 收到宿主 shutdown，优雅退出起点 |
  | `[SHUTDOWN] Complete` | 进程真正结束 |

- **排查重载竞态**：把 EXE 的 `debug.log`（`%APPDATA%\moekoemusic\extensions\moeKoe-taskbar-lyrics\<arch>\`）与宿主日志的时间线对齐。若宿主在 `[SHUTDOWN] Complete` 之前就记「本地程序已退出 …: 0」，说明新进程抢锁秒退被误判为正常退出。

### 6.4 未来扩展

| 功能 | 难度 | 优先级 | 状态 |
| --- | --- | --- | --- |
| ARM64 实机验证 | 中 | ⭐⭐⭐ | 🔜 构建链路已闭环，真实 ARM64 Windows 冒烟测试仍待反馈；**不宣称正式支持** |
| 宿主重载竞态 | — | ⭐⭐⭐ | ✅ 插件侧已加等待接管（3.18）；宿主 `restartExtensions()` 未 await 需上游修复 |
| 构建契约 | — | ⭐⭐ | ✅ bootstrap + `.vcpkgroot` + CI 统一入口，CRT 不入库 |
| 绑定模式进程监控 | 低 | ⭐⭐ | ⚠️ 代码仍在，但当前 `main.cpp` 未接入（3.15）；退出由 Native Host 驱动 |
| 进程生命周期联动优化 | 中 | ⭐⭐ | 🔜 待评估 |
| 支持其他播放器 | 高 | ⭐ | ❌ 新立项 |
| 颜色提取到常量 | 低 | ⭐ | 🔜 低优先级 |

***

## 7. 附录

### 7.1 参考文档

| 主题 | 链接 |
| --- | --- |
| 本项目源码 / Issue | <https://github.com/Yngu196/TaskbarLyrics> |
| 构建说明（可执行命令） | [README 构建与测试](../README.md) |
| MoeKoeMusic 源码 | <https://github.com/MoeKoeMusic/MoeKoeMusic> |
| CMake Presets | <https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html> |
| vcpkg 文档 | <https://learn.microsoft.com/vcpkg/> |
| DirectWrite | <https://learn.microsoft.com/windows/win32/directwrite/text-formatting-and-layout> |
| High DPI | <https://learn.microsoft.com/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows> |
| ixwebsocket | <https://github.com/machinezone/IXWebSocket> |
| cpp-httplib | <https://github.com/yhirose/cpp-httplib> |
| Catch2 | <https://github.com/catchorg/Catch2> |
| nlohmann/json | <https://github.com/nlohmann/json> |

### 7.2 版本与文档同步清单

改动下列任一处时，同步检查其余位置，避免再次出现本文本次修订前那种「文档停在 v1.0.7、构建写 manifest 模式」的分叉：

| 项目 | 事实来源 | 需要同步的位置 |
| --- | --- | --- |
| 版本号 | `CMakeLists.txt` 的 `project(VERSION)` | `constants.h::PLUGIN_VERSION`、插件 `manifest.json`、`vcpkg.json` 的 `version`、README 发布徽章与 tag、`Docs\版本更新日志.md` |
| CMake 下限 | `CMakePresets.json` 的 schema `version` | `cmake_minimum_required`、README 环境表、本文 5.1、`check_build_env.ps1` 阈值 |
| 依赖包名 | `vcpkg.json` 的 `dependencies` | README、本文 5.1；`bootstrap.ps1` / `check_build_env.ps1` 经 `vcpkg_contract.ps1` 读取，**不再各自声明** |
| 必需文件清单 | `scripts\vcpkg_contract.ps1` | `CMakeLists.txt` 的 `find_*` / POST_BUILD（消费侧，不是声明）、本文 5.1 |
| 测试目标 | `CMakeLists.txt` 的 `add_test` | README「测试覆盖」、本文 5.4、`build.yml` 的 ctest 步骤 |
| CRT 行为 | `copy_vc_runtime.ps1` | `CMakeLists.txt` POST_BUILD、`build.yml` / `release.yml`、README、本文 5.5 |
| 部署路径 | 插件 `manifest.json` / 宿主扩展目录 | README「安装与使用」、本文 5.6 |

> **当前已知不一致（2026-11-12，留待发布时拍板）**：
> 1. README 发布徽章指向 `v1.0.9` tag，而代码内三处版本字段仍是 `1.0.8`；
> 2. `Docs\版本更新日志.md` 最新条目停在 `v1.0.8`。`release.yml` 按 tag 从该文件提取 Release notes，找不到对应条目时只报 WARNING 并继续，会发布**无 Release notes** 的版本；
> 3. `Docs\项目状态文档.md` 头部仍写 `v1.0.7`。
>
> 本次只更新开发文档，未擅自改代码版本字段或其他文档，以免在未经确认的情况下改动发布标记。
