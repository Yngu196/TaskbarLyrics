# MoeKoeMusic 任务栏歌词插件 — 开发文档

> **版本：** v0.3（草案）  
> **目标平台：** Windows 10/11（x64）  
> **开发语言：** C++20  
> **构建系统：** CMake + MSVC  
> **协议：** GPL-2.0（继承自 MoeKoeMusic）

---

## 目录

1. [项目概述](#1-项目概述)
2. [技术架构](#2-技术架构)
3. [模块设计](#3-模块设计)
   - 3.1 [歌词数据获取模块](#31-歌词数据获取模块)
   - 3.2 [任务栏嵌入窗口模块](#32-任务栏嵌入窗口模块)
   - 3.3 [歌词同步与解析模块](#33-歌词同步与解析模块)
   - 3.4 [渲染引擎模块](#34-渲染引擎模块)
   - 3.5 [配置管理模块](#35-配置管理模块)
   - 3.6 [系统托盘模块](#36-系统托盘模块)
   - 3.7 [进程监控模块](#37-进程监控模块)
   - 3.8 [主程序入口](#38-主程序入口)
4. [协议与接口](#4-协议与接口)
5. [构建与部署](#5-构建与部署)
6. [扩展与维护](#6-扩展与维护)
7. [附录](#7-附录)

---

## 1. 项目概述

### 1.1 背景

MoeKoeMusic 是一款基于 Electron + Vue 3 的开源音乐播放器，其 Windows 版本使用 Electron 构建桌面界面。原版软件提供了桌面歌词窗口，**但缺少任务栏歌词显示功能**。

### 1.2 目标

开发一个**独立的任务栏歌词插件**，将歌词**嵌入到 Windows 任务栏内部**（而非悬浮在任务栏上方），实现与系统 UI 无缝融合的歌词显示体验。

### 1.3 设计原则

| 原则 | 说明 |
|------|------|
| **零侵入** | 不修改 MoeKoeMusic 本体任何文件，独立 EXE 运行 |
| **独立维护** | 插件可脱离主程序版本独立迭代 |
| **轻量高效** | CPU 占用 < 2%，内存占用 < 20MB |
| **用户友好** | 即开即用，系统托盘右键菜单控制启用/禁用 |
| **嵌入任务栏** | 歌词窗口作为任务栏的子窗口，视觉上融为一体 |

### 1.4 数据获取策略

采用 **WebSocket 监听**（端口 6520）获取歌词和播放状态数据，无需内存 Hook 或文件嗅探。

| 方案 | 可行性 | 维护成本 | 推荐度 |
|------|--------|---------|-------|
| ✅ WebSocket 监听端口 6520 | MoeKoeMusic 原生推送 | 低 | ⭐⭐⭐ |
| ⬜ 歌词缓存文件读取 | 项目无独立缓存文件 | 中 | ⭐⭐ |
| ❌ 内存 Hook | 兼容性差、易崩溃 | 高 | ⭐ |

### 1.5 项目结构

```
MoeKoeMusic-TaskbarLyrics/
├── CMakeLists.txt              # CMake 构建配置
├── README.md                   # 插件说明
├── src/
│   ├── main.cpp                # 程序入口
│   ├── websocket_client.cpp/h  # WebSocket 数据接收
│   ├── lyrics_parser.cpp/h     # 歌词 JSON 解析 & LRC 同步
│   ├── taskbar_window.cpp/h    # 任务栏嵌入窗口管理
│   ├── renderer.cpp/h          # Direct2D 渲染引擎
│   ├── config.cpp/h            # 配置管理
│   ├── process_monitor.cpp/h   # 进程监控（绑定模式）
│   └── tray_icon.cpp/h         # 系统托盘图标
├── resources/
│   ├── icon.ico                # 托盘图标
│   └── font/                   # 自定义字体（可选）
└── scripts/
    └── build.bat               # 一键构建脚本
```

---

## 2. 技术架构

### 2.1 整体架构图

```
┌──────────────────────────────────────────────────────────────┐
│                      MoeKoeMusic 主程序                      │
│  ┌────────────┐    ┌──────────────────┐    ┌──────────────┐  │
│  │  Vue 前端   │───▶│ IPC (主进程)      │───▶│ KuGou API    │  │
│  │ (歌词状态)  │    │                  │    │ (HTTP :6521) │  │
│  └─────┬──────┘    └────────┬─────────┘    └──────────────┘  │
│        │                    │                                 │
│        │  IPC: lyrics-data  │                                 │
│        │  IPC: play-pause   │                                 │
│        ▼                    ▼                                 │
│  ┌──────────────────────────────────────────────────────┐     │
│  │              WebSocket Server (:6520)                 │     │
│  │  推送: {type:"lyrics", data:[...]}                   │     │
│  │  推送: {type:"playerState", data:{...}}              │     │
│  └──────────────────────┬───────────────────────────────┘     │
└─────────────────────────┼─────────────────────────────────────┘
                          │  WebSocket 连接
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                任务栏歌词插件 (独立 EXE)                      │
│                                                             │
│  ┌──────────────────┐                                       │
│  │ 进程监控模块       │  ← 绑定模式：每 2 秒轮询 MoeKoeMusic │
│  │ ProcessMonitor   │     独立模式：跳过此模块               │
│  └────────┬─────────┘                                       │
│           │ MoeKoeMusic 启动/退出事件                        │
│           ▼                                                  │
│  ┌─────────────────┐    ┌──────────────────┐               │
│  │ WebSocket Client │───▶│ 歌词解析器        │               │
│  │ (连接 :6520)     │    │ (JSON→时间轴)    │               │
│  └─────────────────┘    └────────┬─────────┘               │
│                                  │ 当前歌词 + 进度          │
│                                  ▼                          │
│  ┌──────────────────────────────────────────────────┐      │
│  │              Direct2D 渲染引擎                     │      │
│  │   逐帧绘制文字 → 更新 Layered Window 内容          │      │
│  └──────────────────────┬───────────────────────────┘      │
│                         │                                   │
│                         ▼                                   │
│  ┌──────────────────────────────────────────────────┐      │
│  │     嵌入任务栏内部的歌词子窗口                        │      │
│  │  ┌────────────────────────────────────────────┐    │      │
│  │  │  Windows 任务栏 (Shell_TrayWnd)              │    │      │
│  │  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌────────────┐ │    │      │
│  │  │  │ 开始 │ │搜索栏│ │ 任务 │ │ 🎵歌词窗口 │ │    │      │
│  │  │  │     │ │      │ │ 按钮 │ │ (子窗口)   │ │    │      │
│  │  │  └──────┘ └──────┘ └──────┘ └────────────┘ │    │      │
│  │  │  ┌────────────────────────────────────────┐ │    │      │
│  │  │  │ 系统托盘 (通知区域)                      │ │    │      │
│  │  │  └────────────────────────────────────────┘ │    │      │
│  │  └────────────────────────────────────────────┘    │      │
│  └──────────────────────────────────────────────────┘      │
│                                                             │
│  ┌──────────────────────┐                                   │
│  │ 系统托盘图标 + 右键菜单│  启用/禁用/解除绑定/开机自启/退出  │
│  └──────────────────────┘                                   │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 核心技术栈

| 层级 | 技术 | 用途 |
|------|------|------|
| 窗口系统 | Win32 API | 查找任务栏窗口句柄、创建子窗口 |
| 图形渲染 | **Direct2D** + **DirectWrite** | GPU 加速的文字渲染 |
| 通信协议 | **WebSocket**（RFC 6455） | 从 MoeKoeMusic 获取实时数据 |
| 网络库 | **ixwebsocket** | 轻量 C++ WebSocket 客户端库 |
| 配置持久化 | JSON 文件 | 保存用户首选项 |
| 系统托盘 | Win32 Shell API | 托盘图标 + 右键菜单 |

### 2.3 关键性能目标

| 指标 | 目标值 |
|------|--------|
| CPU 占用 (空闲/播放) | < 0.5% / < 2% |
| 内存占用 | < 20 MB |
| 帧率 (歌词滚动) | 30 FPS（人眼舒适） |
| 启动延迟 | < 500 ms（从启动到显示歌词） |
| 渲染延迟 | < 5 ms（从数据到画面） |

---

## 3. 模块设计

### 3.1 歌词数据获取模块

**文件：** `src/websocket_client.cpp/h`

#### 职责

- 连接 MoeKoeMusic 的 WebSocket 服务器（`ws://127.0.0.1:6520`）
- 接收并分发 `lyrics` 和 `playerState` 消息
- 自动重连（断线检测 + 指数退避）

#### 接口设计

```cpp
// websocket_client.h

class WebSocketClient {
public:
    using LyricsCallback = std::function<void(const LyricsData&)>;
    using StateCallback  = std::function<void(const PlayerState&)>;

    WebSocketClient();
    ~WebSocketClient();

    // 连接到 MoeKoeMusic WebSocket 服务
    bool Connect(const std::string& url = "ws://127.0.0.1:6520");

    // 断开连接
    void Disconnect();

    // 注册歌词数据回调
    void OnLyrics(LyricsCallback cb);

    // 注册播放状态回调
    void OnPlayerState(StateCallback cb);

    // 发送控制指令（可选功能）
    void SendControl(const std::string& command);  // toggle/next/prev

    // 连接状态
    bool IsConnected() const;

private:
    void OnMessage(const std::string& raw);
    void ReconnectLoop();

    ix::WebSocket client_;
    LyricsCallback on_lyrics_;
    StateCallback on_state_;
    std::atomic<bool> connected_{false};
    std::thread reconnect_thread_;
};
```

#### 数据流

```
MoeKoeMusic WebSocket Server (:6520)
  │
  │  {"type":"lyrics","data":[{...}]}
  │  {"type":"playerState","data":{"isPlaying":true,"currentTime":12.5}}
  ▼
WebSocketClient::OnMessage()
  │
  ├─ type == "lyrics"      → 解析 → on_lyrics_() 回调
  └─ type == "playerState"  → 解析 → on_state_() 回调
```

#### 重连策略

| 尝试次数 | 等待时间 |
|---------|---------|
| 第 1 次 | 1 秒 |
| 第 2 次 | 2 秒 |
| 第 3 次 | 4 秒 |
| 第 4 次 | 8 秒 |
| 第 5+ 次 | 15 秒（上限） |

---

### 3.2 任务栏嵌入窗口模块

**文件：** `src/taskbar_window.cpp/h`

#### 3.2.1 职责

- 查找 Windows 任务栏窗口句柄（`Shell_TrayWnd`）
- 在任务栏内部创建歌词子窗口（Layered Window）
- 处理窗口消息（`WM_DPICHANGED`、`WM_SETTINGCHANGE` 等）
- 监听任务栏变化（位置/大小/显隐）并自适应调整

#### 3.2.2 核心原理：将歌词窗口嵌入任务栏

**关键技术：** 使用 `SetParent` 将歌词窗口设为任务栏窗口的子窗口，使歌词窗口在视觉上成为任务栏的一部分。

```
Windows 任务栏窗口层次:
Shell_TrayWnd (任务栏主窗口)
  ├── ReBarWindow32 (工具栏容器)
  │   ├── MSTaskSwWClass (任务按钮区)
  │   └── SearchBox / SearchBoxEx (搜索框)
  ├── Shell_SecondaryTrayWnd (系统托盘/通知区域)
  └── [我们的歌词子窗口] ← 通过 SetParent 嵌入
```

#### 3.2.3 查找任务栏窗口句柄

```cpp
HWND FindTaskbarHandle() {
    // 方法1：通过窗口类名查找
    HWND hTaskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    if (hTaskbar) return hTaskbar;

    // 方法2：通过 AppBar 消息查找（兼容性更好）
    APPBARDATA abd = { sizeof(abd) };
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
        // AppBar 存在，任务栏窗口一定存在
        return FindWindow(L"Shell_TrayWnd", nullptr);
    }

    return nullptr;
}
```

#### 3.2.4 创建嵌入歌词窗口

```cpp
HWND CreateEmbeddedLyricsWindow(HWND hTaskbar, HINSTANCE hInstance) {
    // 1. 创建 Layered Window（透明、置顶、点击穿透）
    const DWORD exStyle = WS_EX_LAYERED
                        | WS_EX_TRANSPARENT     // 鼠标点击穿透
                        | WS_EX_NOACTIVATE;     // 点击不激活

    const DWORD style = WS_POPUP | WS_VISIBLE;

    HWND hLyrics = CreateWindowEx(
        exStyle,
        L"TaskbarLyricsClass",
        L"",
        style,
        0, 0, 0, 0,  // 初始位置/尺寸，后续由 PositionWindow() 设置
        hTaskbar,      // 父窗口 = 任务栏
        nullptr,
        hInstance,
        nullptr
    );

    if (!hLyrics) return nullptr;

    // 2. 将窗口设为任务栏的子窗口
    SetParent(hLyrics, hTaskbar);

    // 3. 设置窗口位置（嵌入任务栏内部）
    PositionLyricsInTaskbar(hLyrics, hTaskbar);

    return hLyrics;
}
```

#### 3.2.5 歌词窗口在任务栏内的定位

```cpp
struct TaskbarInfo {
    RECT rect;          // 任务栏区域（屏幕坐标）
    RECT workArea;      // 工作区（排除任务栏）
    enum Position { BOTTOM, TOP, LEFT, RIGHT } position;
    UINT dpi;           // 当前 DPI
};

TaskbarInfo DetectTaskbarPosition(HWND hTaskbar) {
    TaskbarInfo info;

    // 1. 获取任务栏位置
    APPBARDATA abd = { sizeof(abd) };
    SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    info.rect = abd.rc;

    // 2. 获取工作区
    HMONITOR monitor = MonitorFromWindow(hTaskbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(monitor, &mi);
    info.workArea = mi.rcWork;

    // 3. 推断任务栏方位
    if (info.rect.top >= mi.rcWork.bottom)    info.position = BOTTOM;
    else if (info.rect.left >= mi.rcWork.right) info.position = RIGHT;
    else if (info.rect.right <= mi.rcWork.left) info.position = LEFT;
    else                                         info.position = TOP;

    // 4. 获取 DPI
    info.dpi = GetDpiForWindow(hTaskbar);

    return info;
}

void PositionLyricsInTaskbar(HWND hLyrics, HWND hTaskbar) {
    TaskbarInfo info = DetectTaskbarPosition(hTaskbar);

    // 获取任务栏的实际尺寸（用于子窗口坐标计算）
    RECT taskbarRect;
    GetWindowRect(hTaskbar, &taskbarRect);

    // 将屏幕坐标转换为任务栏窗口的客户区坐标
    POINT pt = { 0, 0 };
    ScreenToClient(hTaskbar, &pt);

    int taskbarWidth  = taskbarRect.right - taskbarRect.left;
    int taskbarHeight = taskbarRect.bottom - taskbarRect.top;

    int lyricsWidth, lyricsHeight, lyricsX, lyricsY;

    switch (info.position) {
    case BOTTOM: {
        // Windows 10/11 默认：任务栏在底部
        // 歌词窗口占据任务栏的上部区域（时钟/通知区域上方）
        lyricsWidth  = taskbarWidth;
        lyricsHeight = MulDiv(28, info.dpi, 96);  // 28pt 按 DPI 缩放
        lyricsX = 0;
        lyricsY = 0;  // 任务栏客户区顶部
        break;
    }
    case TOP: {
        lyricsWidth  = taskbarWidth;
        lyricsHeight = MulDiv(28, info.dpi, 96);
        lyricsX = 0;
        lyricsY = taskbarHeight - lyricsHeight;
        break;
    }
    case LEFT: {
        lyricsWidth  = MulDiv(180, info.dpi, 96);
        lyricsHeight = MulDiv(28, info.dpi, 96);
        lyricsX = taskbarWidth - lyricsWidth;
        lyricsY = 0;
        break;
    }
    case RIGHT: {
        lyricsWidth  = MulDiv(180, info.dpi, 96);
        lyricsHeight = MulDiv(28, info.dpi, 96);
        lyricsX = 0;
        lyricsY = 0;
        break;
    }
    }

    SetWindowPos(hLyrics, nullptr,
        lyricsX, lyricsY, lyricsWidth, lyricsHeight,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
```

#### 3.2.6 与"悬浮在上方"方案的关键区别

| 维度 | 悬浮在任务栏上方（旧方案） | 嵌入任务栏内部（本方案） |
|------|------------------------|------------------------|
| **父窗口** | `nullptr`（顶层窗口） | `Shell_TrayWnd`（任务栏） |
| **定位方式** | 屏幕坐标，紧贴任务栏边缘 | 任务栏客户区坐标，在任务栏内部 |
| **视觉融合** | 歌词浮在任务栏上方，有缝隙 | 歌词在任务栏内部，无缝融合 |
| **Z-order** | `WS_EX_TOPMOST` 置顶 | 继承任务栏 Z-order |
| **任务栏隐藏** | 窗口留在屏幕上 | 窗口随任务栏一起隐藏 |
| **全屏应用** | 窗口遮挡全屏内容 | 窗口随任务栏一起隐藏 |
| **点击穿透** | 必须 `WS_EX_TRANSPARENT` | 可选（任务栏区域本身可交互） |

#### 3.2.7 消息处理

```cpp
LRESULT CALLBACK TaskbarWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // 初始化渲染器
        break;

    case WM_DPICHANGED: {
        // DPI 变化时重新计算位置
        HWND hTaskbar = GetParent(hwnd);
        PositionLyricsInTaskbar(hwnd, hTaskbar);
        // 通知渲染器更新 DPI
        break;
    }

    case WM_SETTINGCHANGE:
        // 任务栏可能变化（隐藏/显示/移动/大小调整）
        if (wParam == SPI_SETWORKAREA) {
            HWND hTaskbar = GetParent(hwnd);
            PositionLyricsInTaskbar(hwnd, hTaskbar);
        }
        break;

    case WM_DISPLAYCHANGE:
        // 分辨率变化
        HWND hTaskbar = GetParent(hwnd);
        PositionLyricsInTaskbar(hwnd, hTaskbar);
        break;

    case WM_TIMER:
        // 定时刷新歌词进度
        RenderFrame();
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
```

#### 3.2.8 任务栏变化监听

除了被动响应 `WM_SETTINGCHANGE`，还需要**主动轮询**检测任务栏尺寸变化（某些场景下系统不发送通知）：

```cpp
// 在 WM_TIMER 中每 500ms 检查一次任务栏尺寸
void CheckTaskbarResize(HWND hLyrics, HWND hTaskbar) {
    static RECT lastRect = {0};
    RECT currentRect;
    GetWindowRect(hTaskbar, &currentRect);

    if (!EqualRect(&lastRect, &currentRect)) {
        lastRect = currentRect;
        PositionLyricsInTaskbar(hLyrics, hTaskbar);
    }
}
```

#### 3.2.9 窗口刷新机制

- 使用 `WM_TIMER` 定时器，间隔 **33ms**（≈30 FPS）
- 仅在歌词或进度变化时触发 `UpdateLayeredWindow`
- 空闲状态（无歌词或无播放）停止定时器以节省资源

---

### 3.3 歌词同步与解析模块

**文件：** `src/lyrics_parser.cpp/h`

#### 3.3.1 职责

- 解析 MoeKoeMusic 的 JSON 歌词数据（逐字符时间轴格式）
- 维护当前歌词索引和时间同步
- 输出当前应显示的歌词文本和进度

#### 3.3.2 数据结构

```cpp
// 歌词单字时间轴（MoeKoeMusic 原始格式）
struct CharacterTiming {
    std::string ch;          // 字符
    int64_t startTime;       // 开始时间 (ms)
    int64_t endTime;         // 结束时间 (ms)
};

// 歌词行
struct LyricLine {
    std::string text;        // 完整行文本
    std::string translated;  // 翻译文本（可选）
    std::vector<CharacterTiming> characters;  // 逐字时间轴
};

// 完整歌词数据
struct LyricsData {
    std::vector<LyricLine> lines;
};

// 播放器状态
struct PlayerState {
    bool isPlaying;
    double currentTime;        // 当前进度 (秒)
    std::string songTitle;     // 当前歌曲标题（可选，用于调试）
};

// 当前渲染状态（由解析器计算）
struct RenderState {
    std::string currentLine;       // 当前行文本
    std::string currentTranslated; // 当前行翻译
    double progress;               // 当前行进度 0.0 ~ 1.0
    int currentLineIndex;          // 当前行索引
};
```

#### 3.3.3 核心逻辑

```cpp
class LyricsParser {
public:
    LyricsParser();

    // 更新歌词数据（从 WebSocket 接收时调用）
    void UpdateLyrics(const LyricsData& data);

    // 更新播放状态
    void UpdatePlayerState(const PlayerState& state);

    // 计算当前应显示的渲染状态（主循环每帧调用）
    RenderState GetCurrentRenderState() const;

    // 歌词是否有效
    bool HasLyrics() const;

private:
    LyricsData lyrics_;
    PlayerState state_;

    // 二分查找当前进度对应的歌词行
    int FindLineIndex(double currentTimeSec) const;
};
```

**GetCurrentRenderState 实现原理：**

```
1. 如果 !isPlaying → 保持当前显示状态
2. 根据 currentTime 二分查找 lyrics_.lines 中对应的行索引
3. 在该行的 characters 中，计算当前字符所处的进度位置：
   progress = (currentTime - startTime) / (endTime - startTime)
4. 如果 currentTime 超出最后一行 → 显示空行或"播放结束"
5. 返回 {currentLine, currentTranslated, progress, lineIndex}
```

#### 3.3.4 LRC 歌词兼容（备用方案）

虽然优先使用 WebSocket 推送的 JSON 数据，但保留 LRC 解析能力作为备选：

```cpp
// LRC 解析 (备用)
struct LrcLine {
    double timeSec;
    std::string text;
};

std::vector<LrcLine> ParseLRC(const std::string& lrcContent) {
    std::vector<LrcLine> result;
    std::regex pattern(R"(\[(\d{2}):(\d{2})\.(\d{2,3})\](.*))");

    std::istringstream stream(lrcContent);
    std::string line;
    std::smatch match;

    while (std::getline(stream, line)) {
        if (std::regex_match(line, match, pattern)) {
            int minutes = std::stoi(match[1]);
            int seconds = std::stoi(match[2]);
            double frac;
            if (match[3].length() == 3)  // [mm:ss.xxx]
                frac = std::stoi(match[3]) / 1000.0;
            else                          // [mm:ss.xx]
                frac = std::stoi(match[3]) / 100.0;

            result.push_back({
                minutes * 60 + seconds + frac,
                match[4]
            });
        }
    }

    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) { return a.timeSec < b.timeSec; });
    return result;
}
```

---

### 3.4 渲染引擎模块

**文件：** `src/renderer.cpp/h`

#### 3.4.1 职责

- 初始化 Direct2D 和 DirectWrite 工厂
- 为 Layered Window 创建兼容的渲染目标
- 绘制歌词文本（含卡拉 OK 高亮进度）
- 管理 GPU 资源生命周期

#### 3.4.2 初始化流程

```cpp
class TaskbarRenderer {
public:
    bool Initialize(HWND hwnd);
    void Resize(UINT width, UINT height, UINT dpi);
    void Render(const RenderState& state);
    void Shutdown();

private:
    HWND hwnd_;

    // Direct2D 资源
    ID2D1Factory* d2d_factory_{nullptr};
    ID2D1HwndRenderTarget* render_target_{nullptr};

    // DirectWrite 资源
    IDWriteFactory* dwrite_factory_{nullptr};
    IDWriteTextFormat* text_format_{nullptr};

    // 歌词窗口尺寸
    UINT width_, height_;
    UINT dpi_;
};
```

#### 3.4.3 渲染流程

```cpp
void TaskbarRenderer::Render(const RenderState& state) {
    if (!render_target_) return;

    render_target_->BeginDraw();
    render_target_->Clear(D2D1::ColorF(0, 0, 0, 0)); // 完全透明背景

    if (!state.currentLine.empty()) {
        // 绘制卡拉 OK 高亮效果：
        //   - 前半部分（已唱）: 高亮色 (如 #FF6B81 / 主题红)
        //   - 后半部分（未唱）: 柔白色 (Alpha 0.6)
        DrawHighlightLyric(
            text_format_,
            state.currentLine,
            state.progress,
            D2D1::ColorF(1.0f, 0.42f, 0.50f),   // 高亮色
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.6f) // 柔白色
        );

        // 如果有翻译文本，在下方以小号字体显示
        if (!state.currentTranslated.empty()) {
            DrawTranslatedLyric(state.currentTranslated);
        }
    }

    render_target_->EndDraw();

    // 更新 Layered Window
    UpdateLayeredWindowIndirect();
}
```

#### 3.4.4 卡拉 OK 高亮实现

```cpp
void DrawHighlightLyric(
    IDWriteTextFormat* format,
    const std::wstring& text,
    double progress,                    // 0.0 ~ 1.0
    D2D1_COLOR_F highlightColor,
    D2D1_COLOR_F normalColor)
{
    // 创建文本布局
    IDWriteTextLayout* layout = nullptr;
    dwrite_factory_->CreateTextLayout(
        text.c_str(), (UINT32)text.length(),
        format, width_, height_, &layout);

    // 设置前半部分颜色（已唱）- 从头到 progress 百分比
    DWRITE_TEXT_RANGE highlightRange = { 0, (UINT32)(text.length() * progress) };
    layout->SetDrawingEffect(&highlightColor, highlightRange);

    // 设置后半部分颜色（未唱）
    DWRITE_TEXT_RANGE normalRange = {
        highlightRange.length,
        (UINT32)(text.length() - highlightRange.length)
    };
    layout->SetDrawingEffect(&normalColor, normalRange);

    // 绘制
    render_target_->DrawTextLayout(
        D2D1::Point2F(margin_, centerY_),
        layout,
        nullptr,  // 使用 layout 自带的颜色
        D2D1_DRAW_TEXT_OPTIONS_NONE
    );

    layout->Release();
}
```

> **备选方案：** 对于不支持 `SetDrawingEffect` 的场景，可以使用 `DrawText` 分段绘制：先绘制整行灰色文字，再用 `PushAxisAlignedClip` + 高亮色绘制裁剪后的前半部分。

---

### 3.5 配置管理模块

**文件：** `src/config.cpp/h`

#### 3.5.1 职责

- 读取/保存用户配置（JSON 文件）
- 管理启用/禁用状态
- 管理开机自启动（Windows 注册表 Run key）
- 提供默认值

#### 3.5.2 配置项

```json
{
  "enabled": true,
  "auto_start": true,
  "appearance": {
    "highlight_color": "#FF6B81",
    "normal_color": "#FFFFFF",
    "normal_opacity": 0.6,
    "font_family": "Microsoft YaHei UI",
    "font_size": 14,
    "enable_karaoke": true,
    "enable_translation": true
  },
  "advanced": {
    "websocket_port": 6520,
    "refresh_rate_hz": 30,
    "debug_log": false
  }
}
```

#### 3.5.3 自启动管理

```cpp
class Config {
public:
    void Load();
    void Save();

    bool IsEnabled() const { return enabled_; }
    void SetEnabled(bool value);

    bool IsAutoStart() const { return auto_start_; }
    void SetAutoStart(bool value);

private:
    bool SetAutoStartRegistry(bool enable);
    std::string GetConfigPath() const;

    bool enabled_{true};      // 默认启用
    bool auto_start_{true};   // 默认开机自启
    // ... 其他配置项
};

bool Config::SetAutoStartRegistry(bool enable) {
    HKEY hKey;
    LONG result = RegOpenKeyEx(
        HKEY_CURRENT_USER,
        TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        0, KEY_SET_VALUE, &hKey
    );
    if (result != ERROR_SUCCESS) return false;

    if (enable) {
        TCHAR exePath[MAX_PATH];
        GetModuleFileName(nullptr, exePath, MAX_PATH);
        result = RegSetValueEx(
            hKey,
            TEXT("MoeKoeTaskbarLyrics"),
            0, REG_SZ,
            (BYTE*)exePath,
            (DWORD)(_tcslen(exePath) + 1) * sizeof(TCHAR)
        );
    } else {
        RegDeleteValue(hKey, TEXT("MoeKoeTaskbarLyrics"));
    }

    RegCloseKey(hKey);
    return true;
}
```

#### 3.5.4 配置文件位置

```
%APPDATA%/MoeKoeTaskbarLyrics/config.json
```

---

### 3.6 系统托盘模块

**文件：** `src/tray_icon.cpp/h`

#### 3.6.1 职责

- 在系统托盘区创建图标
- 提供右键菜单（启用/禁用、开机自启、退出等）
- 显示歌词提示（鼠标悬停时）

#### 3.6.2 菜单结构

**独立模式：**

```
┌──────────────────────┐
│ 🎵 当前歌词...        │  ← 禁用状态，仅显示当前歌词文本
├──────────────────────┤
│ ✅ 启用歌词显示       │  ← 勾选状态（默认开启）
│ ✅ 开机自动启动       │  ← 勾选状态（默认开启）
├──────────────────────┤
│ 重新连接              │
├──────────────────────┤
│ 退出                  │
└──────────────────────┘
```

**绑定模式：**

```
┌──────────────────────┐
│ 🔗 已绑定 MoeKoeMusic│  ← 显示绑定状态
├──────────────────────┤
│ 🎵 当前歌词...        │
├──────────────────────┤
│ ✅ 启用歌词显示       │
│ ✅ 开机自动启动       │
├──────────────────────┤
│ ⏳ 等待 MoeKoeMusic.. │  ← 仅在 MoeKoeMusic 未运行时显示
├──────────────────────┤
│ 解除绑定 🔓          │  ← 转为独立模式
├──────────────────────┤
│ 退出                  │
└──────────────────────┘
```

> 点击"解除绑定"后，插件会删除注册表 Run key，退出当前进程。下次启动时不再检查 MoeKoeMusic 目录。用户如需重新绑定，只需将插件复制回 MoeKoeMusic 目录即可。

#### 3.6.3 菜单命令处理

```cpp
void OnTrayMenuCommand(UINT menuId) {
    switch (menuId) {
    case ID_MENU_ENABLE: {
        bool newState = !config_.IsEnabled();
        config_.SetEnabled(newState);
        config_.Save();

        if (!newState) {
            // 用户禁用 → 退出进程
            PostQuitMessage(0);
        }
        // 用户启用 → 确保歌词窗口已创建
        break;
    }
    case ID_MENU_AUTOSTART: {
        bool newState = !config_.IsAutoStart();
        config_.SetAutoStart(newState);
        config_.Save();
        break;
    }
    case ID_MENU_UNBIND: {
        // 解除绑定：删除注册表 Run key + 标记配置
        config_.SetAutoStart(false);
        config_.SetEnabled(false);
        config_.Save();
        // 删除注册表键
        HKEY hKey;
        RegOpenKeyEx(HKEY_CURRENT_USER,
            TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
            0, KEY_SET_VALUE, &hKey);
        RegDeleteValue(hKey, TEXT("MoeKoeTaskbarLyrics"));
        RegCloseKey(hKey);
        // 退出进程
        PostQuitMessage(0);
        break;
    }
    case ID_MENU_RECONNECT: {
        wsClient_.Disconnect();
        wsClient_.Connect();
        break;
    }
    case ID_MENU_EXIT: {
        // 绑定模式下：仅退出进程，下次登录时 Run key 会重启
        // 独立模式下：正常退出
        PostQuitMessage(0);
        break;
    }
    }
}
```

---

### 3.7 进程监控模块

**文件：** `src/process_monitor.cpp/h`

#### 3.7.1 职责

- 检测插件是否处于**绑定模式**（与 `MoeKoeMusic.exe` 在同一目录）
- 轮询检测 `MoeKoeMusic.exe` 进程的启动和退出
- 绑定模式下提供"随 MoeKoeMusic 启停"的生命周期管理

#### 3.7.2 绑定模式检测原理

```
插件 EXE 目录/
├── MoeKoeTaskbarLyrics.exe     ← 插件自身
├── MoeKoeMusic.exe             ← 如果存在 → 绑定模式
├── ... (其他 MoeKoeMusic 文件)
```

当插件检测到同目录下存在 `MoeKoeMusic.exe` 时，自动进入**绑定模式**：

- **插件随系统启动**（注册表 Run key），在后台等待 MoeKoeMusic 启动
- **MoeKoeMusic 启动后** → 插件自动连接并显示歌词
- **MoeKoeMusic 退出后** → 插件自动退出
- 下次用户登录时，插件再次通过 Run key 启动，进入等待循环

#### 3.7.3 接口设计

```cpp
// process_monitor.h

class ProcessMonitor {
public:
    ProcessMonitor();
    ~ProcessMonitor();

    // 检测是否处于绑定模式
    // 检查同目录下是否存在 MoeKoeMusic.exe
    static bool IsBoundMode();

    // 开始监控目标进程
    // onProcessStarted: 目标进程启动时回调
    // onProcessExited:  目标进程退出时回调
    void Start(
        const std::wstring& exeName,
        std::function<void()> onProcessStarted,
        std::function<void()> onProcessExited
    );

    void Stop();
    bool IsTargetRunning() const;

private:
    bool CheckProcessRunning();
    void MonitorLoop();

    std::wstring exe_name_;
    std::atomic<bool> running_{false};
    std::atomic<bool> target_running_{false};
    std::function<void()> on_started_;
    std::function<void()> on_exited_;
    std::thread monitor_thread_;
};
```

#### 3.7.4 核心实现

```cpp
bool ProcessMonitor::IsBoundMode() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::filesystem::path selfPath(exePath);
    std::filesystem::path targetPath = selfPath.parent_path() / L"MoeKoeMusic.exe";

    return std::filesystem::exists(targetPath);
}

bool ProcessMonitor::CheckProcessRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe_name_.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return found;
}

void ProcessMonitor::MonitorLoop() {
    while (running_) {
        bool currentlyRunning = CheckProcessRunning();

        if (currentlyRunning && !target_running_) {
            // MoeKoeMusic 刚启动 → 回调
            target_running_ = true;
            if (on_started_) on_started_();
        } else if (!currentlyRunning && target_running_) {
            // MoeKoeMusic 刚退出 → 回调 → 退出插件
            target_running_ = false;
            if (on_exited_) on_exited_();
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
```

#### 3.7.5 绑定模式 vs 独立模式

| 特性 | 绑定模式 | 独立模式 |
|------|---------|---------|
| 触发条件 | 插件与 MoeKoeMusic.exe 同目录 | 插件在任意其他目录 |
| 启动方式 | 注册表 Run key（开机自启） | 用户手动启动 |
| 生命周期 | 随 MoeKoeMusic 启停 | 常驻系统托盘 |
| 进程监控 | 每 2 秒轮询 `MoeKoeMusic.exe` | 不监控 |
| 退出行为 | MoeKoeMusic 退出 → 插件退出 | 用户点击"退出" |

---

### 3.8 主程序入口

**文件：** `src/main.cpp`

```cpp
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nShow) {
    // 1. 声明 DPI 感知（Per-Monitor V2）
    SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 2. 初始化配置 & 检查开关状态
    Config config;
    config.Load();

    // 3. 检测绑定模式
    bool isBoundMode = ProcessMonitor::IsBoundMode();

    if (!config.IsEnabled()) {
        // 用户已禁用 → 仅显示托盘图标，等待用户重新启用
        TrayIcon tray;
        tray.Initialize(hInstance);
        tray.SetMenuCallback([&](UINT id) {
            if (id == ID_MENU_ENABLE) {
                config.SetEnabled(true);
                config.SetAutoStart(true);
                config.Save();
                // 需要重启才能生效（简化处理）
                MessageBox(nullptr,
                    L"已启用任务栏歌词，请重新启动程序。",
                    L"MoeKoe Taskbar Lyrics",
                    MB_OK | MB_ICONINFORMATION);
                PostQuitMessage(0);
            }
        });
        MSG msg = {};
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        return 0;
    }

    // 4. 注册窗口类
    RegisterLyricsWindowClass(hInstance);

    // 5. 创建系统托盘
    TrayIcon tray;
    tray.Initialize(hInstance);
    tray.SetMenuCallback(OnTrayCommand);

    if (isBoundMode) {
        // === 绑定模式 ===
        // 启动进程监控，等待 MoeKoeMusic 启动后再初始化
        ProcessMonitor monitor;
        monitor.Start(L"MoeKoeMusic.exe",
            [&]() {
                // MoeKoeMusic 启动 → 正式初始化
                HWND hTaskbar = FindTaskbarHandle();
                if (!hTaskbar) return;

                TaskbarWindow taskbarWindow;
                taskbarWindow.Create(hInstance, hTaskbar);

                TaskbarRenderer renderer;
                renderer.Initialize(taskbarWindow.GetHandle());

                LyricsParser lyricsParser;
                WebSocketClient wsClient;
                wsClient.OnLyrics([&](const LyricsData& data) {
                    lyricsParser.UpdateLyrics(data);
                });
                wsClient.OnPlayerState([&](const PlayerState& state) {
                    lyricsParser.UpdatePlayerState(state);
                });
                wsClient.Connect();

                // 存储全局指针供主循环使用
                g_taskbarWindow = &taskbarWindow;
                g_renderer = &renderer;
                g_lyricsParser = &lyricsParser;
                g_wsClient = &wsClient;
            },
            [&]() {
                // MoeKoeMusic 退出 → 退出插件
                PostQuitMessage(0);
            }
        );

        // 消息循环（绑定模式）
        MSG msg = {};
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if (g_taskbarWindow && msg.message == WM_TIMER) {
                g_taskbarWindow->CheckResize();
                auto renderState = g_lyricsParser->GetCurrentRenderState();
                g_renderer->Render(renderState);
                tray.UpdateTooltip(renderState.currentLine);
            }
        }

        // 退出（MoeKoeMusic 已退出）
        if (g_wsClient) g_wsClient->Disconnect();
        if (g_renderer) g_renderer->Shutdown();
        monitor.Stop();
        return 0;
    } else {
        // === 独立模式 ===
        // 6. 查找任务栏窗口
        HWND hTaskbar = FindTaskbarHandle();
        if (!hTaskbar) {
            MessageBox(nullptr,
                L"未找到 Windows 任务栏，请确认系统正常运行。",
                L"MoeKoe Taskbar Lyrics",
                MB_OK | MB_ICONERROR);
            return 1;
        }

        // 7. 在任务栏内创建嵌入歌词窗口
        TaskbarWindow taskbarWindow;
        taskbarWindow.Create(hInstance, hTaskbar);

        // 8. 初始化渲染器
        TaskbarRenderer renderer;
        renderer.Initialize(taskbarWindow.GetHandle());

        // 9. 初始化 WebSocket 客户端
        LyricsParser lyricsParser;
        WebSocketClient wsClient;
        wsClient.OnLyrics([&](const LyricsData& data) {
            lyricsParser.UpdateLyrics(data);
        });
        wsClient.OnPlayerState([&](const PlayerState& state) {
            lyricsParser.UpdatePlayerState(state);
        });
        wsClient.Connect();

        // 10. 消息循环
        MSG msg = {};
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);

            // 每帧更新
            if (msg.message == WM_TIMER) {
                // 检查任务栏尺寸变化
                taskbarWindow.CheckResize();

                // 渲染歌词
                auto renderState = lyricsParser.GetCurrentRenderState();
                renderer.Render(renderState);
                tray.UpdateTooltip(renderState.currentLine);
            }
        }

        // 11. 清理
        renderer.Shutdown();
        wsClient.Disconnect();

        return 0;
    }
}

// 绑定模式下的全局指针（简化实现）
// 对于 MVP，这是可接受的方案
static TaskbarWindow* g_taskbarWindow = nullptr;
static TaskbarRenderer* g_renderer = nullptr;
static LyricsParser* g_lyricsParser = nullptr;
static WebSocketClient* g_wsClient = nullptr;
```

---

## 4. 协议与接口

### 4.1 WebSocket 协议

**地址：** `ws://127.0.0.1:6520`

MoeKoeMusic 在端口 6520 提供 WebSocket 服务（定义于 `electron/services/apiService.js`），采用 JSON 文本帧通信。

#### 服务端推送

**歌词数据：**

```json
{
  "type": "lyrics",
  "data": [
    {
      "text": "你好",
      "translated": "Hello",
      "characters": [
        {"char": "你", "startTime": 12345, "endTime": 12678},
        {"char": "好", "startTime": 12678, "endTime": 13000}
      ]
    },
    {
      "text": "世界",
      "translated": "World",
      "characters": [
        {"char": "世", "startTime": 14000, "endTime": 14300},
        {"char": "界", "startTime": 14300, "endTime": 14600}
      ]
    }
  ]
}
```

**播放器状态：**

```json
{
  "type": "playerState",
  "data": {
    "isPlaying": true,
    "currentTime": 12.5
  }
}
```

#### 客户端指令（可选实现）

```json
{"type": "control", "data": {"command": "toggle"}}
{"type": "control", "data": {"command": "next"}}
{"type": "control", "data": {"command": "prev"}}
```

#### WebSocket 消息处理时序

```
时间线:
│
├─ [连接]    wsClient.Connect("ws://127.0.0.1:6520")
│            ← 收到 {"type":"welcome", "data":"..."}
│
├─ [切歌]    ← 收到 {"type":"lyrics", "data":[...]}
│            → lyricsParser.UpdateLyrics()
│            → 重置当前行索引为 0
│
├─ [播放中]  ← 收到 {"type":"playerState","data":{"isPlaying":true,"currentTime":12.5}}
│            → lyricsParser.UpdatePlayerState()
│            → 渲染线程计算进度并绘制
│
├─ [暂停]    ← 收到 {"type":"playerState","data":{"isPlaying":false,"currentTime":30.0}}
│            → 渲染线程保持当前显示
│
├─ [断线]    连接断开
│            → 启动重连（1s → 2s → 4s → ...）
│            → 窗口显示 "等待连接..."
│
└─ [重连]    连接恢复
            → 等待新一轮歌词推送
```

#### 数据格式注意事项

| 字段 | 说明 |
|------|------|
| `characters[].startTime` | 单位：**毫秒** (ms) |
| `characters[].endTime` | 单位：**毫秒** (ms) |
| `currentTime` | 单位：**秒** (s)，浮点数 |
| `text` | UTF-8 编码 |
| `translated` | UTF-8 编码，可能为空 |
| 歌词数组为空 | 表示当前歌曲无歌词 |

---

## 5. 构建与部署

### 5.1 环境要求

| 工具 | 版本要求 |
|------|---------|
| Windows SDK | 10.0.20348+ (Windows 11 SDK) |
| Visual Studio | 2022 (v143 工具集) |
| CMake | 3.20+ |
| vcpkg | 最新版 |

### 5.2 依赖管理

使用 vcpkg 管理第三方依赖：

```bash
# 安装依赖
vcpkg install ixwebsocket     # WebSocket 客户端库
vcpkg install directx-headers # Direct2D 头文件
vcpkg install nlohmann-json   # JSON 解析（可选，也可手写）

# 将 vcpkg 集成到 CMake
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
```

### 5.3 构建脚本

**CMakeLists.txt：**

```cmake
cmake_minimum_required(VERSION 3.20)
project(MoeKoeTaskbarLyrics VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖
find_package(ixwebsocket REQUIRED)
find_package(nlohmann_json REQUIRED)

# Direct2D 是 Windows SDK 的一部分
# 只需链接 d2d1.lib, dwrite.lib, windowscodecs.lib

# 源文件
set(SOURCES
    src/main.cpp
    src/websocket_client.cpp
    src/lyrics_parser.cpp
    src/taskbar_window.cpp
    src/renderer.cpp
    src/config.cpp
    src/process_monitor.cpp
    src/tray_icon.cpp
)

# 可执行目标
add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
    d2d1.lib
    dwrite.lib
    windowscodecs.lib
    gdi32.lib          # 用于 UpdateLayeredWindow
    shell32.lib        # 用于 SHAppBarMessage
    user32.lib
    comctl32.lib
    advapi32.lib       # 用于注册表操作
)
```

**构建命令：**

```bash
# 配置
cmake -B build -S . ^
    -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build --config Release

# 产物: build/Release/MoeKoeTaskbarLyrics.exe
# 单文件可执行，无需额外 DLL（静态链接）
```

### 5.4 部署流程

插件提供两种使用模式，用户可根据需要选择：

**模式 A：绑定模式（推荐）**

1. 用户安装 MoeKoeMusic Windows 版
2. 将 `MoeKoeTaskbarLyrics.exe` **复制到 MoeKoeMusic 的安装目录**（与 `MoeKoeMusic.exe` 同目录）
3. 双击运行（首次运行自动注册开机自启 + 绑定状态）
4. 插件自动在后台等待 MoeKoeMusic 启动
5. 打开 MoeKoeMusic 播放音乐 → **任务栏内部**显示卡拉 OK 歌词
6. 关闭 MoeKoeMusic → 插件自动退出
7. 下次用户登录 → 插件通过 Run key 自动启动，重复步骤 4-6

**模式 B：独立模式**

1. 将 `MoeKoeTaskbarLyrics.exe` 放置到**任意目录**
2. 双击运行，插件以独立模式启动
3. 常驻系统托盘，手动管理启停

### 5.5 用户控制

所有控制通过系统托盘右键菜单完成：

| 操作 | 效果 |
|------|------|
| 取消勾选"启用歌词显示" | 删除注册表 Run key + 写入 `enabled:false` → 进程退出 |
| 勾选"启用歌词显示" | 写入 `enabled:true` + 注册表 Run key → 重启后生效 |
| 取消勾选"开机自动启动" | 仅删除注册表 Run key，插件仍保持启用 |
| 点击"解除绑定"（绑定模式） | 删除注册表 Run key + 写入 `enabled:false` → 进程退出 |
| 点击"退出" | 进程退出（绑定模式下下次登录时 Run key 重启） |

### 5.6 卸载

1. 右键托盘图标 → 取消勾选"启用歌词显示"（自动清理注册表）
2. 右键托盘图标 → 退出
3. 删除 `MoeKoeTaskbarLyrics.exe` 文件

---

## 6. 扩展与维护

### 6.1 兼容性保障

| 场景 | 策略 |
|------|------|
| MoeKoeMusic 更新 | 只要 WebSocket 协议不变，插件无需更新 |
| WebSocket 协议变化 | 在插件中做 JSON 字段存在性检查 |
| 多显示器 | 使用 `MonitorFromWindow` + `GetMonitorInfo` 定位 |
| 任务栏隐藏/自动隐藏 | 子窗口随任务栏一起隐藏，无需额外处理 |
| 任务栏位置变化 | 主动轮询 + `WM_SETTINGCHANGE` 双重检测 |
| Windows 深色/浅色主题 | 可检测 `UISettings.GetColorValue` 自动适配主题色 |
| Windows 11 任务栏居中 | `SetParent` 方案兼容居中任务栏 |
| 绑定模式安装路径 | 插件只需与 `MoeKoeMusic.exe` 同目录，不影响文件结构 |
| 绑定模式与独立模式切换 | 通过"解除绑定"菜单项或手动移动 EXE 文件位置 |

### 6.2 潜在风险与缓解

| 风险 | 缓解方案 |
|------|---------|
| MoeKoeMusic 关闭后 WebSocket 连接失败 | 优雅显示"等待连接..."，不崩溃 |
| 歌词数据异常（空字符、超长时间戳） | 增加数据校验，异常时显示"暂无歌词" |
| 高 DPI 下文字渲染模糊 | 使用 DirectWrite 的 DPI 感知渲染 |
| GPU 资源泄漏 | RAII 管理 Direct2D 资源，退出时释放 |
| 任务栏窗口句柄获取失败 | 回退为悬浮窗口方案（`SetParent(nullptr)`） |
| Windows 更新改变任务栏结构 | 主动轮询检测 + 优雅降级 |

### 6.3 未来功能扩展

| 功能 | 难度 | 优先级 |
|------|------|--------|
| 歌词滚动动画（平滑移动） | 中 | ⭐⭐⭐ |
| 双行歌词显示（上一行+当前行） | 低 | ⭐⭐⭐ |
| 自定义字体和颜色（配置界面） | 低 | ⭐⭐ |
| Chrome Extension 开关 UI（v2.0） | 中 | ⭐⭐ |
| 歌词搜索（当 WebSocket 无数据时） | 高 | ⭐ |
| 绑定模式高级检测（不依赖文件名） | 中 | ⭐⭐ |
| 支持其他播放器（Foobar2000 等） | 高 | ⭐ |
| 绑定模式自动修复（MoeKoeMusic 路径变更） | 中 | ⭐ |

### 6.4 调试与日志

- 使用 `OutputDebugString` 输出调试信息
- 日志文件位置：`%TEMP%\MoeKoeTaskbarLyrics\debug.log`
- 可通过配置 `"debug_log": true` 启用详细日志

---

## 7. 附录

### 7.1 参考文档

| 主题 | 链接 |
|------|------|
| MoeKoeMusic 源码 | https://github.com/MoeKoeMusic/MoeKoeMusic |
| Electron WebSocket 服务 | `electron/services/apiService.js` |
| Layered Windows 官方文档 | https://learn.microsoft.com/en-us/windows/win32/winmsg/layered-windows |
| Direct2D 官方教程 | https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-tutorial |
| DirectWrite 文字渲染 | https://learn.microsoft.com/en-us/windows/win32/directwrite/text-formatting-and-layout |
| High DPI 桌面应用 | https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows |
| SHAppBarMessage | https://learn.microsoft.com/en-us/windows/win32/shell/abm-gettaskbarpos |
| ixwebsocket | https://github.com/machinezone/IXWebSocket |
| Taskbar-Lyrics 参考项目 | https://gitcode.com/gh_mirrors/ta/Taskbar-Lyrics |

### 7.2 字段格式对照

| WebSocket JSON 字段 | 类型 | 示例 | 说明 |
|---------------------|------|------|------|
| `type` | string | `"lyrics"` | 消息类型 |
| `data[].text` | string | `"你好世界"` | 歌词行文本 |
| `data[].translated` | string | `"Hello World"` | 翻译文本 |
| `data[].characters[].char` | string | `"你"` | 单个字符 |
| `data[].characters[].startTime` | int64 | `12345` | 开始时间 (ms) |
| `data[].characters[].endTime` | int64 | `12678` | 结束时间 (ms) |
| `data.isPlaying` | bool | `true` | 播放状态 |
| `data.currentTime` | double | `12.5` | 当前进度 (秒) |

### 7.3 MoeKoeMusic 端口清单

| 端口 | 用途 | 协议 |
|------|------|------|
| 6520 | WebSocket 服务（歌词 + 播放状态 + 控制） | WebSocket |
| 6521 | KuGou Music API（HTTP REST） | HTTP |

---

> **本文档为开发草案，将在实现过程中持续更新。**  
> 如有疑问，请参考 MoeKoeMusic 源码仓库或提交 Issue 讨论。
