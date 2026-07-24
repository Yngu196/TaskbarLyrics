// SPDX-License-Identifier: GPL-3.0
// fullscreen_detector.cpp - 全屏检测与防抖实现
//
// 检测逻辑：
//   1. 获取前台窗口，排除桌面/任务栏类窗口（Progman/WorkerW/Shell_TrayWnd）
//   2. 窗口尺寸 ≥ 所在显示器尺寸 → 全屏（标准全屏）
//   3. 无 WS_CAPTION | WS_THICKFRAME 且窗口距显示器边缘 ≤ 8px → 无边框最大化窗口
//
// 防抖机制：
//   - 8 帧连续一致才触发状态变更，避免窗口切换/动画期间的抖动
//   - Shell 菜单抑制（shellMenuSuppress_）：MENUPOPUPSTART 恢复歌词后阻止防抖立即再次隐藏
//   - ForceReset：Shell 交互/全屏应用退出时从外部强制重置防抖计数器
#include "taskbar/fullscreen_detector.h"

#include <windows.h>

#include "util/logger.h"

namespace moekoe {

bool FullscreenDetector::IsForegroundFullscreen(bool debugLog) {
    HWND fgw = ::GetForegroundWindow();
    if (!fgw) return false;

    // 排除桌面 Progman / WorkerW 和本进程窗口，避免误判
    wchar_t cls[64] = {};
    ::GetClassNameW(fgw, cls, 63);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0) {
        return false;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(fgw, &pid);
    if (pid == ::GetCurrentProcessId()) return false;

    RECT wr{};
    if (!::GetWindowRect(fgw, &wr)) return false;

    HMONITOR mon = ::MonitorFromWindow(fgw, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return false;

    const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int winW = wr.right - wr.left;
    const int winH = wr.bottom - wr.top;

    // 已注释：逐帧全屏检测日志太噪，影响 bug 定位
    // static int s_debugCounter = 0;
    // if (debugLog && ++s_debugCounter >= 60) {
    //     s_debugCounter = 0;
    //     LONG style = ::GetWindowLongW(fgw, GWL_STYLE);
    //     LONG exStyle = ::GetWindowLongW(fgw, GWL_EXSTYLE);
    //     char clsUtf8[128] = {};
    //     ::WideCharToMultiByte(CP_UTF8, 0, cls, -1, clsUtf8, (int)sizeof(clsUtf8), nullptr, nullptr);
    //     Log("[FullscreenDetect] cls=%s pid=%lu win=(%d,%d,%d,%d) mon=(%d,%d,%d,%d) style=0x%08lX => %s\n",
    //         clsUtf8, pid, wr.left, wr.top, wr.right, wr.bottom,
    //         mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
    //         style, (winW >= monW && winH >= monH) ? "FULLSCREEN" : "normal");
    // }

    if (winW >= monW && winH >= monH) return true;

    // 宽松检测：无标题栏和可调边框 + 尺寸在显示器边缘 8px 以内
    // 覆盖无边框窗口最大化、F11 浏览器全屏等边界情况
    LONG style = ::GetWindowLongW(fgw, GWL_STYLE);
    constexpr LONG kFullscreenExcludeStyles = WS_CAPTION | WS_THICKFRAME;
    if (!(style & kFullscreenExcludeStyles) && winW >= monW - 8 && winH >= monH - 8) {
        return true;
    }

    return false;
}

bool FullscreenDetector::Update(bool enableFullscreenHide, bool debugLog, bool& outShouldHide) {
    if (!enableFullscreenHide) {
        return false;
    }

    const bool isFullscreen = IsForegroundFullscreen(debugLog);

    // Shell 菜单抑制：MENUPOPUPSTART 恢复歌词后阻止防抖立即再次隐藏。
    // 当前台窗口真正离开全屏应用时自动解除抑制。
    if (shellMenuSuppress_) {
        if (!isFullscreen) {
            if (debugLog) Log("[FullscreenDetect] shell menu suppress cleared (fg left fullscreen)\n");
            shellMenuSuppress_ = false;
            // 同步防抖状态到当前实际状态，避免 flip
            debounceCnt_ = 0;
            lastFullscreenState_ = false;
        }
        return false;
    }

    if (isFullscreen == lastFullscreenState_) {
        if (debounceCnt_ < 99) debounceCnt_++;
    } else {
        if (debugLog) Log("[FullscreenDetect] state flip: %d -> %d (debounce reset)\n",
            (int)lastFullscreenState_, (int)isFullscreen);
        debounceCnt_ = 0;
        lastFullscreenState_ = isFullscreen;
    }

    constexpr int kDebounceThreshold = 8;
    if (debounceCnt_ == kDebounceThreshold) {
        if (debugLog) Log("[FullscreenDetect] debounce reached -> shouldHide=%d\n", (int)isFullscreen);
        outShouldHide = isFullscreen;
        return true;  // signal: debounce triggered, caller should act
    }

    return false;
}

bool FullscreenDetector::ConsumeForceDebounceReset() {
    bool v = forceDebounceReset_;
    if (v) {
        forceDebounceReset_ = false;
        debounceCnt_ = 0;
        lastFullscreenState_ = false;
    }
    return v;
}

} // namespace moekoe
