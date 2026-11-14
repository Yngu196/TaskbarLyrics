// SPDX-License-Identifier: GPL-3.0
// single_instance.cpp - 单实例保护与宿主重载时的实例接管
#include "core/single_instance.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace moekoe {

namespace {

bool IsPipeHandle(HANDLE handle) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    // GetFileType 返回 FILE_TYPE_UNKNOWN 时可能是有效句柄，也可能是无效句柄；
    // 这里只关心「是不是管道」，因此不额外用 GetLastError 区分。
    return ::GetFileType(handle) == FILE_TYPE_PIPE;
}

} // namespace

bool IsHostManagedLaunch() {
    return IsPipeHandle(::GetStdHandle(STD_INPUT_HANDLE)) &&
           IsPipeHandle(::GetStdHandle(STD_OUTPUT_HANDLE));
}

InstanceLock AcquireInstanceLock(const InstanceLockOptions& options) {
    InstanceLock lock;
    const ULONGLONG start = ::GetTickCount64();
    // pollMs 为 0 会让等待变成忙等；测试与外部调用方都不该传 0。
    const DWORD pollMs = options.pollMs ? options.pollMs : 1;

    for (;;) {
        ::SetLastError(NO_ERROR);
        HANDLE handle = ::CreateMutexW(nullptr, FALSE, options.mutexName);
        if (!handle) {
            lock.result = InstanceAcquireResult::Error;
            lock.lastError = ::GetLastError();
            return lock;
        }

        if (::GetLastError() != ERROR_ALREADY_EXISTS) {
            // 首次启动，或旧实例的所有句柄都已关闭、命名对象被内核销毁后
            // 接管成功。这个 handle 必须一直持有到进程退出。
            lock.handle = handle;
            lock.result = InstanceAcquireResult::Acquired;
            lock.waitedMs = static_cast<DWORD>(::GetTickCount64() - start);
            return lock;
        }

        const ULONGLONG elapsed = ::GetTickCount64() - start;

        if (!options.managed) {
            // 独立运行：保持历史行为，第二个实例立即退出。
            ::CloseHandle(handle);
            lock.result = InstanceAcquireResult::DuplicateStandalone;
            lock.waitedMs = static_cast<DWORD>(elapsed);
            return lock;
        }

        if (elapsed >= options.timeoutMs) {
            // 旧实例迟迟不退出。仍然继续启动并持有这个 handle：直接退出
            // 会让宿主把本次 exit(0) 当作本地程序正常结束、注销记录且不再
            // 自动拉起，正是重载后没有 EXE 存活的根因。
            lock.handle = handle;
            lock.result = InstanceAcquireResult::Acquired;
            lock.waitedMs = static_cast<DWORD>(elapsed);
            lock.timedOut = true;
            return lock;
        }

        // 每次探测都必须立刻关闭：否则我们自己持有的这份引用会让命名对象
        // 永不消失，即使旧实例已经退出也等不到接管时机。
        ::CloseHandle(handle);
        ::Sleep(pollMs);
    }
}

void ReleaseInstanceLock(HANDLE handle) {
    if (handle) ::CloseHandle(handle);
}

} // namespace moekoe
