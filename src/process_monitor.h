// SPDX-License-Identifier: GPL-2.0
// process_monitor.h - 进程监控模块
//
// 职责:
//   - 检测插件是否处于绑定模式（与 MoeKoeMusic.exe 同目录）
//   - 轮询检测 MoeKoeMusic.exe 进程的启动和退出
//   - 绑定模式下提供"随 MoeKoeMusic 启停"的生命周期管理
//
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace moekoe {

class ProcessMonitor {
public:
    ProcessMonitor();
    ~ProcessMonitor();

    ProcessMonitor(const ProcessMonitor&) = delete;
    ProcessMonitor& operator=(const ProcessMonitor&) = delete;

    // 检测是否处于绑定模式（同目录下存在 MoeKoeMusic.exe）
    static bool IsBoundMode();

    // 开始监控目标进程
    void Start(const std::wstring& exeName,
               std::function<void()> onProcessStarted,
               std::function<void()> onProcessExited);

    void Stop();

    bool IsTargetRunning() const { return targetRunning_.load(); }

private:
    bool CheckProcessRunning();
    void MonitorLoop();

    std::wstring exeName_;
    std::atomic<bool> running_{false};
    std::atomic<bool> targetRunning_{false};
    std::function<void()> onStarted_;
    std::function<void()> onExited_;
    std::thread monitorThread_;
};

} // namespace moekoe
