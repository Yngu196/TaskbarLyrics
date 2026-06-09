# MoeKoeMusic Taskbar Lyrics

> Windows 任务栏的歌词显示插件（v0.3.5）

## 项目简介

这是一个独立运行的 Windows 工具，**不修改 MoeKoeMusic 本体**，通过监听其 WebSocket 服务（端口 6520）实时获取歌词与播放状态，并将歌词作为任务栏上方的浮动窗口进行渲染。

作为 **MoeKoeMusic 插件** 集成，目前支持从插件 popup 界面停止，启动功能待完善。

## 当前状态 (v0.3.5)

### 已完成

- Direct2D 透明窗口渲染 + 逐字高亮
- 悬停显示按钮（上一首/暂停/下一首）
- 拖动定位（约束在任务栏范围内，带视觉边框反馈）
- WebView2 设置界面 + Win32 回退
- 配置持久化（%APPDATA%）
- 6 种预设主题色
- HTTP 接口（ping/shutdown）
- Z-order 三重防护（防止被任务栏覆盖）
- API 模式自动检测与开启（连接失败时自动开启 MoeKoeMusic 的 WS 服务）
- 刷新率最高 120 FPS
- 开机自动启动（注册表/任务计划/启动文件夹三种方式并行）
- 安全加固（命令注入防护、路径验证、CORS 限制）

### 待改进

- 插件 popup 的 EXE 启动方式验证（无法启动）
- 多显示器支持（待测试）
- 歌词缓存（离线显示，待测试）
- 多方向任务栏适配（待测试）
- 字体/颜色/字号/透明度/卡拉OK/翻译 全部可配（实测在应用中切换翻译时，任务栏歌词未切换）
- 长歌词跑马灯滚动（bounce/loop/off 三种模式，待测试）
- Windhawk / Explorer Hook 方案（真正嵌入任务栏的方案后续会考虑的）

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
- HTTP 接口（端口 6523）：ping 检测存活 / shutdown 优雅退出
- 托盘菜单：设置 / 重连 / 解除绑定 / 退出

### 运行模式

- **绑定模式**：EXE 放在 MoeKoeMusic 目录下，随主进程启停（待完善）
- **独立模式**：常驻系统托盘，手动管理生命周期

## 环境要求

| 工具               | 版本          |
| ---------------- | ----------- |
| Windows SDK      | 10.0.26100+ |
| Visual Studio    | 2022 (v143) |
| MSVC 工具集       | 14.44+      |
| CMake            | 3.20+       |
| vcpkg            | latest      |
| WebView2 Runtime | 已安装（设置界面需要） |

## 构建

```powershell
# 安装依赖
vcpkg install ixwebsocket:x64-windows-142 nlohmann-json:x64-windows-142

# 使用预设配置（推荐）
cmake --preset x64-Release
cmake --build --preset x64-Release

# 或使用一键构建脚本
.\build.cmd release

# 构建后自动复制:
#   → resources/ 到输出目录
#   → MoeKoeTaskbarLyrics.exe 到插件目录
#   → WebView2Loader.dll 到输出目录
```

> **注意**：由于 ixwebsocket 预编译库使用 MSVC 14.44 编译，项目需要使用相同版本工具集。`CMakePresets.json` 已配置自动传递 `/p:PlatformToolsetVersion=14.44.35207`。

## 使用方式

### 方式一：独立运行

双击 `MoeKoeTaskbarLyrics.exe`，右键托盘图标操作。

### 方式二：作为 MoeKoeMusic 插件

将 `moeKoe-taskbar-lyrics` 目录复制到：

- 开发版：`MoeKoeMusic/plugins/extensions/moeKoe-taskbar-lyrics/`
- 安装版：`%APPDATA%/moekoemusic/extensions/moeKoe-taskbar-lyrics/`

在 MoeKoeMusic 的插件页面点击"打开插件目录"，打开`moeKoe-taskbar-lyrics`文件夹，双击`MoeKoeTaskbarLyrics.exe`启动。

## 安全说明 (v0.3.5)

本版本进行了多项安全加固：

- **命令注入防护**：自启动路径验证，过滤危险字符
- **HTTP 接口加固**：关闭命令使用 JSON 白名单验证，CORS 限制为 127.0.0.1
- **COM 替代 PowerShell**：创建启动文件夹快捷方式使用 IShellLink COM 接口，避免脚本注入
- **歌词解析限制**：防止恶意歌词导致内存耗尽

## 开发文档

如果你想进一步了解本项目，可以查看 `CODE_REVIEW.md` 了解代码审查和安全分析详情。
