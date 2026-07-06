// SPDX-License-Identifier: GPL-3.0
// logger.h - 统一日志系统
//
// 替代各文件中分散的 DebugLog / ConfigDebugLog 实现。
// 所有模块通过统一的 Log() 接口输出日志到 <exe_dir>/debug.log。
//
// 用法:
//   1. 启动时调用 InitLogger() 初始化日志路径
//   2. 调用 SetLogEnabled(true/false) 控制是否输出（对应 config.debugLog）
//   3. 任意位置: Log("[MODULE] message %s\n", value);
//
#pragma once

#include <cstdarg>
#include <string>

namespace moekoe {

// 初始化日志系统（WinMain 入口处调用一次）
// 解析 EXE 路径，确定 debug.log 文件位置
void InitLogger();

// 设置日志开关（由 config.debugLog 驱动）
void SetLogEnabled(bool enabled);

// 获取当前 debug.log 的绝对路径（UTF-8 编码）
std::string GetLogPath();

// 格式化日志（printf 风格，自动追加换行如需）
void Log(const char* fmt, ...);

// 字符串日志（供 websocket_client 等已格式化字符串的调用方使用）
void Log(const std::string& msg);

} // namespace moekoe
