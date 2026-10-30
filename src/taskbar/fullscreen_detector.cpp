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

// 诊断用：仅在前台/全屏状态翻转时输出，便于排查第三方桌面工具的误判
static void LogForegroundInfo(const char* tag, HWND fgw) {
    if (!fgw) return;
    wchar_t cls[128]{};
    ::GetClassNameW(fgw, cls, 127);

    DWORD pid = 0;
    ::GetWindowThreadProcessId(fgw, &pid);
    wchar_t exePath[MAX_PATH]{};
    DWORD exeLen = MAX_PATH;
    HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        ::QueryFullProcessImageNameW(hProc, 0, exePath, &exeLen);
        ::CloseHandle(hProc);
    }

    RECT r{};
    ::GetWindowRect(fgw, &r);
    const LONG style = ::GetWindowLongW(fgw, GWL_STYLE);
    const LONG exStyle = ::GetWindowLongW(fgw, GWL_EXSTYLE);
    Log("[FullscreenDetect] %s: class=%ls exe=%ls pid=%lu style=0x%lX exstyle=0x%lX rect=(%ld,%ld,%ld,%ld)\n",
        tag, cls, exePath, pid, style, exStyle,
        r.left, r.top, r.right, r.bottom);
}

// 查找与指定显示器关联的任务栏窗口（主任务栏或副任务栏），带缓存避免每帧枚举
static HWND FindTaskbarOnMonitor(HMONITOR mon) {
    static HMONITOR s_cachedMon = nullptr;
    static HWND s_cachedTb = nullptr;
    if (s_cachedMon == mon && s_cachedTb && ::IsWindow(s_cachedTb)) return s_cachedTb;

    const wchar_t* kClasses[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};
    HWND found = nullptr;
    for (const wchar_t* cls : kClasses) {
        HWND h = nullptr;
        while ((h = ::FindWindowExW(nullptr, h, cls, nullptr)) != nullptr) {
            if (::MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == mon) {
                found = h;
                break;
            }
        }
        if (found) break;
    }
    s_cachedMon = mon;
    s_cachedTb = found;
    return found;
}

// 判断前台窗口是否仍被任务栏压着（Z 序上任务栏在它之上）
// 桌面层工具（腾讯桌面整理 TXMiniSkin 等）铺满屏幕但压不住任务栏，
// 真正的全屏应用会盖住任务栏——据此区分，避免误判为全屏而隐藏歌词。
static bool IsTaskbarAboveForeground(HMONITOR mon) {
    HWND tb = FindTaskbarOnMonitor(mon);
    if (!tb) return false;

    RECT tbRect{};
    if (!::GetWindowRect(tb, &tbRect)) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return false;

    // 任务栏滑出屏幕（自动隐藏中）时该判定不可靠，放弃
    if (tbRect.top >= mi.rcMonitor.bottom || tbRect.bottom <= mi.rcMonitor.top ||
        tbRect.left >= mi.rcMonitor.right || tbRect.right <= mi.rcMonitor.left) {
        return false;
    }

    const POINT pt{(tbRect.left + tbRect.right) / 2, (tbRect.top + tbRect.bottom) / 2};
    const HWND hit = ::WindowFromPoint(pt);
    if (!hit) return false;

    HWND root = ::GetAncestor(hit, GA_ROOT);
    if (root == tb) return true;  // 命中的是任务栏本身或其子窗口

    // 歌词窗口（本进程）覆盖在任务栏上，同样说明任务栏未被前台窗口盖住
    DWORD pid = 0;
    ::GetWindowThreadProcessId(root, &pid);
    return pid == ::GetCurrentProcessId();
}

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

    // 桌面层工具（如腾讯桌面整理的 TXMiniSkin）也是「铺满屏幕」的窗口，
    // 但任务栏仍压在它上面。真正的全屏应用会盖住任务栏，据此区分，避免误隐藏歌词。
    const bool taskbarStillOnTop = IsTaskbarAboveForeground(mon);

    if (winW >= monW && winH >= monH) return !taskbarStillOnTop;

    // 宽松检测：无标题栏和可调边框 + 尺寸在显示器边缘 8px 以内
    // 覆盖无边框窗口最大化、F11 浏览器全屏等边界情况
    LONG style = ::GetWindowLongW(fgw, GWL_STYLE);
    constexpr LONG kFullscreenExcludeStyles = WS_CAPTION | WS_THICKFRAME;
    if (!(style & kFullscreenExcludeStyles) && winW >= monW - 8 && winH >= monH - 8) {
        return !taskbarStillOnTop;
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
        LogForegroundInfo("state flip", ::GetForegroundWindow());
        Log("[FullscreenDetect] state flip: %d -> %d (debounce reset)\n",
            (int)lastFullscreenState_, (int)isFullscreen);
        debounceCnt_ = 0;
        lastFullscreenState_ = isFullscreen;
    }

    constexpr int kDebounceThreshold = 8;
    if (debounceCnt_ == kDebounceThreshold) {
        Log("[FullscreenDetect] debounce reached -> shouldHide=%d\n", (int)isFullscreen);
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
