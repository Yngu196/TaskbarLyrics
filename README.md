<p align="center">
  <img src="icons/icon256.png" width="200" alt="Taskbar Lyrics" />
</p>

<h1 align="center">MoeKoeMusic TaskbarLyrics</h1>

<p align="center">
  <a href="https://github.com/Yngu196/TaskbarLyrics/releases/tag/v1.0.7">
    <img src="https://img.shields.io/badge/release-v1.0.7-blue" alt="release" />
  </a>
  <a href="https://github.com/Yngu196/TaskbarLyrics?tab=License-1-ov-file">
    <img src="https://img.shields.io/badge/license-GPL--3.0-orange" alt="license" />
  </a>
  <img src="https://img.shields.io/badge/platform-Windows-0078D4" alt="platform" />
</p>

<p align="center">把歌词放进 Windows 任务栏：单行 / 双行歌词、专辑封面、播放控制、歌词翻译与纯音乐频谱。</p>

## 目录

- [项目简介](#项目简介)
- [功能特性](#功能特性)
- [兼容性说明](#兼容性说明)
- [安装与使用](#安装与使用)
- [常见问题](#常见问题)
- [构建与测试](#构建与测试)
- [界面预览](#界面预览)
- [相关文档](#相关文档)
- [许可证](#许可证)

## 项目简介

MoeKoeMusic TaskbarLyrics 是 [MoeKoeMusic](https://github.com/MoeKoeMusic/MoeKoeMusic) 的 Windows 任务栏歌词插件。程序以 Native Host 方式运行，通过本地 HTTP / WebSocket 接收播放状态、歌词与封面数据，并使用 Direct2D / DirectWrite 在任务栏中绘制歌词窗口。

- 主要面向 Windows x64，ARM64 原生版本处于发布测试阶段。
- 源代码使用 C++17，构建系统为 CMake Presets，第三方依赖通过 vcpkg 管理。
- 程序可随 MoeKoeMusic 自动启动和退出，也可以独立运行。

> 正在收集兼容性反馈：欢迎在 [GitHub Issues](https://github.com/Yngu196/TaskbarLyrics/issues) 或 [Discussion](https://github.com/Yngu196/TaskbarLyrics/discussions/17) 提交问题与建议。

## 功能特性

### 显示与渲染

- **单行歌词**：Direct2D + DirectWrite 渲染，支持逐字高亮与卡拉 OK 进度动画。
- **双行歌词**：当前行 + 下一行卡片布局，支持专辑封面异步下载、圆角裁切与模糊背景。
- **纯音乐频谱**：无歌词时显示频谱条，基于 WASAPI 进程级音频采集；柱数、柱宽、颜色、上下限与透明度均可调整。
- **歌词翻译**：解析 KRC `[language:...]` 标签提取翻译内容，支持单行替换或双行对照。
- **切换动画**：行切换淡入淡出，进度变化使用弹簧动画过渡。

### 交互与窗口

- **悬停控制**：鼠标悬停时显示上一首 / 播放暂停 / 下一首按钮，单行模式显示歌曲名，双行模式第一行显示歌曲名、第二行显示歌手，移开后自动恢复。
- **拖动定位**：可在任务栏范围内拖动调整歌词窗口位置。
- **锁定模式**：通过托盘菜单切换“锁定位置”或“完全锁定”。
- **手动窗口宽度**：任务栏宽度检测异常时，可手动指定歌词窗口宽度。
- **上下偏移微调**：支持单行 / 双行歌词垂直微调，适配特殊任务栏布局与多显示器环境。

### 适配与数据

- **多方向任务栏**：支持底部 / 顶部 / 左侧 / 右侧任务栏（非底部方向测试偏少，可能会出现一些问题，欢迎反馈）。
- **多显示器跟随**：按鼠标位置或活动窗口所在显示器绑定任务栏，跨屏移动时自动切换；也可在「设置 → 窗口」中手动锁定显示器。
- **APPBAR 自动隐藏**：任务栏自动隐藏时歌词窗口跟随显隐（该功能已停止维护）。
- **封面与元数据**：兼容 `artist` / `author` 等歌曲字段，歌词、封面、歌名与歌手会在独立消息之间保持合并，避免被进度消息清空。

### 诊断与配置

- **诊断报告**：一键导出系统信息、运行状态与日志，便于排查问题。
- **配置导入 / 导出**：支持导出为 JSON 并在其他设备导入。
- **示例配置**：[TaskbarLyrics-config.json](TaskbarLyrics-config.json) 可直接下载替换，或在设置中导入。

## 兼容性说明

- 本项目可能与部分桌面 / 任务栏美化工具存在兼容问题。
- 若任务栏位置或显隐异常，可先关闭桌面 / 任务栏美化工具再排查。

## 安装与使用

### 前置条件

1. 安装 MoeKoeMusic **1.6.6 或更高版本**。
2. 在 MoeKoeMusic 设置 → 系统中开启 **API 模式**。
3. 修改 API 模式后需要重启 MoeKoeMusic（建议重启 1–2 次）才会生效。

### 方式一：插件市场安装（推荐）

1. 在 MoeKoeMusic 的插件管理页找到「任务栏歌词」。
2. 点击「本地程序授权」。
3. 确认已开启 API 模式，然后重启 MoeKoeMusic。

安装完成后，任务栏歌词程序会随 MoeKoeMusic 自动启动和关闭。

### 方式二：手动安装

将发布包解压到 MoeKoeMusic 的插件目录：

```
%APPDATA%\moekoemusic\extensions
```

解压后目录中应直接包含 `manifest.json` 以及 `x64` / `arm64` 等子目录。随后重启 MoeKoeMusic。

### 方式三：独立运行（不推荐）

双击 `MoeKoeTaskbarLyrics.exe`，通过托盘图标打开设置或退出程序。独立运行时建议开启开机自动启动。

## 常见问题排查

请参阅 [常见问题自查](Docs/常见问题自查.md)。

## 构建与测试

### 构建环境

| 工具 | 版本 |
| --- | --- |
| Windows SDK | 10.0.20348+ |
| Visual Studio | 2022 (v143) |
| MSVC 工具集 | 14.44+（构建 ARM64 需安装 ARM64 工具链） |
| CMake | 3.20+ |
| vcpkg | latest |

### 使用 Visual Studio

1. 安装 Visual Studio 2022，勾选「使用 C++ 的桌面开发」、Windows SDK 与 CMake 工具。
2. 用 Visual Studio 打开项目根目录的 `CMakeLists.txt`，等待 CMake 配置完成。
3. 在顶部配置栏选择 `x64-Release`、`x64-Debug`、`ARM64-Release` 或 `ARM64-Debug`，不要选择「无配置」。
4. 选择「生成 → 生成全部」，或右键 `MoeKoeTaskbarLyrics` 目标并选择「生成」。
5. 调试前关闭旧的 `MoeKoeTaskbarLyrics.exe` 进程，避免输出文件被占用。
6. 测试目标可在「测试资源管理器」中运行。

如果「配置任务 / CMake 设置」不可用，关闭解决方案后重新打开 `CMakeLists.txt`；仍未恢复时删除对应的 `out/build/<preset>` 缓存并重新配置。

### 命令行一键构建（推荐）

```powershell
# x64 Release
.\build.cmd release x64

# ARM64 Release（需安装 VS2022 ARM64 工具链与 arm64-windows 依赖）
.\build.cmd release arm64

# 全量构建发布包：x64 → ARM64 → 架构校验 → 打包
.\build.cmd release all

# 清理
.\build.cmd clean x64
```

构建脚本会显式传入对应 triplet 的 `VCPKG_INSTALLED_DIR`：

- x64 → `<vcpkg>\installed\x64-windows`
- ARM64 → `<vcpkg>\installed\arm64-windows`

这样可以确保第三方库与目标架构一致，避免跨 triplet fallback。

### 手动构建

```powershell
# 安装依赖（ixwebsocket / nlohmann-json / zlib / mbedtls / kissfft）
vcpkg install --triplet x64-windows
vcpkg install --triplet arm64-windows

# x64
cmake --preset x64-Release -DVCPKG_INSTALLED_DIR="D:\vcpkg\installed\x64-windows"
cmake --build --preset x64-Release

# ARM64
cmake --preset ARM64-Release -DVCPKG_INSTALLED_DIR="D:\vcpkg\installed\arm64-windows"
cmake --build --preset ARM64-Release
```

> ixwebsocket 预编译库使用 MSVC 14.44 编译，项目需使用相同版本工具集。`CMakePresets.json` 已配置 `/p:PlatformToolsetVersion=14.44.35207`。

### 测试、校验与打包

```powershell
# 自动化测试
ctest --test-dir out\build\x64-Release -C Release --output-on-failure

# 架构校验：逐文件检查根目录 / Launcher、x64、arm64 的 PE Machine 字段
python scripts\verify_package.py moeKoe-taskbar-lyrics\

# 打包：根目录同时包含 x64/ 与 arm64/ 即为双架构混合包
python scripts\pack_zip.py moeKoe-taskbar-lyrics\ moeKoe-taskbar-lyrics-windows-x64-arm64.zip
```

当前测试覆盖歌词解析、KRC 解析、频谱数学映射与 HTTP 服务生命周期。WASAPI、Direct2D 设备丢失与 Explorer 重启属于真实 Windows 环境集成测试范围。

## 界面预览

| 双行歌词模式 | 单行歌词模式 | 双行歌词（双语切换） |
| :---: | :---: | :---: |
| ![双行歌词模式](Samples/sample1.gif "双行歌词模式") | ![单行歌词模式](Samples/sample2.gif "单行歌词模式") | ![双行歌词模式（歌词双语切换）](Samples/sample3.gif "双行歌词模式（歌词双语切换）") |

| 设置页面 | 歌词窗口菜单 |托盘菜单 |
| :---: | :---: | :---: |
| ![设置页面](Samples/sample4.png "设置页面") | ![菜单](Samples/sample6.png "菜单") | ![菜单](Samples/sample7.png "菜单") |

## 相关文档

- [开发文档](Docs/MoeKoeMusic_TaskbarLyrics_开发文档.md)
- [项目状态文档](Docs/项目状态文档.md)
- [版本更新日志](Docs/版本更新日志.md)
- [常见问题自查](Docs/常见问题自查.md)
- [高 DPI 测试矩阵](Docs/高DPI测试矩阵.md)
- [计划书](Docs/计划书.md)

## 许可证

[GPL-3.0](LICENSE)
