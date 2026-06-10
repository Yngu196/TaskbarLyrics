# MoeKoeMusic Taskbar Lyrics

> Windows 任务栏的歌词显示插件（v0.3.8）

## 项目简介

这是一个独立运行的 Windows 工具，**不修改 MoeKoeMusic 本体**，通过监听其 WebSocket 服务（端口 6520）实时获取歌词与播放状态，并将歌词作为任务栏上方的浮动窗口进行渲染。

作为 **MoeKoeMusic 插件** 集成，目前支持从插件 popup 界面停止，启动功能待完善。

## 当前状态 (v0.3.8)

### 已完成

- Direct2D 透明窗口渲染 + 逐字高亮
- 悬停显示按钮（上一首/暂停/下一首）
- 拖动定位（约束在任务栏范围内，带视觉边框反馈）
- 锁定位置/完全锁定（托盘菜单切换，锁定位置时仍可点击控制按钮）
- APPBAR 自动隐藏适配（任务栏自动隐藏时歌词窗口跟随显隐）
- WebView2 设置界面 + Win32 回退
- 配置持久化（%APPDATA%）
- 6 种预设主题色
- HTTP 接口（ping/shutdown，含本地鉴权）
- Z-order 三重防护（防止被任务栏覆盖）
- 统一日志系统（`logger.h/cpp`，线程安全，6 处分散实现合并为单一接口）
- API 模式自动检测与开启（连接失败时自动开启 MoeKoeMusic 的 WS 服务）
- 刷新率最高 120 FPS
- 开机自动启动（注册表/任务计划/启动文件夹三种方式并行）
- 安全加固 v2（命令注入防护、路径验证、CORS 限制、本地 Token 鉴权、响应体校验）
- 端口可配置（`config.json` 的 `advanced.http_server_port` 字段，默认 6523）
- OPTIONS 预检请求正确处理（CORS 兼容）
- 插件图标统一为 256px（manifest + 发布包 public/ 结构）
- 打包脚本 `scripts/package.ps1`（生成符合 MoeKoeMusic-Plugins 审核规范的 zip）

### 待改进

- 插件 popup 的 EXE 启动方式验证（无法启动）
- 多显示器支持（待测试）
- 歌词缓存（离线显示，待测试）
- 多方向任务栏适配（待测试）
- 字体/颜色/字号/透明度/卡拉OK/翻译 全部可配（实测在应用中切换翻译时，任务栏歌词未切换）
- 长歌词跑马灯滚动（bounce/loop/off 三种模式，待测试，额，这个走马灯效果好像不怎么好看？）
- Windhawk / Explorer Hook 方案（真正嵌入任务栏的方案后续会考虑的）

## 主要特性

### 核心功能

- **零侵入**：独立 EXE，与 MoeKoeMusic 完全解耦
- **卡拉 OK 效果**：基于 Direct2D + DirectWrite 渲染，逐字高亮渐变
- **悬停控制按钮**：鼠标悬停歌词时显示 ⏮ ⏸/▶ ⏭
- **拖动定位**：可在任务栏范围内左右/上下拖动调整位置
- **锁定位置**：托盘菜单切换锁定，锁定后禁止拖动但保留按钮操作
- **完全锁定**：禁止所有鼠标交互（含悬停和按钮）
- **APPBAR 自动隐藏**：检测任务栏自动隐藏状态，歌词窗口跟随显隐
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
- HTTP 接口（端口 6523，可配置）：ping 检测存活 / shutdown 优雅退出 / 播放控制
- 本地 Token 鉴权：所有非 OPTIONS 请求需携带 `X-MoeKoe-Token` 头，防止本地其他进程劫持
- 托盘菜单：设置 / 重连 / 锁定位置 / 完全锁定 / 解除绑定 / 退出
- 许可证：GPL-2.0（LICENSE 文件）

### 运行模式

- **绑定模式**：EXE 放在 MoeKoeMusic 目录下，随主进程启停（待完善）
- **独立模式**：常驻系统托盘，手动管理生命周期

## 环境要求

| 工具               | 版本          |
| ---------------- | ----------- |
| Windows SDK      | 10.0.26100+ |
| Visual Studio    | 2022 (v143) |
| MSVC 工具集         | 14.44+      |
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

# 打包发布（生成符合 MoeKoeMusic-Plugins 审核规范的 zip）
.\scripts\package.ps1
# 输出: moeKoe-taskbar-lyrics.zip（内含 public/ 目录结构）
```

> **注意**：由于 ixwebsocket 预编译库使用 MSVC 14.44 编译，项目需要使用相同版本工具集。`CMakePresets.json` 已配置自动传递 `/p:PlatformToolsetVersion=14.44.35207`。如果说您在自己的设备上编译时出现环境问题的话，请尽量自己解决，因为我自己的设备上环境太乱了，我也很头大，等哪天固态价格降到合适的时候，我一定买个512或者1T的，然后重新配置环境。

## 使用方式

### 方式一：独立运行

双击 `MoeKoeTaskbarLyrics.exe`，右键托盘图标操作。

### 方式二：作为 MoeKoeMusic 插件

将 `moeKoe-taskbar-lyrics` 目录复制到：

- 开发版：`MoeKoeMusic/plugins/extensions/moeKoe-taskbar-lyrics/`
- 安装版：`%APPDATA%/moekoemusic/extensions/moeKoe-taskbar-lyrics/`

在 MoeKoeMusic 的插件页面点击"打开插件目录"，打开`moeKoe-taskbar-lyrics`文件夹，双击`MoeKoeTaskbarLyrics.exe`启动。

## 安全说明 (v0.3.8)

本版本进行了多项安全加固：

### v0.3.5 基础加固

- **命令注入防护**：自启动路径验证，过滤危险字符
- **HTTP 接口加固**：关闭命令使用 JSON 白名单验证，CORS 限制为 127.0.0.1
- **COM 替代 PowerShell**：创建启动文件夹快捷方式使用 IShellLink COM 接口，避免脚本注入
- **歌词解析限制**：防止恶意歌词导致内存耗尽

### v0.3.6 新增加固

- **本地 Token 鉴权**：HTTP 端点要求 `X-MoeKoe-Token` 自定义头，防止本地其他进程绑定同端口后劫持控制（OPTIONS 预检请求自动跳过）
- **响应体校验**：shutdown 端点不再仅凭 HTTP 200 判断成功，必须返回 `{"status":"shutting_down"}` 确定性 JSON 体
- **端口可配置**：HTTP 监听端口可通过 `config.json` 的 `advanced.http_server_port` 自定义（默认 6523），CORS `Allow-Origin` 自动使用实际监听端口
- **onMessage 兜底处理**：Chrome Extension 的消息监听器所有分支均显式调用 sendResponse，default 路径返回 false 避免告警
- **发布包清理**：`.gitignore` 排除 `*.WebView2/` 目录，防止开发者环境路径泄露到发布包

### v0.3.7 变更

- **卡拉OK平滑插值**：新增基于 QueryPerformanceCounter 的本地高精度时钟插值，播放状态下用 `effectiveTime = currentTime + wallElapsed` 推算当前进度，使逐字高亮在 playerState 低频更新时仍能每帧平滑推进（10 秒上限防异常）
- **QPF 缓存优化**：`QueryPerformanceFrequency` 缓存为 `static const` 局部变量，避免每帧冗余系统调用
- **RenderState 时间一致性**：`out.currentTime` 改为输出插值后的 `effectiveTime`，与实际渲染所用时间同步
- **编译修复**：修复 `CheckLocalAuthToken` 未声明 `port` 参数的 C2065 错误；消除 `settings_window.cpp` 未引用参数 C4100 警告

### v0.3.8 变更

- **APPBAR 自动隐藏适配**：通过 `SHAppBarMessage(ABM_GETSTATE)` 检测任务栏自动隐藏状态，歌词窗口跟随任务栏显隐（鼠标进出任务栏区域触发）
- **锁定位置 / 完全锁定功能**：托盘菜单新增"锁定位置"和"完全锁定"选项；锁定位置禁止拖动但保留按钮操作；完全锁定禁止所有鼠标交互
- **统一日志系统**：新建 `logger.h/cpp` 模块，将 6 处分散的 DebugLog/ConfigDebugLog 实现合并为单一 `moekoe::Log()` 接口；线程安全（`std::mutex`）；消除 api_enabler.cpp 中硬编码路径的安全风险
- **移除"启用歌词显示"功能**：删除托盘菜单项、设置页面开关、禁用模式启动分支；程序启动后始终完整初始化所有模块
- **锁定按钮交互修复**：修正位置锁定模式下仍可点击上一首/下一首/暂停按钮
- **GPL-2.0 许可证文件**：添加 LICENSE 文件，GitHub 自动识别显示

## 开发文档

如果你想进一步了解本项目，可以查看 `MoeKoeMusic_TaskbarLyrics_开发文档.md` 和 `项目状态文档.md` 了解代码项目详情。
