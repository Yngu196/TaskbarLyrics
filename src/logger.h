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
//   4. 需要区分严重性时使用 LogWarn/LogError/LogFatal
//
// 日志级别:
//   - Info  : 常规信息（默认输出）
//   - Warn  : 警告（如线程 join 超时 detach、封面下载失败）
//   - Error : 错误（如 HTTP listen 失败、COM 初始化失败）
//   - Fatal : 致命错误（如崩溃前的关键状态）
//   通过 SetLogLevel() 设置最低输出级别，低于此级别的日志被丢弃。
//
// 日志轮转:
//   debug.log 超过 5MB 时自动重命名为 debug.1.log，新日志写入新的 debug.log。
//
#pragma once

#include <cstdarg>
#include <string>

namespace moekoe {

// 日志级别（严重性递增）
enum class LogLevel {
    Info,
    Warn,
    Error,
    Fatal
};

// 初始化日志系统（WinMain 入口处调用一次）
// 解析 EXE 路径，确定 debug.log 文件位置
void InitLogger();

// 设置日志开关（由 config.debugLog 驱动）
void SetLogEnabled(bool enabled);

// 设置最低输出级别（默认 Info，输出所有级别）
// 例如 SetLogLevel(LogLevel::Warn) 将丢弃 Info 级别日志
void SetLogLevel(LogLevel level);

// 获取当前 debug.log 的绝对路径（UTF-8 编码）
std::string GetLogPath();

// 格式化日志（printf 风格，Info 级别，向后兼容）
void Log(const char* fmt, ...);

// 字符串日志（供 websocket_client 等已格式化字符串的调用方使用，Info 级别）
void Log(const std::string& msg);

// 带级别的日志函数：输出前添加 [W]/[E]/[F] 前缀
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);
void LogFatal(const char* fmt, ...);

} // namespace moekoe
