// SPDX-License-Identifier: GPL-3.0
// tray_commands.h - 托盘/歌词窗口菜单命令处理
//
#pragma once

#include "core/app_context.h"
#include <windows.h>

namespace moekoe {

// 菜单命令处理（托盘菜单 + 歌词窗口右键菜单共用的命令分支）
void OnTrayCommand(AppContext& app, UINT menuId);

} // namespace moekoe
