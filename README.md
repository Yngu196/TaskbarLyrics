# MoeKoeMusic Taskbar Lyrics

>  Windows 任务栏的歌词显示插件（v0.3.1）

## 项目简介

这是一个独立运行的 Windows 工具，**不修改 MoeKoeMusic 本体**，通过监听其 WebSocket 服务（端口 6520）实时获取歌词与播放状态，并将歌词作为任务栏上方的浮动窗口进行渲染。

作为 **MoeKoeMusic 插件** 集成，支持从插件 popup 界面启动/停止。

## 主要特性

### 核心功能

- **零侵入**：独立 EXE，与 MoeKoeMusic 完全解耦
- **卡拉 OK 效果**：基于 Direct2D + DirectWrite 渲染，逐字高亮渐变
- **悬停控制按钮**：鼠标悬停歌词时显示 ⏮ ⏸/▶ ⏭
- **拖动定位**：可在任务栏范围内左右/上下拖动调整位置
- **高 DPI 适配**：Per-Monitor V2 DPI Awareness
- **多方向任务栏**：支持底部 / 顶部 / 左侧 / 右侧任务栏

### 配置系统

- **持久化存储**：`%APPDATA%\MoeKoeTaskbarLyrics\config.json`
- **WebView2 设置界面**（优先）：现代化 UI，暗色模式自动切换，实时预览
- **Win32 设置界面**（回退）：WebView2 不可用时自动降级
- **可配置项**：
  - 字体、字号、粗细
  - 高亮颜色 / 普通歌词颜色 + 6 种预设主题
  - 不透明度
  - 卡拉OK 开关 / 翻译开关
  - 水平偏移 / 垂直偏移
  - WebSocket 端口 / 刷新率

### 插件集成

- Chrome Extension Manifest V3 格式
- popup.js 通过 `file://` 协议启动 EXE（不依赖宿主 IPC）
- HTTP 接口（端口 6521）：ping 检测存活 / shutdown 优雅退出
- 托盘菜单：设置 / 重连 / 解除绑定 / 退出（未实现绑定模式）

### 运行模式

- **绑定模式**：EXE 放在 MoeKoeMusic 目录下，随主进程启停（目前还未能随主进程启停）
- **独立模式**：常驻系统托盘，手动管理生命周期

## 项目架构

```
MoeKoeTaskbarLyrics.exe (C++ Win32)
├── Lyrics Window (Layered Window + Direct2D)
│   ├── 歌词渲染（居中对齐 + 逐字高亮）
│   ├── 翻译文本渲染
│   ├── 悬停控制按钮（⏮ ⏸ ⏭）
│   └── 拖动定位（约束在任务栏范围内）
├── Tray Icon (系统托盘)
├── Settings Window (WebView2 → Win32 回退)
│   └── 加载 resources/settings.html
├── HTTP Server (端口 6521)
│   ├── GET /ping → {"status":"ok"}
│   └── POST /shutdown → 退出
├── WebSocket Client (连接 :6520)
│   └── 接收歌词数据 + 发送控制指令
├── Process Monitor (绑定模式)
└── Config (%APPDATA%/MoeKoeTaskbarLyrics/config.json)

插件端 (Chrome Extension V3)
├── manifest.json
├── background.js (Service Worker, WS 中转)
├── popup.html / popup.js (启动/停止/控制 UI)
└── MoeKoeTaskbarLyrics.exe
```

## 文件结构

```
src/
├── main.cpp              # 入口、消息循环、IPC 处理
├── taskbar_window.cpp/h  # 歌词窗口创建、定位、鼠标事件、拖动
├── renderer.cpp/h        # Direct2D/DirectWrite 渲染引擎
├── lyrics_parser.cpp/h   # WebSocket 数据解析、进度计算
├── websocket_client.cpp/h# ixWebSocket 封装
├── http_server.cpp/h     # 内嵌 HTTP 服务器（ping/shutdown）
├── settings_window.cpp/h # WebView2 设置界面宿主窗口
├── config_dialog.cpp/h   # Win32 设置对话框（回退方案）
├── config.cpp/h          # JSON 配置读写
├── tray_icon.cpp/h       # 系统托盘图标+菜单
├── process_monitor.cpp/h # 绑定模式进程监控
└── lyrics_data.h         # 共享数据结构定义

resources/
├── settings.html         # WebView2 设置页面（HTML/CSS/JS）
└── icon.ico              # 应用图标
```

## 环境要求

| 工具               | 版本          |
| ---------------- | ----------- |
| Windows SDK      | 10.0.26100+ |
| Visual Studio    | 2022 (v143) |
| CMake            | 3.20+       |
| vcpkg            | latest      |
| WebView2 Runtime | 已安装（设置界面需要） |

## 构建

```bash
# 安装依赖
vcpkg install ixwebsocket nlohmann-json

# 配置（x64 Debug）
cmake -B build -S . ^
    -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build --config x64-Debug

# 构建后自动复制:
#   → resources/ 到输出目录
#   → MoeKoeTaskbarLyrics.exe 到插件目录
#   → WebView2Loader.dll 到输出目录
```

## 使用方式

### 方式一：独立运行

双击 `MoeKoeTaskbarLyrics.exe`，右键托盘图标操作。

### 方式二：作为 MoeKoeMusic 插件

将 `moeKoe-taskbar-lyrics` 目录复制到：

- 开发版：`MoeKoeMusic/plugins/extensions/moeKoe-taskbar-lyrics/`
- 安装版：`%APPDATA%/moekoemusic/extensions/moeKoe-taskbar-lyrics/`

在 MoeKoeMusic 的插件页面点击 popup 启动。

## 当前状态 (v0.3.1)

### 已完成

- [x] Direct2D 透明窗口渲染 + 逐字高亮
- [x] 悬停控制按钮（上一首/暂停/下一首）
- [x] 多方向任务栏适配
- [x] 拖动定位（约束在任务栏范围内，带视觉边框反馈）
- [x] WebView2 设置界面 + Win32 回退
- [x] 配置持久化（%APPDATA%）
- [x] 字体/颜色/字号/透明度/卡拉OK/翻译 全部可配
- [x] 6 种预设主题色
- [x] HTTP 接口（ping/shutdown）
- [x] 插件 popup 启动/停止
- [x] Z-order 三重防护（防止被任务栏覆盖）
- [x] 长歌词跑马灯滚动（bounce/loop/off 三种模式）
- [x] API 模式自动检测与开启（连接失败时自动开启 MoeKoeMusic 的 WS 服务）
- [x] 刷新率最高 120 FPS

### 待改进

- [ ] WebView2 设置界面的实际测试和调试
- [ ] 插件 popup 的 EXE 启动方式验证（file:// 协议是否有效）
- [ ] 多显示器支持
- [ ] 自动隐藏（无歌词时隐藏窗口）
- [ ] 歌词缓存（离线显示）
- [ ] Windhawk / Explorer Hook 方案（真正嵌入任务栏）

