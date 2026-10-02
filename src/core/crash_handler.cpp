// SPDX-License-Identifier: GPL-3.0
// crash_handler.cpp - 全局未处理异常过滤器实现
//
// 从 main.cpp 提取，在进程崩溃时输出异常代码、故障模块、调用栈到日志。
//
#include "core/crash_handler.h"

#include "util/logger.h"

namespace moekoe {

// 日志快捷方式（崩溃路径必须走 LogCrash：
// 常规 Log() 抢 g_logMutex，崩溃线程可能正持有该锁导致过滤器死锁；
// 且 debugLog=false 时常规 Log 被 g_enabled 门控丢弃，崩溃信息会丢失）
using moekoe::LogCrash;

// 全局异常过滤器
LONG WINAPI GlobalExceptionHandler(EXCEPTION_POINTERS* ep) {
    Log("[CRASH] Unhandled exception code=0x%08lX at address=%p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);

    // 输出故障模块名
    if (ep->ExceptionRecord->ExceptionAddress) {
        HMODULE hMod = nullptr;
        if (::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress),
                &hMod)) {
            wchar_t modName[MAX_PATH] = {};
            ::GetModuleFileNameW(hMod, modName, MAX_PATH);
            Log("[CRASH] Faulting module: %ls\n", modName);
        }
    }

    // 输出调用栈（WalkHelperStack64）
    static const int kMaxFrames = 32;
    void* frames[kMaxFrames];
    USHORT nFrames = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    Log("[CRASH] Stack trace (%hu frames):\n", nFrames);
    for (USHORT i = 0; i < nFrames; ++i) {
        HMODULE hMod = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(frames[i]), &hMod);
        wchar_t modName[MAX_PATH] = {};
        if (hMod) ::GetModuleFileNameW(hMod, modName, MAX_PATH);
        DWORD offset = hMod
            ? static_cast<DWORD>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(hMod))
            : 0;
        Log("[CRASH]   #%02u %ls + 0x%08lX (%p)\n", i, hMod ? modName : L"<unknown>", offset, frames[i]);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace moekoe
