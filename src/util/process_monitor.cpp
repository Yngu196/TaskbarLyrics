// SPDX-License-Identifier: GPL-3.0
// process_monitor.cpp - 进程监控模块实现
#include "util/process_monitor.h"
#include "core/constants.h"
#include "util/logger.h"

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
    std::filesystem::path targetPath = selfPath.parent_path() / L"MoeKoeMusic.exe";

    return std::filesystem::exists(targetPath);
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
        DWORD waitResult = ::WaitForSingleObject(
            monitorThread_.native_handle(),
            moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
        if (waitResult == WAIT_TIMEOUT) {
            moekoe::Log("[PM] Monitor thread join timed out (%d ms), detaching\n",
                       moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
            monitorThread_.detach();
        } else {
            monitorThread_.join();
        }
    }
}

} // namespace moekoe
