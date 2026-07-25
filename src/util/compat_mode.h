// SPDX-License-Identifier: GPL-3.0
// compat_mode.h - 兼容模式定义
//
// 职责:
//   - 定义兼容模式标志（位掩码）
//   - 检测第三方 Shell 修改工具并返回需要启用的兼容标志
//   - 提供标志名称用于日志输出
//
#pragma once

namespace moekoe {

// 兼容模式标志（位掩码，可组合）
enum CompatFlag : unsigned {
    CompatNone           = 0,
    CompatNoFrameLock    = 1 << 0,  // 禁用帧锁定双采样
    CompatNoUIA          = 1 << 1,  // 禁用 UIA 枚举，回退到 EnumChildWindows
    CompatSlowReposition = 1 << 2,  // 延长 WinEvent 重定位延迟（16ms → 100ms）
};

// 检测第三方 Shell 修改工具并返回需要启用的兼容标志
unsigned DetectCompatMode();

// 兼容标志名称（用于日志输出），返回静态字符串
const char* CompatFlagName(unsigned flag);

} // namespace moekoe
