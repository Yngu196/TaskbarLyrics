// SPDX-License-Identifier: GPL-3.0
// logger.cpp - 统一日志系统实现
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <windows.h>

// 局部屏蔽 fopen 的 C4996 警告（本文件仅在此处使用不安全 API，其余项目已移除全局 _CRT_SECURE_NO_WARNINGS）
#pragma warning(disable: 4996)

namespace moekoe {

namespace {

std::string g_logPath;
bool      g_enabled = true;   // 默认开启，InitLogger 后由 config 覆盖
std::mutex g_logMutex;        // 线程安全：WebSocket 线程可能同时写日志

// 日志轮转：超过此大小（字节）时备份旧日志
constexpr long long MAX_LOG_SIZE = 5 * 1024 * 1024;  // 5 MB

void RotateLogIfNeeded() {
    // 检查当前日志文件大小
    WIN32_FILE_ATTRIBUTE_DATA attrData;
    if (!::GetFileAttributesExA(g_logPath.c_str(), GetFileExInfoStandard, &attrData)) {
        return;  // 文件不存在或无权限，跳过
    }
    LARGE_INTEGER fileSize;
    fileSize.LowPart = attrData.nFileSizeLow;
    fileSize.HighPart = attrData.nFileSizeHigh;
    if (fileSize.QuadPart < MAX_LOG_SIZE) return;

    // 删除旧的备份（如果存在）
    std::string backupPath = g_logPath;
    size_t dotPos = backupPath.rfind('.');
    if (dotPos != std::string::npos) {
        backupPath.insert(dotPos, ".1");
    } else {
        backupPath += ".1";
    }
    // 关闭当前文件（如果有的话）以便重命名 — 我们通过锁保证串行，直接重命名即可
    ::MoveFileExA(g_logPath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

} // namespace

void InitLogger() {
    wchar_t exePath[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *slash = L'\0';
    std::wstring dir(exePath);
    int len = ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(),
                                    static_cast<int>(dir.size()), nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        std::string dirUtf8(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(),
                              static_cast<int>(dir.size()), &dirUtf8[0], len, nullptr, nullptr);
        g_logPath = dirUtf8 + "\\debug.log";
    }
}

void SetLogEnabled(bool enabled) {
    g_enabled = enabled;
}

void Log(const char* fmt, ...) {
    if (!g_enabled || g_logPath.empty()) return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    RotateLogIfNeeded();
    FILE* f = fopen(g_logPath.c_str(), "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

void Log(const std::string& msg) {
    if (!g_enabled || g_logPath.empty()) return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    RotateLogIfNeeded();
    FILE* f = fopen(g_logPath.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s", msg.c_str());
    fclose(f);
}

} // namespace moekoe
