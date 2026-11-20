// SPDX-License-Identifier: GPL-3.0
// shell_companion.cpp - Shell 层抽象实现
// v0.5.x: 从 taskbar_window.cpp 中抽取 WinEvent 钩子、全屏检测、定位计算等 Shell 逻辑
#include "taskbar/shell_companion.h"
#include "core/constants.h"
#include "util/logger.h"

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

// 自动隐藏判定：任务栏在屏幕内的可见部分小于该值视为已隐藏（px）
constexpr int kAutoHideVisibleMinPx = 10;

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

    // 投递延迟消息：正常 16ms，兼容模式 SlowReposition 100ms
    // 兼容模式下 WinEvent 更频繁，延长延迟减少不必要的重定位
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
            LogDebug("[SHELL-COMPANION] MENUPOPUPSTART: fullscreen hidden, restoring immediately\n");
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
            LogDebug("[SHELL-COMPANION] MENUPOPUPSTART: lock ON, frozen=(%d,%d,%d,%d)"
                     " from s_lastGoodTaskbarRect_\n",
                     s_frozenTaskbarRect_.left,
                     s_frozenTaskbarRect_.top,
                     s_frozenTaskbarRect_.right,
                     s_frozenTaskbarRect_.bottom);
        } else {
            // 降级：冷启动时 s_lastGoodTaskbarRect_ 尚未初始化
            // 优先取当前绑定的任务栏（多显示器下可能是副屏 Shell_SecondaryTrayWnd）
            HWND tb = s_instance_ ? s_instance_->GetTaskbarHandle() : nullptr;
            if (!tb || !::IsWindow(tb)) tb = ::FindWindowW(L"Shell_TrayWnd", nullptr);
            if (tb) {
                ::GetWindowRect(tb, &s_frozenTaskbarRect_);
                LogDebug("[SHELL-COMPANION] MENUPOPUPSTART: lock ON, frozen=(%d,%d,%d,%d)"
                         " from GetWindowRect (cold boot fallback)\n",
                         s_frozenTaskbarRect_.left,
                         s_frozenTaskbarRect_.top,
                         s_frozenTaskbarRect_.right,
                         s_frozenTaskbarRect_.bottom);
            }
        }
    } else if (event == EVENT_SYSTEM_MENUPOPUPEND) {
        LogDebug("[SHELL-COMPANION] MENUPOPUPEND: scheduling unlock (300ms)\n");
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
            LogDebug("[SHELL-COMPANION] ForegroundHook: StartMenu ON, lock set\n");
        }
    } else if (s_lockedByStartMenuFg_) {
        // 前台离开 Start Menu → 300ms 后解锁
        s_lockedByStartMenuFg_ = false;
        ::SetTimer(s_lyricsWnd_, 4, 300, nullptr);
        LogDebug("[SHELL-COMPANION] ForegroundHook: StartMenu OFF, unlock in 300ms\n");
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

    // UIA 由后台工作线程自建（COM + IUIAutomation 均在工作线程上），
    // UI 线程不再直接初始化或调用 UIA

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
    geometry_.ShutdownUiaWorker();
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
            LogDebug("[SHELL-COMPANION] CheckResize: s_shellInteractionLocked_ timeout (5s), force reset\n");
        } else {
            return;
        }
    }

    RECT tb{};
    ::GetWindowRect(hTaskbar_, &tb);

    // 始终置顶：任务栏滑出屏幕时用最后可见矩形参与比较，
    // 避免隐藏期间每帧重复触发重定位
    RECT tbEff = tb;
    if (taskbarAutoHide_ && alwaysShowLyrics_ && !ComputeTaskbarVisible(tb) &&
        lastVisibleTbRect_.right > lastVisibleTbRect_.left) {
        tbEff = lastVisibleTbRect_;
    }

    if (!::EqualRect(&tbEff, &lastTaskbarRect_)) {
        LogDebug("[SHELL-COMPANION] CheckResize: rect changed "
                 "old=(%d,%d,%d,%d) new=(%d,%d,%d,%d) → posting WM_DELAYED_REPOSITION\n",
                 lastTaskbarRect_.left, lastTaskbarRect_.top,
                 lastTaskbarRect_.right, lastTaskbarRect_.bottom,
                 tb.left, tb.top, tb.right, tb.bottom);

        // 使用 PostMessage 延迟定位：确保任何已排队的 MENUPOPUPSTART 事件优先处理
        RequestReposition(lyricsWnd);
    } else if (taskbarAutoHide_) {
        // 自动隐藏模式：即使矩形未变，任务栏可能已滑出/滑入
        const bool tbIsVisible = ComputeTaskbarVisible(tb);
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

// ── 自动隐藏可见性检测（Win11 滑出式） ──

bool ShellCompanion::GetTaskbarMonitorRect(RECT& rcMonitor) const {
    if (!hTaskbar_) return false;
    HMONITOR hMon = ::MonitorFromWindow(hTaskbar_, MONITOR_DEFAULTTONEAREST);
    if (!hMon) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(hMon, &mi)) return false;
    rcMonitor = mi.rcMonitor;
    return true;
}

bool ShellCompanion::ComputeTaskbarVisible(const RECT& tb) const {
    RECT mon{};
    if (!GetTaskbarMonitorRect(mon)) {
        // 无法获取显示器信息：退回旧的“尺寸阈值”判断（Win10 行为）
        const int size = (info_.position == TaskbarPosition::LEFT ||
                          info_.position == TaskbarPosition::RIGHT)
                             ? (tb.right - tb.left) : (tb.bottom - tb.top);
        return size >= kAutoHideVisibleMinPx;
    }
    int visible = 0;
    switch (info_.position) {
    case TaskbarPosition::TOP:    visible = tb.bottom - mon.top;    break;
    case TaskbarPosition::LEFT:   visible = tb.right - mon.left;    break;
    case TaskbarPosition::RIGHT:  visible = mon.right - tb.left;    break;
    case TaskbarPosition::BOTTOM:
    default:                      visible = mon.bottom - tb.top;    break;
    }
    return visible >= kAutoHideVisibleMinPx;
}

bool ShellCompanion::TaskbarFullyOnScreen(const RECT& tb) const {
    RECT mon{};
    if (!GetTaskbarMonitorRect(mon)) return true;  // 无法判断时保守记录
    return tb.left  >= mon.left  - 2 &&
           tb.top   >= mon.top   - 2 &&
           tb.right <= mon.right + 2 &&
           tb.bottom <= mon.bottom + 2;
}

void ShellCompanion::PositionLyricsInTaskbar(
    HWND lyricsWnd,
    const std::string& displayMode,
    int dragOffsetX, int dragOffsetY,
    RECT& inOutLastPosRect,
    int dynamicWidthDip,
    int cardMaxExpandRatio,
    int widthOverrideDip) {
    if (!lyricsWnd || !hTaskbar_) return;

    // 全屏隐藏期间跳过定位
    if (embedder_.IsFullscreenHidden()) return;

    // Start Menu 冻结期间跳过重定位
    const bool useFrozen = s_shellInteractionLocked_ && s_frozenTaskbarRect_.right != 0;
    if (useFrozen) {
        LogDebug("[SHELL-COMPANION] PositionLyricsInTaskbar: frozen → skip\n");
        return;
    }

    // 记录旧方位，用于检测方向变化
    // 方位变化检测由调用方（TaskbarWindow）在 InternalPosition 中处理
    DetectTaskbarInfo();

    // ── 自动隐藏状态跟踪 ──
    if (taskbarAutoHide_) {
        RECT tbCurrent{};
        ::GetWindowRect(hTaskbar_, &tbCurrent);
        // Win11 自动隐藏任务栏滑出屏幕时窗口尺寸不变（仅位置移出），
        // 高度阈值检测失效 → 按“屏幕内可见部分”判断
        taskbarVisible_ = ComputeTaskbarVisible(tbCurrent);
    } else {
        taskbarVisible_ = true;
    }

    RECT tbRect{};
    ::GetWindowRect(hTaskbar_, &tbRect);

    // ── 始终置顶：任务栏滑出屏幕时保持歌词在最后可见位置 ──
    // 任务栏完整在屏幕内时记录矩形（滑动过程中间帧不记录，避免记录到
    // 半滑出状态）；已滑出且开启置顶时，用最后可见矩形替代当前矩形
    if (TaskbarFullyOnScreen(tbRect)) {
        lastVisibleTbRect_ = tbRect;
    } else if (taskbarAutoHide_ && !taskbarVisible_ && alwaysShowLyrics_ &&
               lastVisibleTbRect_.right > lastVisibleTbRect_.left) {
        tbRect = lastVisibleTbRect_;
    }

    lastTaskbarRect_ = tbRect;
    // 同步稳定 rect 到静态缓存，供 ShellMenuWinEventProc 冻结快照使用
    s_lastGoodTaskbarRect_ = tbRect;
    const int tbWidth  = tbRect.right  - tbRect.left;
    const int tbHeight = tbRect.bottom - tbRect.top;

    // ── P2 节流：UIA 枚举委托后台工作线程 ──
    // UIA（ElementFromHandle/FindAll/BoundingRectangle）是无超时的跨进程 COM，
    // 若在 UI 线程直接发起，会与设置应用等前台 XAML 窗口形成双向等待死锁
    // （WER MoAppHangXProc：本程序 ↔ immersivecontrolpanel，任务栏自动隐藏
    // 切换时卡死）。因此 UI 线程只做两件安全的事：
    //   1) 非阻塞请求后台枚举（内部 200ms 节流）
    //   2) 读取工作线程发布的快照
    // 快照缺失/过期时降级到 EnumChildWindows（不发送跨进程消息，UI 线程安全）
    RECT taskListRect = {};
    bool foundTaskList = false;
    RECT trayRect = {};
    bool foundTray = false;
    RECT rebarRect = {};
    bool foundRebar = false;
    RECT tbCheck{};           bool tbCheckValid = false;
    RECT taskListCheck{};     bool taskListCheckValid = false;

    if (HasCompatFlag(CompatNoUIA)) {
        // 兼容模式：不使用 UIA，UI 线程直接 EnumChildWindows
        geometry_.GetChildRectsByEnum(hTaskbar_, taskListRect, foundTaskList,
                                      trayRect, foundTray,
                                      rebarRect, foundRebar, tbWidth);
    } else {
        geometry_.RequestUiaRefresh(hTaskbar_, tbWidth,
                                    !HasCompatFlag(CompatNoFrameLock));
        UiaSnapshot snap;
        if (geometry_.LoadFreshUiaSnapshot(snap, 1000)) {
            taskListRect = snap.taskList; foundTaskList = snap.hasTaskList;
            trayRect    = snap.tray;      foundTray    = snap.hasTray;
            rebarRect   = snap.rebar;     foundRebar   = snap.hasRebar;
            tbCheck = snap.tbCheck;             tbCheckValid = snap.tbCheckValid;
            taskListCheck = snap.taskListCheck; taskListCheckValid = snap.taskListCheckValid;
        } else {
            // 启动初期 / UIA 不可用 / 快照过期：UI 线程 EnumChildWindows 兜底
            geometry_.GetChildRectsByEnum(hTaskbar_, taskListRect, foundTaskList,
                                          trayRect, foundTray,
                                          rebarRect, foundRebar, tbWidth);
        }
    }

    // ── 帧锁定（扩展）：双采样检测任务栏本体 + 任务列表子窗口的稳定性 ──
    // 兼容模式 NoFrameLock：跳过双采样（修改后的 Shell 容易误判为不稳定）
    if (!HasCompatFlag(CompatNoFrameLock)) {
        if (!tbCheckValid) {
            // 枚举兜底路径：UI 线程自做二次采样
            //（GetWindowRect/EnumChildWindows 不发送跨进程消息，可安全执行）
            ::Sleep(2);
            tbCheckValid = ::GetWindowRect(hTaskbar_, &tbCheck) != 0;
            if (foundTaskList) {
                taskListCheckValid = geometry_.EnumFindTaskList(hTaskbar_, tbWidth,
                                                                taskListCheck);
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
                LogDebug("[SHELL-COMPANION] Instability: dW=%d dH=%d dTaskList=%d"
                         " → fallback stable tbRect=(%d,%d,%d,%d)\n",
                         deltaW, deltaH, deltaTaskList,
                         stableTaskbarRect_.left, stableTaskbarRect_.top,
                         stableTaskbarRect_.right, stableTaskbarRect_.bottom);
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
    } else {
        // 兼容模式：跳过双采样，直接信任当前测量值
        stableTaskbarRect_ = tbRect;
    }

    // ── UIA 几何结果合理性校验 ──
    // 子窗口矩形异常时 availableWidth 会被压缩到最小宽度（窗口只剩封面+几个字），
    // 这里对检测结果做边界检查，非法时回退到不使用该子窗口（用整段任务栏宽度）。
    if (foundTray) {
        if (trayRect.right <= trayRect.left || trayRect.bottom <= trayRect.top ||
            trayRect.left < tbRect.left || trayRect.left >= tbRect.right) {
            LogDebug("[SHELL-COMPANION] Pos: trayRect invalid (%d,%d,%d,%d), ignore\n",
                     trayRect.left, trayRect.top, trayRect.right, trayRect.bottom);
            foundTray = false;
        }
    }
    if (foundTaskList) {
        const bool overlapTray = foundTray && (taskListRect.right >= trayRect.left - 2);
        if (taskListRect.right <= taskListRect.left || taskListRect.bottom <= taskListRect.top ||
            taskListRect.right < tbRect.left || taskListRect.right > tbRect.right ||
            overlapTray) {
            LogDebug("[SHELL-COMPANION] Pos: taskListRect invalid (%d,%d,%d,%d)"
                     " overlapTray=%d, ignore\n",
                     taskListRect.left, taskListRect.top, taskListRect.right, taskListRect.bottom,
                     overlapTray ? 1 : 0);
            foundTaskList = false;
        }
    }

    // 歌词区尺寸（根据显示模式选择不同的高度）
    const bool isCardMode = (displayMode == "card");
    const int lyricH = ::MulDiv(
        isCardMode ? constants::CARD_HEIGHT_BASE_DP : constants::LYRIC_HEIGHT_BASE_DP,
        info_.dpi, 96);
    int w = 0, h = lyricH, x = 0, y = 0;
    int rightEdge = tbRect.right;
    int availableWidth = 0;

    switch (info_.position) {
    case TaskbarPosition::BOTTOM: {
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

        // 卡片模式动态宽度：仅当测量值超过默认宽度时才扩展
        // cardMaxExpandRatio: 0=默认(扩展1/3, 即1.5x基础), 1=50%(2x基础), 2=100%(无上限)
        int cardMaxDip = isCardMode ? constants::CARD_MIN_WIDTH_BASE_DP * 2 : constants::MAX_LYRIC_WIDTH_BASE_DP;
        if (isCardMode && dynamicWidthDip > constants::CARD_MIN_WIDTH_BASE_DP * 2) {
            if (cardMaxExpandRatio >= 2) {
                // 100%: 无硬性上限，仅受可用空间限制
                cardMaxDip = dynamicWidthDip;
            } else if (cardMaxExpandRatio == 1) {
                // 50%: 扩展到2倍基础宽度
                cardMaxDip = std::min(dynamicWidthDip, constants::CARD_MIN_WIDTH_BASE_DP * 4);
            } else {
                // 默认: 扩展到1.5倍基础宽度
                cardMaxDip = std::min(dynamicWidthDip, constants::CARD_MIN_WIDTH_BASE_DP * 3);
            }
        }
        const int maxLyricWidth = ::MulDiv(cardMaxDip, info_.dpi, 96);
        availableWidth = rightEdge - tbRect.left;
        // Win10 守卫：MSTaskListWClass 覆盖整个任务栏时忽略 taskListRect
        if (foundTaskList && (taskListRect.right - taskListRect.left) < tbWidth * 0.9) {
            availableWidth = rightEdge - taskListRect.right;
        }

        // 手动宽度覆盖：作为最小宽度（不受 360 上限约束），
        // 用于任务栏空闲区域检测异常导致窗口被压缩到过窄的场景
        int targetW = std::min(maxLyricWidth,
                               std::max(availableWidth, constants::MIN_LYRIC_AVAILABLE_WIDTH));
        if (widthOverrideDip > 0) {
            targetW = std::max(targetW, ::MulDiv(widthOverrideDip, info_.dpi, 96));
        }
        w = std::min(targetW, tbWidth - 2);
        w = std::max(w, constants::MIN_WINDOW_WIDTH);
        x = rightEdge - w + dragOffsetX;
        y = tbRect.top + dragOffsetY;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::TOP: {
        if (foundTray) rightEdge = trayRect.left;

        // 卡片模式动态宽度：仅当测量值超过默认宽度时才扩展
        int cardMaxDip2 = isCardMode ? constants::CARD_MIN_WIDTH_BASE_DP * 2 : constants::MAX_LYRIC_WIDTH_BASE_DP;
        if (isCardMode && dynamicWidthDip > constants::CARD_MIN_WIDTH_BASE_DP * 2) {
            if (cardMaxExpandRatio >= 2) {
                cardMaxDip2 = dynamicWidthDip;
            } else if (cardMaxExpandRatio == 1) {
                cardMaxDip2 = std::min(dynamicWidthDip, constants::CARD_MIN_WIDTH_BASE_DP * 4);
            } else {
                cardMaxDip2 = std::min(dynamicWidthDip, constants::CARD_MIN_WIDTH_BASE_DP * 3);
            }
        }
        const int maxLyricWidth = ::MulDiv(cardMaxDip2, info_.dpi, 96);
        availableWidth = rightEdge - tbRect.left;
        // Win10 守卫：MSTaskListWClass 覆盖整个任务栏时忽略 taskListRect
        if (foundTaskList && (taskListRect.right - taskListRect.left) < tbWidth * 0.9) {
            availableWidth = rightEdge - taskListRect.right;
        }

        int targetW = std::min(maxLyricWidth,
                               std::max(availableWidth, constants::MIN_LYRIC_AVAILABLE_WIDTH));
        if (widthOverrideDip > 0) {
            targetW = std::max(targetW, ::MulDiv(widthOverrideDip, info_.dpi, 96));
        }
        w = std::min(targetW, tbWidth - 2);
        w = std::max(w, constants::MIN_WINDOW_WIDTH);
        x = rightEdge - w + dragOffsetX;
        y = tbRect.top + dragOffsetY;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::LEFT: {
        w = ::MulDiv(constants::VERTICAL_TASKBAR_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
        if (widthOverrideDip > 0) {
            w = std::max(w, ::MulDiv(widthOverrideDip, info_.dpi, 96));
        }
        // 垂直任务栏必须以实际任务栏宽度为硬上限。此前再套用
        // MIN_WINDOW_WIDTH 会在窄任务栏（或自动隐藏的瞬态宽度）中把窗口撑到栏外。
        w = std::clamp(w, 1, std::max(1, tbWidth - 2));
        x = tbRect.right - w;

        int availableBottom = tbRect.bottom;
        if (foundTray) {
            availableBottom = trayRect.top;
        }

        const int maxLyricHeight = ::MulDiv(
            isCardMode ? constants::CARD_HEIGHT_BASE_DP * 4 : constants::LYRIC_HEIGHT_BASE_DP * 6,
            info_.dpi, 96);
        int availableH = availableBottom - tbRect.top;
        h = std::min(maxLyricHeight, std::max(1, availableH));

        y = tbRect.top + (availableH - h) / 2 + dragOffsetY;
        break;
    }
    case TaskbarPosition::RIGHT: {
        w = ::MulDiv(constants::VERTICAL_TASKBAR_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
        if (widthOverrideDip > 0) {
            w = std::max(w, ::MulDiv(widthOverrideDip, info_.dpi, 96));
        }
        w = std::clamp(w, 1, std::max(1, tbWidth - 2));
        x = tbRect.left;

        int availableBottom = tbRect.bottom;
        if (foundTray) {
            availableBottom = trayRect.top;
        }

        const int maxLyricHeight = ::MulDiv(
            isCardMode ? constants::CARD_HEIGHT_BASE_DP * 4 : constants::LYRIC_HEIGHT_BASE_DP * 6,
            info_.dpi, 96);
        int availableH = availableBottom - tbRect.top;
        h = std::min(maxLyricHeight, std::max(1, availableH));

        y = tbRect.top + (availableH - h) / 2 + dragOffsetY;
        break;
    }
    default:
        x = tbRect.left; y = tbRect.top; w = tbWidth; h = tbHeight;
        break;
    }

    if (info_.position == TaskbarPosition::LEFT ||
        info_.position == TaskbarPosition::RIGHT) {
        w = std::clamp(w, 1, std::max(1, tbWidth));
        h = std::clamp(h, 1, std::max(1, tbHeight));
    } else {
        w = std::max(w, constants::MIN_WINDOW_WIDTH);
        h = std::max(h, lyricH);
    }

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
        // 上/下任务栏的窗口高度等于任务栏高度。若复用了纵向配置中的
        // offsetY，必须把 y 收回任务栏，避免窗口整体漂到任务栏外。
        if (y < tbRect.top) y = tbRect.top;
        if (y + h > tbRect.bottom) y = tbRect.bottom - h;
        if (y < tbRect.top) y = tbRect.top;
    }

    // 调试日志
    LogDebug("[SHELL-COMPANION] Pos: pos=%d x=%d y=%d w=%d h=%d "
             "tbRect=(%d,%d,%d,%d) taskList=(%d,%d,%d,%d) foundTL=%d "
             "tray=(%d,%d,%d,%d) foundTray=%d rebar=(%d,%d,%d,%d) foundRebar=%d "
             "rightEdge=%d availableWidth=%d widthOverride=%d dragOffX=%d\n",
             static_cast<int>(info_.position), x, y, w, h,
             tbRect.left, tbRect.top, tbRect.right, tbRect.bottom,
             taskListRect.left, taskListRect.top, taskListRect.right, taskListRect.bottom,
             foundTaskList ? 1 : 0,
             trayRect.left, trayRect.top, trayRect.right, trayRect.bottom,
             foundTray ? 1 : 0,
             rebarRect.left, rebarRect.top, rebarRect.right, rebarRect.bottom,
             foundRebar ? 1 : 0,
             rightEdge, availableWidth, widthOverrideDip,
             dragOffsetX);

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
// Explorer 重启恢复
// ═════════════════════════════════════════

void ShellCompanion::Rebind(HWND hNewTaskbar, HWND lyricsWnd) {
    if (!hNewTaskbar) return;

    Log("[ShellCompanion] Rebind: old=%p new=%p\n", hTaskbar_, hNewTaskbar);

    // 1) 更新句柄
    hTaskbar_ = hNewTaskbar;

    // 2) 重检测任务栏几何
    info_ = geometry_.Detect(hTaskbar_);
    taskbarAutoHide_ = info_.autoHide;

    // 3) 重装 WinEvent 钩子
    RemoveHooks();
    InstallHooks(lyricsWnd);

    // 4) 重置稳定跟踪状态
    lastTaskbarRect_ = {0, 0, 0, 0};
    stableTaskbarRect_ = {0, 0, 0, 0};
    stableTaskListRect_ = {0, 0, 0, 0};
    stableTaskListValid_ = false;
    cachedRightEdgeOffset_ = 0;

    // 5) 重置冻结锁
    s_shellInteractionLocked_ = false;
    s_lockedByStartMenuFg_ = false;
    s_frozenTaskbarRect_ = {0, 0, 0, 0};
    s_lastGoodTaskbarRect_ = {0, 0, 0, 0};

    Log("[ShellCompanion] Rebind: complete, new handle=%p\n", hNewTaskbar);
}

// ═════════════════════════════════════════
// Shell 交互锁定管理
// ═════════════════════════════════════════

void ShellCompanion::UnlockShellInteraction(int timerId) {
    if (timerId == 3) {
        LogDebug("[SHELL-COMPANION] Timer3: unlocking s_shellInteractionLocked_\n");
    } else if (timerId == 4) {
        LogDebug("[SHELL-COMPANION] Timer4: unlock (ForegroundHook)\n");
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
