<p align="center">
  <img src="moeKoe-taskbar-lyrics/icons/icon256.png" width="200" alt="Taskbar Lyrics" />
</p>

<h1 align="center">MoeKoeMusic TaskbarLyrics</h1>

<p align="center">
  <img src="https://img.shields.io/badge/release-v0.5.0-blue" alt="release" />
  <img src="https://img.shields.io/badge/license-GPL--3.0-orange" alt="license" />

</p>

<p align="center">在 Windows 任务栏上显示逐字高亮歌词，支持卡拉OK效果和播放控制</p>

***

## 功能特性

- **Native Host 托管** — 随 MoeKoeMusic 自动启动/关闭，无需手动管理
- **卡拉 OK 效果** — 基于 Direct2D + DirectWrite 渲染，逐字高亮渐变
- **悬停控制按钮** — 鼠标悬停歌词时显示 ⏮ ⏸/▶ ⏭
- **拖动定位** — 可在任务栏范围内自由拖动调整位置
- **锁定模式** — 托盘菜单切换锁定位置 / 完全锁定
- **APPBAR 自动隐藏** — 任务栏自动隐藏时歌词窗口跟随显隐
- **高 DPI 适配** — Per-Monitor V2 DPI Awareness
- **多方向任务栏** — 支持底部 / 顶部 / 左侧 / 右侧任务栏 (待测试)
- **6 种预设主题色** — 一键切换高亮颜色和普通歌词颜色
- **跑马灯滚动** — 长歌词自动滚动（bounce / loop / off 三种模式）
- **WebView2 / D2D 双模式设置界面** — 现代化 WebView2 HTML 设置页或纯 Direct2D 自绘原生界面，可随时切换并记住选择，暗色模式自动适配，实时预览
- **卡片样式显示** — 双行歌词 + 封面图标（可选），无卡拉OK效果，独立字号和颜色配置

## 使用方式

### 作为 MoeKoeMusic 插件（推荐）

将发布的压缩包解压后复制到 MoeKoeMusic 的插件目录：

```
MoeKoeMusic/plugins/extensions/moeKoe-taskbar-lyrics/
```

或者直接在 MoeKoeMusic 的 插件市场 安装此插件。

然后在 MoeKoeMusic 插件管理页找到「任务栏歌词」→ 点击「本地程序授权」。授权后，程序将随 MoeKoeMusic 自动启动/关闭（此功能需MoeKoeMusic版本>1.6.5）。

### 独立运行

双击 `MoeKoeTaskbarLyrics.exe`，右键托盘图标操作。

## 构建环境

| 工具               | 版本          |
| ---------------- | ----------- |
| Windows SDK      | 10.0.26100+ |
| Visual Studio    | 2022 (v143) |
| MSVC 工具集         | 14.44+      |
| CMake            | 3.20+       |
| vcpkg            | latest      |
| WebView2 Runtime | 已安装         |

## 构建

```powershell
# 安装依赖
vcpkg install ixwebsocket:x64-windows-142 nlohmann-json:x64-windows-142

# 构建
cmake --preset x64-Release
cmake --build --preset x64-Release

# 打包发布
.\scripts\package.ps1
```

> **注意**：由于 ixwebsocket 预编译库使用 MSVC 14.44 编译，项目需要使用相同版本工具集。`CMakePresets.json` 已配置自动传递 `/p:PlatformToolsetVersion=14.44.35207`。

## v0.5.0 变更

### 架构重构

- 拆分 TaskbarWindow 为 ShellCompanion（全屏检测 + 歌词窗口管理）+ TaskbarWindow（纯任务栏子窗口操作）
- ShellCompanion 管理歌词 HWND 生命周期与全屏显隐逻辑，TaskbarWindow 专注 `SetParent` / `SetWindowPos` / 位置缓存

新增：封面渲染增强、歌词切换动画、频谱、原生设置...

修复：全屏隐藏失效、退出全屏后位置偏移、插件无法开启MoeKoeMusic的api模式、不支持垂直任务栏等问题

<details>
<summary>展开历史版本（v0.4.1 / v0.4.0）</summary>

## v0.4.1 变更

### 优化：卡片模式歌词切换动画

重新设计卡片模式的歌词切换动画，从"三行卷轴式 + 字号缩放"改为**双轨淡入淡出 + 独立位移**：

- **旧当前行**：直接消失（无淡出），避免同位置重影
- **旧下一行**：向上滑入当前行位置 + 淡出（`EaseInOutQuad`）
- **新当前行**：从下方滑入 + 淡入（`EaseOutBack` 带轻微回弹 + `EaseOutCubic`）
- **新下一行**：同步淡入
- 新增 `DrawCardLyricsSingle` 单行绘制方法（独立控制透明度/偏移/字号/颜色）
- 新增 `EaseInOutQuad` / `EaseOutBack` 缓动函数
- 动画时长调整为 500ms（更流畅不拖沓）

### 新增：卡片样式显示模式

参考 [ANYNC/TaskbarLyrics](https://github.com/ANYNC/TaskbarLyrics) 以及酷狗音乐的任务栏歌词的视觉风格，新增可切换的卡片样式显示模式：

- **双行歌词布局** — 当前行 + 下一行预览，无卡拉OK逐字效果
- **封面图标区域** — 左侧显示专辑封面（支持异步下载）或音乐符号降级图标
- **独立字号配置** — 当前行（10\~20pt）和下一行（8\~18pt）字号独立可调
- **独立颜色配置** — 当前行/下一行文字颜色分别可自定义
- **无文字阴影** — 卡片模式使用纯色绘制，避免黑边问题
- **设置界面分组** — 切换显示模式时，仅显示当前模式的选项

### 修复

- 修复切换显示模式后保存导致位置重置的问题
- 修复 `display_mode` 等字段未在 C++ 配置同步中传递的 bug
- 封面图下载改为异步（`std::thread`），不再阻塞渲染线程
- 移除位置偏移滑块（拖动窗口直接调整更直观），保留重置按钮

## v0.4.0 变更

### 核心变更：Native Host 托管模式

从 Chrome 标准 Native Messaging 迁移到 MoeKoeMusic 自定义 `moekoe_native_hosts` 机制：

- manifest.json 新增 `moekoe_permissions: ["moekoe:nativeHost"]` + `moekoe_native_hosts` 声明
- 支持 `auto_start: true` 实现随 MoeKoeMusic 自动启动
- 新增 `native-bridge.html` / `native-bridge.js` 隐藏桥接页
- JSON Lines 通信协议替代 Chrome NMH 4 字节前缀协议
- 移除旧版 `native_launcher` 字段和 Chrome NMH 注册表配置

### 其他变更

- manifest 最低版本要求更新为 `1.6.6`
- popup 界面移除启动/停止按钮，更新为授权流程说明
- background.js 新增 Native Host Bridge Port 管理层
- C++ EXE 新增 `NativeMessagingHost` 类封装协议层
- stdin EOF 不再触发退出，支持独立运行模式

## 开发文档

- [MoeKoeMusic\_TaskbarLyrics\_开发文档.md](MoeKoeMusic_TaskbarLyrics_开发文档.md)
- [项目状态文档.md](项目状态文档.md)
- [计划书.md](计划书.md)

## 许可证

[GPL-3.0](LICENSE)
