// SPDX-License-Identifier: GPL-2.0
// process_monitor.cpp - 进程监控模块实现
#include "process_monitor.h"

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <filesystem>

namespace moekoe {

ProcessMonitor::ProcessMonitor() = default;

ProcessMonitor::~ProcessMonitor() {
    Stop();
}

bool ProcessMonitor::IsBoundMode() {
    wchar_t exePath[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::filesystem::path selfPath(exePath);
    auto selfDir = selfPath.parent_path();

    // 1) 同目录（原始逻辑：与 MoeKoeMusic.exe 并列）
    if (std::filesystem::exists(selfDir / L"MoeKoeMusic.exe"))
        return true;

    // 2) 父目录（覆盖 extensions/moeKoe-taskbar-lyrics/ 部署场景）
    auto parentDir = selfDir.parent_path();
    if (std::filesystem::exists(parentDir / L"MoeKoeMusic.exe"))
        return true;

    // 3) 父目录的父目录（覆盖更深层嵌套或开发目录结构）
    auto grandparentDir = parentDir.parent_path();
    if (std::filesystem::exists(grandparentDir / L"MoeKoeMusic.exe"))
        return true;

    return false;
}

bool ProcessMonitor::CheckProcessRunning() {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;

    if (::Process32FirstW(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName_.c_str()) == 0) {
                found = true;
                break;
            }
        } while (::Process32NextW(snapshot, &pe));
    }

    ::CloseHandle(snapshot);
    return found;
}

void ProcessMonitor::MonitorLoop() {
    while (running_.load()) {
        bool currentlyRunning = CheckProcessRunning();

        if (currentlyRunning && !targetRunning_.load()) {
            targetRunning_.store(true);
            if (onStarted_) onStarted_();
        } else if (!currentlyRunning && targetRunning_.load()) {
            targetRunning_.store(false);
            if (onExited_) onExited_();
        }

        // 每 2 秒轮询一次
        for (int i = 0; i < 20 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void ProcessMonitor::Start(const std::wstring& exeName,
                            std::function<void()> onProcessStarted,
                            std::function<void()> onProcessExited) {
    exeName_ = exeName;
    onStarted_ = std::move(onProcessStarted);
    onExited_ = std::move(onProcessExited);

    // 初始检测
    targetRunning_.store(CheckProcessRunning());
    if (targetRunning_.load() && onStarted_) {
        onStarted_();
    }

    running_.store(true);
    if (!monitorThread_.joinable()) {
        monitorThread_ = std::thread([this] { MonitorLoop(); });
    }
}

void ProcessMonitor::Stop() {
    running_.store(false);
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
}

} // namespace moekoe
