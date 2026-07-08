// SPDX-License-Identifier: GPL-3.0
// shell_companion.cpp - Shell 层抽象实现
// v0.5.x: 从 taskbar_window.cpp 中抽取 WinEvent 钩子、全屏检测、定位计算等 Shell 逻辑
#include "shell_companion.h"
#include "constants.h"
#include "logger.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cassert>
#include <vector>

namespace moekoe {

// ═════════════════════════════════════════
// 静态成员定义
// ═════════════════════════════════════════
ShellCompanion* ShellCompanion::s_instance_                   = nullptr;
HWND            ShellCompanion::s_lyricsWnd_                  = nullptr;
HWINEVENTHOOK   ShellCompanion::s_taskbarHook_                = nullptr;
HWINEVENTHOOK   ShellCompanion::s_shellMenuHook_              = nullptr;
HWINEVENTHOOK   ShellCompanion::s_foregroundHook_             = nullptr;
bool            ShellCompanion::s_shellInteractionLocked_      = false;
std::chrono::steady_clock::time_point ShellCompanion::s_shellInteractionLockedTime_{};
RECT            ShellCompanion::s_frozenTaskbarRect_{};
RECT            ShellCompanion::s_lastGoodTaskbarRect_{};
bool            ShellCompanion::s_lockedByStartMenuFg_        = false;

// ═════════════════════════════════════════
// WinEvent 回调
// ═════════════════════════════════════════

void CALLBACK ShellCompanion::TaskbarWinEventProc(
    HWINEVENTHOOK, DWORD, HWND hWnd,
    LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || !hWnd) return;
    wchar_t cls[32] = {};
    ::GetClassNameW(hWnd, cls, 31);
    if (wcscmp(cls, L"Shell_TrayWnd") != 0) return;

    // 投递延迟消息：10ms 等 DWM 帧完成，避免在动画中定位
    if (s_lyricsWnd_) {
        ::PostMessageW(s_lyricsWnd_,
                       WM_USER + 0x101, 0, 0);  // WM_DELAYED_REPOSITION
    }
}

void CALLBACK ShellCompanion::ShellMenuWinEventProc(
    HWINEVENTHOOK, DWORD event, HWND,
    LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_MENUPOPUPSTART) {
        // 全屏隐藏时，按 Win 键呼出开始菜单应该立即恢复歌词显示。
        // 不等待防抖（全屏应用可能仍占前台 → IsForegroundFullscreen 持续为 true → 迟迟不恢复）。
        // 必须在冻结锁之前执行：冻结锁会阻止 PositionLyricsInTaskbar 定位，若先锁后恢复则窗口停在
        // (-32000,-32000) 屏幕外。先恢复后锁，SetFullscreenHidden(false) 内的定位可正常执行。
        if (s_instance_ && s_instance_->IsFullscreenHidden()) {
            ::OutputDebugStringW(
                L"[TaskbarLyrics] MENUPOPUPSTART: fullscreen hidden, restoring immediately\n");
            s_instance_->SetFullscreenHidden(false, s_lyricsWnd_);
            s_instance_->Fullscreen().SetShellMenuSuppress(true);
        }

        // 锁定定位 + 冻结任务栏几何快照（必须在恢复歌词之后设置，见上方注释）
        // Explorer 在 Start Menu 激活期间会临时改动任务栏内部布局（托盘重排、
        // client width 微变），导致 GetWindowRect 返回值被污染（right 膨胀 +3~7px）。
        // 使用 s_lastGoodTaskbarRect_（PositionLyricsInTaskbar 非冻结模式时缓存的最后稳定 rect），
        // 彻底隔离 Explorer 的瞬时脏写。
        s_shellInteractionLocked_ = true;
        s_shellInteractionLockedTime_ = std::chrono::steady_clock::now();
        if (s_lastGoodTaskbarRect_.right != 0) {
            s_frozenTaskbarRect_ = s_lastGoodTaskbarRect_;
            wchar_t dbg[160];
            swprintf_s(dbg, L"[TaskbarLyrics] MENUPOPUPSTART: lock ON, frozen=(%d,%d,%d,%d)"
                       L" from s_lastGoodTaskbarRect_\n",
                       s_frozenTaskbarRect_.left,
                       s_frozenTaskbarRect_.top,
                       s_frozenTaskbarRect_.right,
                       s_frozenTaskbarRect_.bottom);
            ::OutputDebugStringW(dbg);
        } else {
            // 降级：冷启动时 s_lastGoodTaskbarRect_ 尚未初始化
            HWND tb = ::FindWindowW(L"Shell_TrayWnd", nullptr);
            if (tb) {
                ::GetWindowRect(tb, &s_frozenTaskbarRect_);
                wchar_t dbg[160];
                swprintf_s(dbg, L"[TaskbarLyrics] MENUPOPUPSTART: lock ON, frozen=(%d,%d,%d,%d)"
                           L" from GetWindowRect (cold boot fallback)\n",
                           s_frozenTaskbarRect_.left,
                           s_frozenTaskbarRect_.top,
                           s_frozenTaskbarRect_.right,
                           s_frozenTaskbarRect_.bottom);
                ::OutputDebugStringW(dbg);
            }
        }
    } else if (event == EVENT_SYSTEM_MENUPOPUPEND) {
        ::OutputDebugStringW(
            L"[TaskbarLyrics] MENUPOPUPEND: scheduling unlock (300ms)\n");
        if (s_lyricsWnd_) {
            ::SetTimer(s_lyricsWnd_, 3, 300, nullptr);
        }
    }
}

void CALLBACK ShellCompanion::ForegroundWinEventProc(
    HWINEVENTHOOK, DWORD, HWND hWnd,
    LONG, LONG, DWORD, DWORD) {
    if (!hWnd || !s_lyricsWnd_) return;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hWnd, &pid);
    if (!pid) return;

    HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return;

    wchar_t path[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    ::QueryFullProcessImageNameW(hProc, 0, path, &sz);
    ::CloseHandle(hProc);

    const wchar_t* exeName = ::wcsrchr(path, L'\\');
    exeName = exeName ? exeName + 1 : path;

    // Win11 Start Menu / Search 叠加层判断
    const bool isStartMenu =
        (::_wcsicmp(exeName, L"StartMenuExperienceHost.exe") == 0) ||
        (::_wcsicmp(exeName, L"SearchHost.exe") == 0);

    if (isStartMenu) {
        if (!s_shellInteractionLocked_) {
            s_shellInteractionLocked_ = true;
            s_shellInteractionLockedTime_ = std::chrono::steady_clock::now();
            s_lockedByStartMenuFg_    = true;
            if (s_lastGoodTaskbarRect_.right != 0) {
                s_frozenTaskbarRect_ = s_lastGoodTaskbarRect_;
            }
            ::OutputDebugStringW(
                L"[TaskbarLyrics] ForegroundHook: StartMenu ON, lock set\n");
        }
    } else if (s_lockedByStartMenuFg_) {
        // 前台离开 Start Menu → 300ms 后解锁
        s_lockedByStartMenuFg_ = false;
        ::SetTimer(s_lyricsWnd_, 4, 300, nullptr);
        ::OutputDebugStringW(
            L"[TaskbarLyrics] ForegroundHook: StartMenu OFF, unlock in 300ms\n");
    }
}

// ═════════════════════════════════════════
// WinEvent 钩子安装/移除
// ═════════════════════════════════════════

void ShellCompanion::InstallHooks(HWND lyricsWnd) {
    s_lyricsWnd_ = lyricsWnd;
    s_taskbarHook_ = ::SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
        nullptr, TaskbarWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    s_shellMenuHook_ = ::SetWinEventHook(
        EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPEND,
        nullptr, ShellMenuWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Win11 Start Menu 检测（Win 键 / Win+S 叠加层）
    s_foregroundHook_ = ::SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, ForegroundWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void ShellCompanion::RemoveHooks() {
    if (s_taskbarHook_) {
        ::UnhookWinEvent(s_taskbarHook_);
        s_taskbarHook_ = nullptr;
    }
    if (s_shellMenuHook_) {
        ::UnhookWinEvent(s_shellMenuHook_);
        s_shellMenuHook_ = nullptr;
    }
    if (s_foregroundHook_) {
        ::UnhookWinEvent(s_foregroundHook_);
        s_foregroundHook_ = nullptr;
    }
    s_lockedByStartMenuFg_ = false;
    s_lyricsWnd_ = nullptr;
}

// ═════════════════════════════════════════
// 构造函数 / 析构函数
// ═════════════════════════════════════════

ShellCompanion::ShellCompanion() = default;

ShellCompanion::~ShellCompanion() {
    Shutdown();
}

// ═════════════════════════════════════════
// 生命周期
// ═════════════════════════════════════════

bool ShellCompanion::Initialize(HWND hTaskbar, HWND lyricsWnd) {
    if (!hTaskbar) return false;

    hTaskbar_ = hTaskbar;

    // 初始化 UIA
    geometry_.InitUIA();

    // 检测任务栏几何信息
    info_ = geometry_.Detect(hTaskbar_);
    taskbarAutoHide_ = info_.autoHide;

    // 安装 WinEvent 钩子（监听任务栏位置变化、Start Menu、前台切换）
    InstallHooks(lyricsWnd);

    // 注入歌词窗口句柄到 embedder_，供全屏显隐等操作使用 SetWindowPos
    embedder_.SetHandle(lyricsWnd);

    s_instance_ = this;
    assert(s_instance_ && "ShellCompanion::Initialize called twice");
    return true;
}

void ShellCompanion::Shutdown() {
    s_instance_ = nullptr;
    RemoveHooks();
    geometry_.CleanupUIA();
}

// ═════════════════════════════════════════
// 全屏显隐控制
// ═════════════════════════════════════════

void ShellCompanion::SetFullscreenHidden(bool hidden, HWND lyricsWnd) {
    if (embedder_.IsFullscreenHidden() == hidden) return;
    HWND hwnd = embedder_.GetHandle();
    if (!hwnd) return;

    if (hidden) {
        Log("[ShellCompanion] SetFullscreenHidden(true): hiding window (hwnd=%p)\n", hwnd);

        // 全屏隐藏时主动释放 Start Menu 冻结锁：
        // 全屏游戏激活时常触发 MENUPOPUPSTART 全局事件却永不发送 MENUPOPUPEND，
        // 导致 s_shellInteractionLocked_ 永久锁死，CheckResize/PositionLyricsInTaskbar 冻结。
        if (s_shellInteractionLocked_) {
            s_shellInteractionLocked_ = false;
            s_lockedByStartMenuFg_ = false;
            Log("[ShellCompanion] SetFullscreenHidden: released freeze lock\n");
        }

        // 委托 embedder_ 执行实际的 SetWindowPos(SWP_HIDEWINDOW) + 移出屏幕
        embedder_.SetFullscreenHidden(true);
        Log("[ShellCompanion] SetFullscreenHidden: SWP_HIDEWINDOW + offscreen done\n");
    } else {
        Log("[ShellCompanion] SetFullscreenHidden(false): restoring window\n");

        // 通知 FullscreenDetector 重置全屏检测防抖计数器
        fullscreenDetector_.ForceReset();

        // 委托 embedder_ 执行恢复显示（HWND_TOPMOST + SWP_SHOWWINDOW）
        // 不在此处重定位：定位由 main.cpp 帧循环通过 TaskbarWindow::Reposition() 完成，
        // 确保使用正确的 dragOffset（用户手动拖动后保留的偏移量）
        embedder_.SetFullscreenHidden(false);
        Log("[ShellCompanion] SetFullscreenHidden: SWP_SHOWWINDOW done, repaint queued\n");
    }
}

// ═════════════════════════════════════════
// 帧级更新
// ═════════════════════════════════════════

void ShellCompanion::CheckResize(HWND lyricsWnd) {
    if (!hTaskbar_) return;

    // 全屏隐藏期间跳过重检测：避免 WM_DELAYED_REPOSITION → PositionLyricsInTaskbar
    // 中 SetWindowPos(SWP_SHOWWINDOW) 重新显示已隐藏的窗口
    if (embedder_.IsFullscreenHidden()) return;

    // Start Menu 激活期间跳过帧级重检测
    // 超时保护：若 s_shellInteractionLocked_ 超过 5 秒未解除则自动复位
    if (s_shellInteractionLocked_) {
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kMaxLockDuration = std::chrono::seconds(5);
        if (now - s_shellInteractionLockedTime_ > kMaxLockDuration) {
            s_shellInteractionLocked_ = false;
            ::OutputDebugStringW(
                L"[TaskbarLyrics] CheckResize: s_shellInteractionLocked_ timeout (5s), force reset\n");
        } else {
            return;
        }
    }

    RECT tb{};
    ::GetWindowRect(hTaskbar_, &tb);
    if (!::EqualRect(&tb, &lastTaskbarRect_)) {
        wchar_t dbg[160];
        swprintf_s(dbg, L"[TaskbarLyrics] CheckResize: rect changed "
                   L"old=(%d,%d,%d,%d) new=(%d,%d,%d,%d) → posting WM_DELAYED_REPOSITION\n",
                   lastTaskbarRect_.left, lastTaskbarRect_.top,
                   lastTaskbarRect_.right, lastTaskbarRect_.bottom,
                   tb.left, tb.top, tb.right, tb.bottom);
        ::OutputDebugStringW(dbg);

        // 使用 PostMessage 延迟定位：确保任何已排队的 MENUPOPUPSTART 事件优先处理
        RequestReposition(lyricsWnd);
    } else if (taskbarAutoHide_) {
        // 自动隐藏模式：即使矩形未变，任务栏可能已滑出/滑入
        const int tbH = tb.bottom - tb.top;
        constexpr int kAutoHideThreshold = 10;
        const bool tbIsVisible = (tbH >= kAutoHideThreshold);
        if (tbIsVisible != taskbarVisible_) {
            // 通知外部进行重定位（通过回调或直接触发）
            if (onReposition_) onReposition_();
        }
    }
}

// ═════════════════════════════════════════
// 定位计算 + SetWindowPos
// ═════════════════════════════════════════

void ShellCompanion::DetectTaskbarInfo() {
    info_ = geometry_.Detect(hTaskbar_);
    taskbarAutoHide_ = info_.autoHide;
}

void ShellCompanion::PositionLyricsInTaskbar(
    HWND lyricsWnd,
    const std::string& displayMode,
    int dragOffsetX, int dragOffsetY,
    RECT& inOutLastPosRect) {
    if (!lyricsWnd || !hTaskbar_) return;

    // 全屏隐藏期间跳过定位
    if (embedder_.IsFullscreenHidden()) return;

    // Start Menu 冻结期间跳过重定位
    const bool useFrozen = s_shellInteractionLocked_ && s_frozenTaskbarRect_.right != 0;
    if (useFrozen) {
        ::OutputDebugStringW(L"[TaskbarLyrics] PositionLyricsInTaskbar: frozen → skip\n");
        return;
    }

    // 记录旧方位，用于检测方向变化
    // 方位变化检测由调用方（TaskbarWindow）在 InternalPosition 中处理
    DetectTaskbarInfo();

    // ── 自动隐藏状态跟踪 ──
    if (taskbarAutoHide_) {
        RECT tbCurrent{};
        ::GetWindowRect(hTaskbar_, &tbCurrent);
        const int tbSize = (info_.position == TaskbarPosition::LEFT ||
                            info_.position == TaskbarPosition::RIGHT)
                               ? (tbCurrent.right - tbCurrent.left)
                               : (tbCurrent.bottom - tbCurrent.top);
        constexpr int kAutoHideThreshold = 10;
        taskbarVisible_ = (tbSize >= kAutoHideThreshold);
    } else {
        taskbarVisible_ = true;
    }

    RECT tbRect{};
    ::GetWindowRect(hTaskbar_, &tbRect);

    lastTaskbarRect_ = tbRect;
    // 同步稳定 rect 到静态缓存，供 ShellMenuWinEventProc 冻结快照使用
    s_lastGoodTaskbarRect_ = tbRect;
    const int tbWidth  = tbRect.right  - tbRect.left;
    const int tbHeight = tbRect.bottom - tbRect.top;

    // ── P2 节流：UIA 枚举委托 geometry_，200ms 节流缓存 ──
    RECT taskListRect = {};
    bool foundTaskList = false;
    RECT trayRect = {};
    bool foundTray = false;
    RECT rebarRect = {};
    bool foundRebar = false;

    const bool uiaExpired = geometry_.IsUiaCacheExpired(200);
    if (uiaExpired) {
        geometry_.GetChildRectsByUIA(taskListRect, foundTaskList,
                                     trayRect, foundTray,
                                     rebarRect, foundRebar, tbWidth);
        geometry_.CacheUiaResults(taskListRect, foundTaskList,
                                  trayRect, foundTray,
                                  rebarRect, foundRebar);
    } else {
        geometry_.LoadUiaCache(taskListRect, foundTaskList,
                               trayRect, foundTray,
                               rebarRect, foundRebar);
    }

    // ── 帧锁定（扩展）：双采样检测任务栏本体 + 任务列表子窗口的稳定性 ──
    if (uiaExpired) {
        RECT tbCheck{};
        ::Sleep(2);
        ::GetWindowRect(hTaskbar_, &tbCheck);

        RECT taskListCheck{};
        bool taskListCheckValid = false;
        if (foundTaskList) {
            // 二次 UIA 查询
            bool fTL2, fTR2, fRB2;
            RECT tl2, tr2, rb2;
            geometry_.GetChildRectsByUIA(tl2, fTL2, tr2, fTR2, rb2, fRB2, tbWidth);
            if (fTL2) { taskListCheck = tl2; taskListCheckValid = true; }
            else if (foundTaskList) {
                // UIA 失败降级：用 EnumChildWindows 做二次采样
                HWND hChild2 = ::GetWindow(hTaskbar_, GW_CHILD);
                while (hChild2) {
                    if (::IsWindowVisible(hChild2)) {
                        wchar_t cname[256] = {};
                        ::GetClassNameW(hChild2, cname, 255);
                        bool match = (wcscmp(cname, L"MSTaskListWClass") == 0);
                        if (!match && wcscmp(cname,
                                L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
                            RECT cr{};
                            ::GetWindowRect(hChild2, &cr);
                            if ((cr.right - cr.left) < tbWidth - 10) match = true;
                        }
                        if (match) {
                            ::GetWindowRect(hChild2, &taskListCheck);
                            taskListCheckValid = true;
                            break;
                        }
                    }
                    hChild2 = ::GetWindow(hChild2, GW_HWNDNEXT);
                }
            }
        }

        const int deltaW = abs((tbRect.right - tbRect.left) -
                               (tbCheck.right - tbCheck.left));
        const int deltaH = abs((tbRect.bottom - tbRect.top) -
                               (tbCheck.bottom - tbCheck.top));
        const int deltaTaskList = taskListCheckValid
            ? abs(taskListRect.right - taskListCheck.right) : 0;

        if (deltaW > 5 || deltaH > 5 || deltaTaskList > 3) {
            if (stableTaskbarRect_.right != 0) {
                wchar_t dbg[160];
                swprintf_s(dbg,
                    L"[TaskbarLyrics] Instability: dW=%d dH=%d dTaskList=%d"
                    L" → fallback stable tbRect=(%d,%d,%d,%d)\n",
                    deltaW, deltaH, deltaTaskList,
                    stableTaskbarRect_.left, stableTaskbarRect_.top,
                    stableTaskbarRect_.right, stableTaskbarRect_.bottom);
                ::OutputDebugStringW(dbg);
                tbRect = stableTaskbarRect_;
            }
            if (stableTaskListValid_ && foundTaskList) {
                taskListRect = stableTaskListRect_;
            }
        } else {
            stableTaskbarRect_ = tbRect;
            if (taskListCheckValid) {
                stableTaskListRect_ = taskListRect;
                stableTaskListValid_ = true;
            }
        }
    }

    // 歌词区尺寸（根据显示模式选择不同的高度）
    const bool isCardMode = (displayMode == "card");
    const int lyricH = ::MulDiv(
        isCardMode ? constants::CARD_HEIGHT_BASE_DP : constants::LYRIC_HEIGHT_BASE_DP,
        info_.dpi, 96);
    int w = 0, h = lyricH, x = 0, y = 0;

    switch (info_.position) {
    case TaskbarPosition::BOTTOM: {
        int rightEdge = tbRect.right;
        if (foundTray) {
            rightEdge = trayRect.left;
        }

        // ── 缓存托盘偏移，动画期间复用稳定值 ──
        const int offsetFromRight = tbRect.right - rightEdge;
        const bool isAnimating = taskbarAutoHide_ && (
            abs(tbRect.bottom - stableTaskbarRect_.bottom) > 5 ||
            abs(tbRect.top - stableTaskbarRect_.top) > 5);
        if (!isAnimating && foundTray) {
            cachedRightEdgeOffset_ = offsetFromRight;
        } else if (isAnimating) {
            rightEdge = tbRect.right - cachedRightEdgeOffset_;
        }

        const int maxLyricWidth = ::MulDiv(
            isCardMode ? constants::CARD_MIN_WIDTH_BASE_DP * 2 : constants::MAX_LYRIC_WIDTH_BASE_DP,
            info_.dpi, 96);
        int availableWidth = rightEdge - tbRect.left;
        if (foundTaskList) {
            availableWidth = rightEdge - taskListRect.right;
        }

        w = std::min(maxLyricWidth, std::max(availableWidth, constants::MIN_LYRIC_AVAILABLE_WIDTH));
        x = rightEdge - w + dragOffsetX;
        y = tbRect.top + dragOffsetY;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::TOP: {
        int rightEdge = tbRect.right;
        if (foundTray) rightEdge = trayRect.left;

        const int maxLyricWidth = ::MulDiv(
            isCardMode ? constants::CARD_MIN_WIDTH_BASE_DP * 2 : constants::MAX_LYRIC_WIDTH_BASE_DP,
            info_.dpi, 96);
        int availableWidth = rightEdge - tbRect.left;
        if (foundTaskList) {
            availableWidth = rightEdge - taskListRect.right;
        }

        w = std::min(maxLyricWidth, std::max(availableWidth, constants::MIN_LYRIC_AVAILABLE_WIDTH));
        x = rightEdge - w + dragOffsetX;
        y = tbRect.top + dragOffsetY;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::LEFT: {
        w = ::MulDiv(constants::VERTICAL_TASKBAR_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
        x = tbRect.right - w;

        int availableBottom = tbRect.bottom;
        if (foundTray) {
            availableBottom = trayRect.top;
        }

        const int maxLyricHeight = ::MulDiv(
            isCardMode ? constants::CARD_HEIGHT_BASE_DP * 4 : constants::LYRIC_HEIGHT_BASE_DP * 6,
            info_.dpi, 96);
        int availableH = availableBottom - tbRect.top;
        h = std::min(maxLyricHeight, std::max(availableH, lyricH));

        y = tbRect.top + (availableH - h) / 2 + dragOffsetY;
        break;
    }
    case TaskbarPosition::RIGHT: {
        w = ::MulDiv(constants::VERTICAL_TASKBAR_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
        x = tbRect.left;

        int availableBottom = tbRect.bottom;
        if (foundTray) {
            availableBottom = trayRect.top;
        }

        const int maxLyricHeight = ::MulDiv(
            isCardMode ? constants::CARD_HEIGHT_BASE_DP * 4 : constants::LYRIC_HEIGHT_BASE_DP * 6,
            info_.dpi, 96);
        int availableH = availableBottom - tbRect.top;
        h = std::min(maxLyricHeight, std::max(availableH, lyricH));

        y = tbRect.top + (availableH - h) / 2 + dragOffsetY;
        break;
    }
    default:
        x = tbRect.left; y = tbRect.top; w = tbWidth; h = tbHeight;
        break;
    }

    w = std::max(w, constants::MIN_WINDOW_WIDTH);
    h = std::max(h, lyricH);

    // 防御性检查：确保窗口坐标在任务栏范围内
    if (info_.position == TaskbarPosition::LEFT ||
        info_.position == TaskbarPosition::RIGHT) {
        if (y < tbRect.top) y = tbRect.top;
        if (y + h > tbRect.bottom) y = tbRect.bottom - h;
        if (y < tbRect.top) y = tbRect.top;
    } else {
        if (x < tbRect.left) x = tbRect.left;
        if (x + w > tbRect.right) x = tbRect.right - w;
        if (x < tbRect.left) x = tbRect.left;
    }

    // 调试日志
    {
        wchar_t dbg[256];
        swprintf_s(dbg, L"[TaskbarLyrics] Pos: pos=%d x=%d y=%d w=%d h=%d "
                   L"tbRect=(%d,%d,%d,%d) dragOffX=%d\n",
                   static_cast<int>(info_.position), x, y, w, h,
                   tbRect.left, tbRect.top, tbRect.right, tbRect.bottom,
                   dragOffsetX);
        ::OutputDebugStringW(dbg);
    }

    // 短路：坐标未变且窗口可见时跳过 SetWindowPos
    if (inOutLastPosRect.left == x && inOutLastPosRect.top == y &&
        inOutLastPosRect.right == w && inOutLastPosRect.bottom == h &&
        ::IsWindowVisible(lyricsWnd)) {
        return;
    }
    inOutLastPosRect = {x, y, w, h};

    // owned window 天然在 owner (任务栏) 之上，用 HWND_TOP 保持此关系
    ::SetWindowPos(
        lyricsWnd, HWND_TOP,
        x, y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

// ═════════════════════════════════════════
// 拖动后吸附到空闲空间
// ═════════════════════════════════════════

void ShellCompanion::SnapToEmptySpace(HWND lyricsWnd) {
    if (!lyricsWnd || !hTaskbar_) return;

    // 全屏隐藏期间跳过吸附
    if (embedder_.IsFullscreenHidden()) return;

    RECT winRect{};
    ::GetWindowRect(lyricsWnd, &winRect);
    const int winW = winRect.right - winRect.left;
    const int winH = winRect.bottom - winRect.top;
    if (winW <= 0 || winH <= 0) return;

    RECT tbRect{};
    ::GetWindowRect(hTaskbar_, &tbRect);

    const int myStart = (info_.position == TaskbarPosition::LEFT || info_.position == TaskbarPosition::RIGHT)
                             ? winRect.top : winRect.left;
    const int myEnd = myStart + ((info_.position == TaskbarPosition::LEFT || info_.position == TaskbarPosition::RIGHT)
                                      ? winH : winW);

    constexpr int kSampleStep = 8;

    const int centerX = (winRect.left + winRect.right) / 2;
    const int centerY = (winRect.top + winRect.bottom) / 2;

    // 临时将窗口移到屏幕外（避免遮挡采样）
    ::SetWindowPos(lyricsWnd, nullptr,
                   -winW * 2, -winH * 2, winW, winH,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    struct SamplePoint { int pos; bool occupied; };
    std::vector<SamplePoint> samples;

    const int sampleCount = (myEnd - myStart) / kSampleStep + 1;
    for (int i = 0; i < sampleCount; ++i) {
        int pos = std::min(myStart + i * kSampleStep, myEnd - 1);
        POINT pt{};

        switch (info_.position) {
        case TaskbarPosition::BOTTOM:
        case TaskbarPosition::TOP:
            pt.x = pos;
            pt.y = centerY;
            break;
        case TaskbarPosition::LEFT:
        case TaskbarPosition::RIGHT:
            pt.x = centerX;
            pt.y = pos;
            break;
        default:
            continue;
        }

        HWND hHit = ::WindowFromPoint(pt);
        bool isObstacle = false;

        if (hHit && hHit != lyricsWnd && hHit != hTaskbar_) {
            isObstacle = true;
        }

        samples.push_back({pos, isObstacle});
    }

    // 立即恢复窗口原位
    ::SetWindowPos(lyricsWnd, nullptr,
                   winRect.left, winRect.top, winW, winH,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    if (samples.empty()) return;

    // 判断整体是否有重叠
    bool hasObstacle = false;
    for (const auto& s : samples) {
        if (s.occupied) { hasObstacle = true; break; }
    }
    if (!hasObstacle) return;

    // 合并占用区间
    struct OccupiedRange { int start; int end; };
    std::vector<OccupiedRange> occupied;

    int rangeStart = -1;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].occupied && rangeStart < 0) {
            rangeStart = samples[i].pos;
        }
        if (!samples[i].occupied && rangeStart >= 0) {
            int rangeEnd = (i > 0) ? samples[i - 1].pos + kSampleStep : rangeStart + kSampleStep;
            occupied.push_back({rangeStart, rangeEnd});
            rangeStart = -1;
        }
    }
    if (rangeStart >= 0) {
        occupied.push_back({rangeStart, samples.back().pos + kSampleStep});
    }

    if (occupied.empty()) return;

    std::sort(occupied.begin(), occupied.end(),
              [](const OccupiedRange& a, const OccupiedRange& b) {
                  return a.start < b.start;
              });

    std::vector<OccupiedRange> merged;
    for (const auto& o : occupied) {
        if (!merged.empty() && o.start <= merged.back().end + kSampleStep) {
            merged.back().end = std::max(merged.back().end, o.end);
        } else {
            merged.push_back(o);
        }
    }

    const int tbStart = (info_.position == TaskbarPosition::LEFT || info_.position == TaskbarPosition::RIGHT)
                             ? tbRect.top : tbRect.left;
    const int tbEnd = (info_.position == TaskbarPosition::LEFT || info_.position == TaskbarPosition::RIGHT)
                           ? tbRect.bottom : tbRect.right;

    const int neededSize = (info_.position == TaskbarPosition::LEFT || info_.position == TaskbarPosition::RIGHT)
                                ? winH : winW;

    int bestPos = -1;
    int bestDist = INT_MAX;

    auto tryGap = [&](int gapStart, int gapEnd) {
        int gapSize = gapEnd - gapStart;
        if (gapSize >= neededSize) {
            int candidate = std::clamp(myStart, gapStart, gapEnd - neededSize);
            int dist = std::abs(candidate - myStart);
            if (dist < bestDist) {
                bestDist = dist;
                bestPos = candidate;
            }
        }
    };

    if (!merged.empty()) {
        tryGap(tbStart, merged.front().start);
    }

    for (size_t i = 0; i < merged.size(); ++i) {
        int gapStart = merged[i].end;
        int gapEnd = (i + 1 < merged.size()) ? merged[i + 1].start : tbEnd;
        tryGap(gapStart, gapEnd);
    }

    if (bestPos < 0) return;

    // 应用新位置
    int newX = winRect.left;
    int newY = winRect.top;
    switch (info_.position) {
    case TaskbarPosition::BOTTOM:
    case TaskbarPosition::TOP:
        newX = bestPos;
        break;
    case TaskbarPosition::LEFT:
    case TaskbarPosition::RIGHT:
        newY = bestPos;
        break;
    default:
        return;
    }

    // 注意：拖动偏移量更新由调用方（TaskbarWindow）负责，此处只执行 SetWindowPos
    ::SetWindowPos(lyricsWnd, nullptr, newX, newY, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ═════════════════════════════════════════
// Shell 交互锁定管理
// ═════════════════════════════════════════

void ShellCompanion::UnlockShellInteraction(int timerId) {
    if (timerId == 3) {
        ::OutputDebugStringW(
            L"[TaskbarLyrics] Timer3: unlocking s_shellInteractionLocked_\n");
    } else if (timerId == 4) {
        ::OutputDebugStringW(
            L"[TaskbarLyrics] Timer4: unlock (ForegroundHook)\n");
    }
    s_shellInteractionLocked_ = false;
}

// ═════════════════════════════════════════
// 回调与消息投递
// ═════════════════════════════════════════

void ShellCompanion::OnRepositionNeeded(RepositionCallback cb) {
    onReposition_ = std::move(cb);
}

void ShellCompanion::RequestReposition(HWND lyricsWnd) {
    if (lyricsWnd) {
        ::PostMessageW(lyricsWnd, WM_USER + 0x101, 0, 0);  // WM_DELAYED_REPOSITION
    }
}

// ═════════════════════════════════════════
// 任务栏矩形查询
// ═════════════════════════════════════════

RECT ShellCompanion::GetTaskbarRect() const {
    RECT rect{0, 0, 0, 0};
    if (hTaskbar_) {
        ::GetWindowRect(hTaskbar_, &rect);
    }
    return rect;
}

// ═════════════════════════════════════════
// 静态：查找任务栏句柄
// ═════════════════════════════════════════

HWND ShellCompanion::FindTaskbarHandle() {
    // 主任务栏
    HWND h = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (h) return h;

    // 兼容性: 触发 AppBar 消息
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    if (::SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
        h = ::FindWindowW(L"Shell_TrayWnd", nullptr);
        if (h) return h;
    }
    return nullptr;
}

} // namespace moekoe
