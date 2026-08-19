// SPDX-License-Identifier: GPL-3.0
// taskbar_selector.cpp - 多任务栏选择器实现
//
// 实现要点:
//   - 枚举: EnumWindows 遍历顶层窗口，按类名收集主任务栏 Shell_TrayWnd
//     与各副屏任务栏 Shell_SecondaryTrayWnd，并用 MonitorFromWindow 建立
//     显示器 → 任务栏 映射。枚举带 1s 节流，避免每帧全量遍历。
//   - 决策: 活动窗口显示器优先 → 鼠标显示器兜底 → 主任务栏最终兜底。
//   - 防抖: 候选目标需持续稳定 300ms 才返回切换建议，防止鼠标快速扫过
//     屏幕边界或焦点瞬变导致歌词窗口在任务栏间来回跳动。
#include "taskbar/taskbar_selector.h"
#include "util/logger.h"

#include <algorithm>
#include <cstdio>

namespace moekoe {

namespace {

constexpr auto kEnumInterval   = std::chrono::milliseconds(1000);
constexpr auto kSwitchDebounce = std::chrono::milliseconds(300);

// EnumWindows 回调：收集任务栏句柄（主 + 副屏）
struct CollectCtx {
    std::vector<HWND> handles;
};

BOOL CALLBACK EnumTaskbarProc(HWND hwnd, LPARAM lParam) {
    wchar_t cls[64]{};
    ::GetClassNameW(hwnd, cls, 63);
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 ||
        wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
        auto* ctx = reinterpret_cast<CollectCtx*>(lParam);
        ctx->handles.push_back(hwnd);
    }
    return TRUE;
}

// EnumDisplayMonitors 回调：收集全部显示器句柄（顺序即序号，0-based）
BOOL CALLBACK EnumDisplayMonProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) {
    auto* vec = reinterpret_cast<std::vector<HMONITOR>*>(lParam);
    vec->push_back(hMonitor);
    return TRUE;
}

} // namespace

int TaskbarSelector::MonitorCount() {
    std::vector<HMONITOR> displays;
    ::EnumDisplayMonitors(nullptr, nullptr, EnumDisplayMonProc, reinterpret_cast<LPARAM>(&displays));
    return static_cast<int>(displays.size());
}

std::vector<std::string> TaskbarSelector::MonitorLabels() {
    std::vector<HMONITOR> displays;
    ::EnumDisplayMonitors(nullptr, nullptr, EnumDisplayMonProc, reinterpret_cast<LPARAM>(&displays));

    std::vector<std::string> labels;
    labels.reserve(displays.size());
    for (size_t i = 0; i < displays.size(); ++i) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(displays[i], &mi)) {
            labels.push_back("显示器 " + std::to_string(i + 1));
            continue;
        }
        const int w = mi.rcMonitor.right - mi.rcMonitor.left;
        const int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "显示器 %zu (%dx%d)", i + 1, w, h);
        labels.push_back(buf);
    }
    return labels;
}

void TaskbarSelector::Reset() {
    taskbars_.clear();
    displays_.clear();
    enumerated_ = false;
    debouncing_ = false;
    debounceTarget_ = nullptr;
    lastUsedMouse_ = false;
    // 注意: manualMode_/manualIndex_ 由配置驱动，Reset（显示器变化/Explorer 重启）不重置
}

void TaskbarSelector::EnumerateTaskbars() {
    const auto now = std::chrono::steady_clock::now();
    if (enumerated_ && now - lastEnumTime_ < kEnumInterval) return;
    lastEnumTime_ = now;

    taskbars_.clear();
    CollectCtx ctx;
    ::EnumWindows(EnumTaskbarProc, reinterpret_cast<LPARAM>(&ctx));

    for (HWND h : ctx.handles) {
        wchar_t cls[64]{};
        ::GetClassNameW(h, cls, 63);
        const bool primary = (wcscmp(cls, L"Shell_TrayWnd") == 0);

        TaskbarEntry e;
        e.hwnd    = h;
        e.primary = primary;
        e.monitor = ::MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        taskbars_.push_back(e);
    }

    // 主任务栏排最前，作为最终兜底
    std::stable_sort(taskbars_.begin(), taskbars_.end(),
                     [](const TaskbarEntry& a, const TaskbarEntry& b) {
                         return a.primary && !b.primary;
                     });

    // 刷新全部显示器列表（手动指定模式按此序号索引）
    displays_.clear();
    ::EnumDisplayMonitors(nullptr, nullptr, EnumDisplayMonProc, reinterpret_cast<LPARAM>(&displays_));

    LogDebug("[TASKBAR-SELECTOR] enumerated %zu taskbar(s), %zu monitor(s)\n",
             taskbars_.size(), displays_.size());
    enumerated_ = true;
}

HWND TaskbarSelector::FindByMonitor(HMONITOR mon) const {
    if (!mon) return nullptr;
    for (const auto& e : taskbars_) {
        if (e.monitor == mon) return e.hwnd;
    }
    return nullptr;
}

HWND TaskbarSelector::PrimaryTaskbar() const {
    for (const auto& e : taskbars_) {
        if (e.primary) return e.hwnd;
    }
    return taskbars_.empty() ? nullptr : taskbars_.front().hwnd;
}

bool TaskbarSelector::IsShellOrDesktop(HWND hwnd) const {
    if (!hwnd) return true;
    wchar_t cls[64]{};
    ::GetClassNameW(hwnd, cls, 63);
    return wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

HWND TaskbarSelector::SelectTarget(HWND lyricsWnd, HWND currentBound, bool& outShouldSwitch) {
    outShouldSwitch = false;

    EnumerateTaskbars();
    if (taskbars_.empty()) {
        // 枚举失败（如 Explorer 尚未就绪）→ 保持现状，交由 WM_TIMER/选择器下次重试
        return currentBound;
    }

    // ── 手动指定模式: 锁定到用户所选显示器的任务栏，不参与自动决策/防抖 ──
    if (manualMode_) {
        HWND desired = nullptr;
        if (manualIndex_ >= 0 && manualIndex_ < static_cast<int>(displays_.size())) {
            desired = FindByMonitor(displays_[manualIndex_]);
        }
        if (!desired) {
            // 目标显示器无任务栏（如关闭"在所有显示器显示任务栏"）或索引越界 → 回退主任务栏
            desired = PrimaryTaskbar();
        }
        if (!desired || desired == currentBound) {
            return currentBound;
        }
        Log("[TASKBAR-SELECTOR] manual: switch %p -> %p (monitor #%d)\n",
            currentBound, desired, manualIndex_);
        outShouldSwitch = true;
        return desired;
    }

    // ── 决策: L1 活动窗口 → L2 鼠标 → L3 主任务栏 ──
    HWND desired = nullptr;
    lastUsedMouse_ = false;

    HWND fg = ::GetForegroundWindow();
    if (fg && fg != lyricsWnd && !IsShellOrDesktop(fg)) {
        HMONITOR mon = ::MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        desired = FindByMonitor(mon);
    }

    if (!desired) {
        POINT pt{};
        ::GetCursorPos(&pt);
        HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        desired = FindByMonitor(mon);
        if (desired) lastUsedMouse_ = true;
    }

    if (!desired) {
        // 目标显示器无任务栏（如关闭"在所有显示器显示任务栏"）→ 强制刷新一次后回退主任务栏
        taskbars_.clear();
        enumerated_ = false;
        EnumerateTaskbars();
        desired = PrimaryTaskbar();
        if (!desired) return currentBound;
    }

    // ── 与当前绑定一致 → 无动作 ──
    if (desired == currentBound) {
        debouncing_ = false;
        debounceTarget_ = nullptr;
        return currentBound;
    }

    // ── 防抖: 候选需持续稳定 kSwitchDebounce 才切换 ──
    const auto now = std::chrono::steady_clock::now();
    if (!debouncing_ || debounceTarget_ != desired) {
        debouncing_ = true;
        debounceTarget_ = desired;
        debounceSince_ = now;
        return currentBound;
    }
    if (now - debounceSince_ < kSwitchDebounce) return currentBound;

    debouncing_ = false;
    debounceTarget_ = nullptr;
    Log("[TASKBAR-SELECTOR] switch %p -> %p (decided by %s)\n",
        currentBound, desired, lastUsedMouse_ ? "mouse" : "foreground window");
    outShouldSwitch = true;
    return desired;
}

} // namespace moekoe
