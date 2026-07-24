# MoeKoeMusic 任务栏歌词插件 — 开发文档

> **版本：** v1.0.1 | **语言：** C++17 | **平台：** Windows 10/11 (x64) | **协议：** GPL-3.0

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

```
MoeKoeMusic-TaskbarLyrics/
├── src/
│   ├── constants.h              # 全局常量（端口/尺寸/消息号/鉴权）
│   ├── main.cpp                 # WinMain 入口 + NativeMessagingHost
│   ├── native_messaging.h/cpp   # Native Host JSON Lines 协议
│   ├── websocket_client.cpp/h   # WebSocket 数据接收 (ixwebsocket)
│   ├── http_server.cpp/h        # HTTP 服务器 (默认6523, Token鉴权)
│   ├── lyrics_parser.cpp/h      # 歌词 JSON/KRC/LRC 解析
│   ├── taskbar_window.cpp/h     # 浮动窗口 + Z-order + APPBAR 适配
│   ├── taskbar_embedder.cpp/h   # 任务栏嵌入逻辑
│   ├── taskbar_geometry.cpp/h   # 任务栏几何查询
│   ├── renderer.cpp/h           # D2D 渲染引擎 + 跑马灯
│   ├── config.cpp/h             # 配置管理 (JSON + 注册表自启)
│   ├── api_enabler.cpp/h        # API 模式自动检测/开启
│   ├── tray_icon.cpp/h          # 系统托盘图标+菜单
│   ├── logger.cpp/h             # 统一日志系统
│   ├── d2d_settings_window.cpp/h # D2D 自绘设置界面
│   └── color_picker.cpp/h       # D2D 颜色选择器
├── moeKoe-taskbar-lyrics/       # Chrome Extension
│   ├── manifest.json            # moekoe_native_hosts + minversion:1.6.6
│   ├── background.js            # Service Worker + Bridge Port
│   ├── native-bridge.html/js    # Bridge: chrome.runtime.Port ↔ electronAPI
│   └── icons/icon256.png
├── scripts/pack_zip.py          # 发布打包脚本
└── public/icons/icon256.png     # 插件中心图标
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

### 2.3 性能目标

| 指标     | 目标值                     |
| ------ | ----------------------- |
| CPU 占用 | < 0.5% (空闲) / < 2% (播放) |
| 内存     | < 20 MB                 |
| 帧率     | 60 FPS (最高 120 FPS)     |
| 启动延迟   | < 500 ms                |
| 渲染延迟   | < 5 ms                  |

### 2.4 全局常量 (`constants.h`)

| 分类  | 常量                           | 值              | 用途                |
| --- | ---------------------------- | -------------- | ----------------- |
| 端口  | WEBSOCKET\_LISTEN\_PORT      | 6520           | WS 歌词数据           |
| 端口  | HTTP\_SERVER\_PORT           | 6523           | Extension 通信(可覆盖) |
| 鉴权  | LOCAL\_AUTH\_HEADER\_NAME    | X-MoeKoe-Token | HTTP 鉴权头          |
| 渲染  | MIN\_FRAME\_INTERVAL\_MS     | 15             | 最小帧间隔             |
| 尺寸  | LYRIC\_HEIGHT\_BASE\_DP      | 28             | 歌词高度(96DPI)       |
| 尺寸  | MAX\_LYRIC\_WIDTH\_BASE\_DP  | 360            | 最大宽度              |
| 消息号 | WM\_TRAY\_CALLBACK           | 0x0600         | 托盘回调              |
| 消息号 | WM\_RENDER\_UPDATE           | 0x0700         | 渲染更新              |
| 消息号 | WM\_PROCESS\_EXITED          | 0x0800         | 进程退出              |
| 安全  | MAX\_WS\_MESSAGE\_SIZE       | 1MB            | WS 消息上限           |
| 跑马灯 | MARQUEE\_DELAY/PAUSE\_MS     | 2000/1000      | 滚动延迟/暂停           |
| 跑马灯 | MARQUEE\_SPEED\_PX\_PER\_SEC | 40             | 默认速度              |

***

## 3. 模块设计

### 3.1 歌词数据获取模块

**文件：** `src/websocket_client.cpp/h`

- 连接 `ws://127.0.0.1:6520`，接收 `lyrics`/`playerState` 消息
- **重连策略：** 指数退避 1s→2s→4s→8s→15s（上限）
- **协作式退出：** `Disconnect()` 设置 `stopRequested_` + `join()`
- **消息限制：** 入口检查 `raw.size() > 1MB`，超限丢弃
- **KRC 解析：** MoeKoeMusic 实际推送 KRC 格式，`ParseKrc()` 内联实现
- **API 自动开启：** 第 3 次连接失败时触发 `ApiEnabler::TryEnableApi()`

### 3.2 HTTP 服务器模块

**文件：** `src/http_server.cpp/h`

- 端口可配置（默认 6523），用于 Chrome Extension 通信
- **鉴权流程：**
  ```
  请求 → method == "OPTIONS"? → 跳过鉴权 → 204 No Content
       → CheckLocalAuthToken() → Token 缺失 → 403
       → 路由处理 (GET/ping, POST/shutdown 等)
  ```
- **CORS 动态端口：** `Allow-Origin` 使用运行时实际端口
- **确定性响应体：** ping → `{status, service}`，shutdown → `{status:"shutting_down"}`

### 3.3 任务栏浮动窗口模块

**文件：** `src/taskbar_window.cpp/h`

- 查找 `Shell_TrayWnd`，创建独立浮动 `Layered Window`
- **窗口样式：** `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE` + `WS_POPUP`
- **嵌入方式对比：**

| 维度      | SetParent (原规划) | 浮动覆盖 (实际)       |
| ------- | --------------- | --------------- |
| 父窗口     | Shell\_TrayWnd  | nullptr (顶层)    |
| Z-order | 继承任务栏           | WS\_EX\_TOPMOST |
| 稳定性     | Win11 不稳定       | ✅ 成熟方案          |

- **APPBAR 自动隐藏：** 检测 `ABS_AUTOHIDE` 状态，窗口跟随任务栏显隐
- **拖动+锁定：** 支持拖动调整位置、锁定位置（保留按钮）、完全锁定（禁止所有交互）

### 3.4 歌词同步与解析模块

**文件：** `src/lyrics_parser.cpp/h`

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

### 3.5 渲染引擎模块

**文件：** `src/renderer.cpp/h`

- 初始化 D2D/DW/WIC 工厂，通过 `UpdateLayeredWindow` 呈现
- **卡拉 OK 高亮：** PushAxisAlignedClip 裁剪方案（非 SetDrawingEffect）
  1. 绘制灰色文字 → 2. 计算裁剪宽度 → 3. Push Clip 绘制高亮 → 4. Pop Clip
- **翻译文本：** 小号字体居中对齐
- **悬停控制按钮：** ⏮ ⏸/▶ ⏭ + 半透明背景
- **性能优化：**
  - `btnFormat_` 类成员缓存，避免每帧重建
  - UI 参数常量引用
  - 按需渲染（变化时才 UpdateLayeredWindow）
- **异常恢复：** WM\_TIMER catch → Shutdown + Reinitialize → 失败则 PostQuitMessage

### 3.6 跑马灯状态机

**文件：** `src/renderer.cpp/h`

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

### 3.7 配置管理模块

**文件：** `src/config.cpp/h`

#### 配置结构

```json
{
  "auto_start": true,
  "appearance": {
    "highlight_color": "#4CC2FF", "normal_color": "#333333",
    "normal_opacity": 0.85, "font_family": "华文细黑",
    "font_size": 20, "enable_karaoke": true,
    "enable_translation": true, "enable_marquee": true,
    "marquee_mode": "bounce", "marquee_delay_ms": 2000,
    "marquee_pause_ms": 1000, "marquee_speed_px_per_sec": 40
  },
  "advanced": {
    "websocket_port": 6520, "http_server_port": 6523,
    "refresh_rate_hz": 60, "debug_log": false
  },
  "position": { "offset_x": 0, "offset_y": 0, "lock_position": false, "lock_fully": false }
}
```

- **范围验证：** `std::clamp` 校验 opacity\[0,1], fontSize\[8,72], port\[1024,65535], refreshRate\[1,120]
- **配置路径：** `%APPDATA%/MoeKoeTaskbarLyrics/config.json`
- **自启动：** 注册表 `HKCU\...\Run\MoeKoeTaskbarLyrics`

### 3.8 系统托盘模块

**文件：** `src/tray_icon.cpp/h`

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

### 3.9 D2D 原生设置窗口

**文件：** `src/d2d_settings_window.cpp/h`

- 纯 D2D + DW 自绘，零外部依赖
- **控件体系：** LabelRow/ToggleRow/SliderRow/ColorRow/DropdownRow/ButtonRow
- **自绘标题栏：** 拖动/关闭/最小化
- **暗/亮检测：** 注册表读取系统主题
- **延迟操作：** PostMessage 异步执行防崩溃

### 3.10 API 自动开启模块

**文件：** `src/api_enabler.cpp/h`

- **触发时机：** 启动时主动检测 + WS 第 3 次重连失败
- **工作流程：**
  ```
  检测进程 → 定位 config.json → 读取 apiMode
  → 写入 .tmp → MoveFileEx 原子替换 → ShellExecuteW 重启
  ```
- **防重复：** 静态 `s_attempted` 每周期只尝试一次

### 3.11 主程序入口

**文件：** `src/main.cpp`

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

#### Z-order 三重防护

| 层级     | 触发            | 实现                                             |
| ------ | ------------- | ---------------------------------------------- |
| ① 创建时  | `Create()`    | `WS_EX_TOPMOST` + `SetWindowPos(HWND_TOPMOST)` |
| ② 消息响应 | `WM_ACTIVATE` | 激活/失活均断言 TOPMOST                               |
| ③ 定期兜底 | \~30帧         | 周期性强制断言 TOPMOST                                |

### 3.12 统一日志模块

**文件：** `src/logger.cpp/h`

- `moekoe::Log(fmt, ...)` + `moekoe::Log(string)` 两种重载
- 路径：exe 同级 `debug.log`（可移植）
- 开关：`config.debugLog` 驱动
- 线程安全：`std::mutex` 保护

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
| vcpkg         | 最新版         |

### 5.2 构建命令

```bash
vcpkg install ixwebsocket:x64-windows nlohmann-json:x64-windows
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
# 产物: build/Release/MoeKoeTaskbarLyrics.exe
```

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
| 绑定模式接入  | 低  | ⭐⭐  | ⚠️ 待接入  |
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

