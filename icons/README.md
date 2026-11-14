# Icons

图标资源目录。

## icon256.png

项目主图标（PNG），用于 README 展示与插件市场登记的图标链接，请勿随意移动或重命名。

## icon.ico

应用图标（多分辨率 ICO，建议含 16x16 / 32x32 / 48x48），用于原生 Win32 层：

- 经 `src/app_icon.rc` 嵌入 EXE 资源；
- 构建时由 CMake POST_BUILD 复制到输出目录 `resources/` 子目录，供托盘图标运行时加载（`tray_icon.cpp` 按运行时路径 `resources\icon.ico` 读取）。

若未提供 icon.ico，程序将回退使用系统默认应用图标 (`IDI_APPLICATION`)。
