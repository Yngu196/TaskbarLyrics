// SPDX-License-Identifier: GPL-2.0
// logger.cpp - 统一日志系统实现
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <windows.h>

namespace moekoe {

namespace {

std::string g_logPath;
bool      g_enabled = true;   // 默认开启，InitLogger 后由 config 覆盖
std::mutex g_logMutex;        // 线程安全：WebSocket 线程可能同时写日志

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
    FILE* f = fopen(g_logPath.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s", msg.c_str());
    fclose(f);
}

} // namespace moekoe
