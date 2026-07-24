// SPDX-License-Identifier: GPL-3.0
// crash_handler.h - 全局未处理异常过滤器（崩溃调用栈输出）
//
#pragma once

#include <windows.h>

namespace moekoe {

// 全局异常过滤器：输出异常代码、故障模块、调用栈到日志
LONG WINAPI GlobalExceptionHandler(EXCEPTION_POINTERS* ep);

} // namespace moekoe
