// SPDX-License-Identifier: GPL-3.0
// single_instance.h - 单实例保护与宿主重载时的实例接管
//
// 背景：MoeKoeMusic 的 nativeHostManager 在重载插件时，会先给旧 EXE 发
// {"type":"shutdown"}，但旧 EXE 的 HTTP/WebSocket 收尾需要一两秒；而
// extensions.js 的 restartExtensions() 没有等待清理完成就开始初始化，
// 新 EXE 往往在旧 EXE 真正退出之前就被 spawn。
//
// 旧逻辑遇到 ERROR_ALREADY_EXISTS 时直接 return 0。宿主把这记作
// "本地程序已退出 <plugin>/<host>: 0"，删除 processes 记录且不会再次自动
// 拉起，于是出现「插件已加载但没有任何 EXE 存活」。
//
// 本模块把这段判定抽出来：宿主托管启动时限时等待旧实例释放命名互斥体再
// 接管；独立运行（用户手动双击）保持原来的「重复实例秒退」语义。
#pragma once

#include <windows.h>

namespace moekoe {

// 默认互斥体名，保持与历史版本一致（同一用户会话内互斥）。
constexpr wchar_t kInstanceMutexName[] = L"MoeKoeTaskbarLyrics_Mutex";

// 宿主托管启动下等待旧实例退出的上限。正常优雅退出约 2s，宿主自身在
// 2s 后还会 tree-kill 兜底，10s 已覆盖卡在收尾流程的极端情况。
constexpr DWORD kInstanceTakeoverTimeoutMs = 10000;
constexpr DWORD kInstanceTakeoverPollMs    = 50;

// 宿主 nativeHostManager 用 spawn(..., stdio:['pipe','pipe','pipe']) 启动，
// 根目录 launcher 再通过 STARTF_USESTDHANDLES 把同一组管道透传给真实程序。
// 要求 stdin 与 stdout 同时是管道，避免把「从 shell 用管道喂输入」或
// 「CI 重定向 stdout」误判成托管启动。
bool IsHostManagedLaunch();

enum class InstanceAcquireResult {
    Acquired,             // 成功占用单实例名字（首次启动，或旧实例退出后接管）
    DuplicateStandalone,  // 独立运行遇到重复实例：调用方应直接退出
    Error                 // CreateMutexW 失败，详见 InstanceLock::lastError
};

struct InstanceLockOptions {
    const wchar_t* mutexName = kInstanceMutexName;
    bool   managed   = false;
    DWORD  timeoutMs = kInstanceTakeoverTimeoutMs;
    DWORD  pollMs    = kInstanceTakeoverPollMs;
};

struct InstanceLock {
    HANDLE handle = nullptr;   // 成功时由调用方持有，进程退出时内核自动关闭
    InstanceAcquireResult result = InstanceAcquireResult::Error;
    DWORD waitedMs  = 0;       // 等待旧实例退出所花的时间
    DWORD lastError = 0;       // 仅 Error 时有意义
    // 等待超时后仍继续启动并持有句柄：宁可两个实例短暂共存，也不能让
    // 宿主收到 exit(0) 后注销进程记录且不再拉起。此时调用方应记警告。
    bool timedOut = false;
};

// 获取单实例名字。托管模式下遇到重复实例会轮询重试 CreateMutexW：命名
// 内核对象在最后一个句柄关闭（即旧进程完全退出）后销毁，重试成功就精确
// 意味着旧实例已消失，不需要额外的 PID 共享或跨进程事件。
InstanceLock AcquireInstanceLock(const InstanceLockOptions& options = {});

void ReleaseInstanceLock(HANDLE handle);

} // namespace moekoe
