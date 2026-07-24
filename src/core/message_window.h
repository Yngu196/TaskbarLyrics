// SPDX-License-Identifier: GPL-3.0
// message_window.h - 隐式消息窗口（接收托盘消息 + 帧定时器）
//
#pragma once

#include "core/app_context.h"
#include <windows.h>

namespace moekoe {

// 隐式消息窗口过程(用于接收托盘消息 + WM_FRAME_TICK)
LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 注册一个不显示的 message-only 类
bool RegisterMessageClass(HINSTANCE hInst);

} // namespace moekoe
