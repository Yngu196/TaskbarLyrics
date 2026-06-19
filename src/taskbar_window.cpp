// SPDX-License-Identifier: GPL-2.0
// taskbar_window.cpp - 任务栏歌词窗口实现
// Win11 兼容方案: 独立浮动窗口覆盖在任务栏上方
// (类似 TrafficMonitor / TranslucentTB 的实现方式)
#include "taskbar_window.h"
#include "constants.h"
#include "logger.h"

#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

namespace moekoe {

// ---- 静态: SetWinEventHook ----
HWINEVENTHOOK TaskbarWindow::s_taskbarHook_ = nullptr;
HWINEVENTHOOK TaskbarWindow::s_shellMenuHook_ = nullptr;
HWND TaskbarWindow::s_lyricsWnd_ = nullptr;
bool TaskbarWindow::s_shellInteractionLocked_ = false;
std::chrono::steady_clock::time_point TaskbarWindow::s_shellInteractionLockedTime_{};
RECT TaskbarWindow::s_frozenTaskbarRect_{};
RECT TaskbarWindow::s_lastGoodTaskbarRect_{};
HWINEVENTHOOK TaskbarWindow::s_foregroundHook_      = nullptr;
bool          TaskbarWindow::s_lockedByStartMenuFg_ = false;
TaskbarWindow* TaskbarWindow::s_instance_           = nullptr;
bool          TaskbarWindow::s_forceDebounceReset_  = false;

void CALLBACK TaskbarWinEventProc(HWINEVENTHOOK, DWORD, HWND hWnd,
                                   LONG idObject, LONG, DWORD, DWORD) {
    // 只响应 Shell_TrayWnd 的位置变化
    if (idObject != OBJID_WINDOW || !hWnd) return;
    wchar_t cls[32] = {};
    ::GetClassNameW(hWnd, cls, 31);
    if (wcscmp(cls, L"Shell_TrayWnd") != 0) return;

    // 投递延迟消息：10ms 等 DWM 帧完成，避免在动画中定位
    if (TaskbarWindow::s_lyricsWnd_) {
        ::PostMessageW(TaskbarWindow::s_lyricsWnd_,
                       TaskbarWindow::WM_DELAYED_REPOSITION, 0, 0);
    }
}

void CALLBACK ShellMenuWinEventProc(HWINEVENTHOOK, DWORD event, HWND,
                                     LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_MENUPOPUPSTART) {
        // 锁定定位 + 冻结任务栏几何快照
        // Explorer 在 Start Menu 激活期间会临时改动任务栏内部布局（托盘重排、
        // client width 微变），导致 GetWindowRect 返回值被污染（right 膨胀 +3~7px）。
        // 使用 s_lastGoodTaskbarRect_（PositionLyricsInTaskbar 非冻结模式时缓存的最后稳定 rect），
        // 彻底隔离 Explorer 的瞬时脏写。
        TaskbarWindow::s_shellInteractionLocked_ = true;
        TaskbarWindow::s_shellInteractionLockedTime_ = std::chrono::steady_clock::now();
        if (TaskbarWindow::s_lastGoodTaskbarRect_.right != 0) {
            TaskbarWindow::s_frozenTaskbarRect_ = TaskbarWindow::s_lastGoodTaskbarRect_;
            wchar_t dbg[160];
            swprintf_s(dbg, L"[TaskbarLyrics] MENUPOPUPSTART: lock ON, frozen=(%d,%d,%d,%d)"
                       L" from s_lastGoodTaskbarRect_\n",
                       TaskbarWindow::s_frozenTaskbarRect_.left,
                       TaskbarWindow::s_frozenTaskbarRect_.top,
                       TaskbarWindow::s_frozenTaskbarRect_.right,
                       TaskbarWindow::s_frozenTaskbarRect_.bottom);
            ::OutputDebugStringW(dbg);
        } else {
            // 降级：冷启动时 s_lastGoodTaskbarRect_ 尚未初始化（首次定位尚未执行）
            HWND tb = ::FindWindowW(L"Shell_TrayWnd", nullptr);
            if (tb) {
                ::GetWindowRect(tb, &TaskbarWindow::s_frozenTaskbarRect_);
                wchar_t dbg[160];
                swprintf_s(dbg, L"[TaskbarLyrics] MENUPOPUPSTART: lock ON, frozen=(%d,%d,%d,%d)"
                           L" from GetWindowRect (cold boot fallback)\n",
                           TaskbarWindow::s_frozenTaskbarRect_.left,
                           TaskbarWindow::s_frozenTaskbarRect_.top,
                           TaskbarWindow::s_frozenTaskbarRect_.right,
                           TaskbarWindow::s_frozenTaskbarRect_.bottom);
                ::OutputDebugStringW(dbg);
            }
        }

        // 全屏隐藏时，按 Win 键呼出开始菜单应该立即恢复歌词显示。
        // 不等待防抖（全屏应用可能仍占前台 → main.cpp IsForegroundFullscreen 持续为 true → 迟迟不恢复）。
        // 此处直接调用 SetFullscreenHidden(false) 恢复窗口，并设置标志位通知 main.cpp 重置防抖计数器。
        if (TaskbarWindow::s_instance_ && TaskbarWindow::s_instance_->fullscreenHidden_) {
            ::OutputDebugStringW(L"[TaskbarLyrics] MENUPOPUPSTART: fullscreen hidden, restoring immediately\n");
            TaskbarWindow::s_instance_->SetFullscreenHidden(false);
        }
    } else if (event == EVENT_SYSTEM_MENUPOPUPEND) {
        ::OutputDebugStringW(L"[TaskbarLyrics] MENUPOPUPEND: scheduling unlock (300ms)\n");
        if (TaskbarWindow::s_lyricsWnd_) {
            ::SetTimer(TaskbarWindow::s_lyricsWnd_, 3, 300, nullptr);
        }
    }
}

void CALLBACK ForegroundWinEventProc(HWINEVENTHOOK, DWORD, HWND hWnd,
                                      LONG, LONG, DWORD, DWORD) {
    if (!hWnd || !TaskbarWindow::s_lyricsWnd_) return;

    // 获取前台窗口的可执行文件名
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
        if (!TaskbarWindow::s_shellInteractionLocked_) {
            TaskbarWindow::s_shellInteractionLocked_ = true;
            TaskbarWindow::s_shellInteractionLockedTime_ = std::chrono::steady_clock::now();
            TaskbarWindow::s_lockedByStartMenuFg_    = true;
            if (TaskbarWindow::s_lastGoodTaskbarRect_.right != 0) {
                TaskbarWindow::s_frozenTaskbarRect_ = TaskbarWindow::s_lastGoodTaskbarRect_;
            }
            ::OutputDebugStringW(
                L"[TaskbarLyrics] ForegroundHook: StartMenu ON, lock set\n");
        }
    } else if (TaskbarWindow::s_lockedByStartMenuFg_) {
        // 前台离开 Start Menu → 300ms 后解锁
        TaskbarWindow::s_lockedByStartMenuFg_ = false;
        ::SetTimer(TaskbarWindow::s_lyricsWnd_, 4, 300, nullptr);
        ::OutputDebugStringW(
            L"[TaskbarLyrics] ForegroundHook: StartMenu OFF, unlock in 300ms\n");
    }
}

void TaskbarWindow::InstallTaskbarHook(HWND lyricsWnd) {
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
    // StartMenuExperienceHost.exe 是独立进程，不触发 MENUPOPUPSTART，
    // 因此需要 EVENT_SYSTEM_FOREGROUND 来捕获前台切换。
    s_foregroundHook_ = ::SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, ForegroundWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void TaskbarWindow::RemoveTaskbarHook() {
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

// ---- 静态: 任务栏句柄查找 ----
HWND TaskbarWindow::FindTaskbarHandle() {
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

TaskbarWindow::TaskbarWindow() = default;

TaskbarWindow::~TaskbarWindow() {
    Destroy();
}

bool TaskbarWindow::Create(HINSTANCE hInstance, HWND hParent) {
    if (created_) return true;
    hInstance_ = hInstance;

    // 1) 解析任务栏
    hTaskbar_ = hParent ? hParent : FindTaskbarHandle();
    if (!hTaskbar_) return false;

    // 2) 注册窗口类(幂等)
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = &TaskbarWindow::WndProc;
    wc.cbWndExtra    = sizeof(TaskbarWindow*);
    wc.hInstance     = hInstance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // 透明背景
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    // 3) 创建独立浮动窗口 (不嵌入任务栏)
    //   WS_EX_TOOLWINDOW   : 不在任务栏显示
    //   WS_EX_LAYERED      : 支持透明 (配合 UpdateLayeredWindow)
    //   不使用 WS_EX_TOPMOST ：Start Menu / Shell UI 不会与歌词冲突（酷狗方案）
    //   注意: 不使用 WS_EX_TRANSPARENT，以便接收鼠标消息
    const DWORD exStyle = WS_EX_NOACTIVATE |
                          WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    const DWORD style   = WS_POPUP;

    hwnd_ = ::CreateWindowExW(
        exStyle,
        kWindowClass,
        L"",
        style,
        0, 0, 0, 0,
        nullptr,         // 无父窗口 - 独立浮动
        nullptr,
        hInstance,
        this);
    if (!hwnd_) return false;

    // 设为 Shell_TrayWnd 的 owned window，使其在天生处于任务栏 Z-order 组内
    // 始终渲染在任务栏上方，无需 HWND_TOPMOST 竞争，不受 Win 键 Start Menu 影响
    ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hTaskbar_));

    // 4) 初始化 UI Automation（用于获取任务栏子窗口几何，替代 EnumChildWindows）
    InitUIAutomation();

    // 5) 初始化信息并定位
    DetectTaskbarInfo();
    PositionLyricsInTaskbar();

    // 6) 监听任务栏位置变化（auto-hide 滑出、Win 键、DPI 变化等）
    InstallTaskbarHook(hwnd_);

    s_instance_ = this;
    created_ = true;
    return true;
}

void TaskbarWindow::Destroy() {
    s_instance_ = nullptr;
    RemoveTaskbarHook();
    CleanupUIAutomation();
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    created_ = false;
}

void TaskbarWindow::SetFullscreenHidden(bool hidden) {
    if (fullscreenHidden_ == hidden) return;
    fullscreenHidden_ = hidden;
    if (!hwnd_) return;

    if (hidden) {
        Log("[TaskbarWindow] SetFullscreenHidden(true): hiding window (hwnd=%p)\n", hwnd_);

        // 全屏隐藏时主动释放 Start Menu 冻结锁：
        // 全屏游戏激活时常触发 MENUPOPUPSTART 全局事件却永不发送 MENUPOPUPEND，
        // 导致 s_shellInteractionLocked_ 永久锁死，CheckResize/PositionLyricsInTaskbar 冻结。
        // 此处强制解冻，确保后续恢复显示时定位正常。
        if (s_shellInteractionLocked_) {
            s_shellInteractionLocked_ = false;
            s_lockedByStartMenuFg_ = false;
            Log("[TaskbarWindow] SetFullscreenHidden: released freeze lock\n");
        }

        // 使用 SetWindowPos(SWP_HIDEWINDOW) 而非 ShowWindow(SW_HIDE)：
        // ShowWindow 会发送 WM_SHOWWINDOW 消息，owned window 的 owner (Shell_TrayWnd)
        // 可能拦截或覆盖显隐状态。SWP_HIDEWINDOW 直接操作 WS_VISIBLE 位，更可靠。
        // 同时移到屏幕外作为双重保险（防止 UpdateLayeredWindow 在隐藏过程中闪现）。
        ::SetWindowPos(hwnd_, nullptr,
                       -32000, -32000, 0, 0,
                       SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Log("[TaskbarWindow] SetFullscreenHidden: SWP_HIDEWINDOW + offscreen done\n");
    } else {
        Log("[TaskbarWindow] SetFullscreenHidden(false): restoring window\n");
        // 通知 main.cpp 重置全屏检测防抖计数器。
        // Shell 交互（MENUPOPUPSTART）触发的恢复需要防抖清零，否则下一帧全屏检测读到
        // 前台仍是全屏应用（isFullscreen==true），会继续往 fullscreen 方向计数。
        // 正常恢复路径（全屏应用关闭）也设此标志，无害（idempotent reset）。
        s_forceDebounceReset_ = true;
        // 恢复显示：移回可见区域（具体位置由下一帧 PositionLyricsInTaskbar 重新计算）
        // HWND_TOPMOST 确保从 owned Z-order 中脱离后正确叠放
        ::SetWindowPos(hwnd_, HWND_TOPMOST,
                       0, 0, 0, 0,
                       SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        Log("[TaskbarWindow] SetFullscreenHidden: SWP_SHOWWINDOW done, awaiting reposition\n");
    }
}

void TaskbarWindow::DetectTaskbarInfo() {
    if (!hTaskbar_) return;

    // 工作区
    HMONITOR mon = ::MonitorFromWindow(hTaskbar_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(mon, &mi);

    // 任务栏外接矩形
    RECT tb{};
    ::GetWindowRect(hTaskbar_, &tb);
    info_.rect = tb;

    // 推断方位
    const int tbWidth  = tb.right - tb.left;
    const int tbHeight = tb.bottom - tb.top;
    if (tbWidth > tbHeight) {
        if (tb.top >= (mi.rcWork.bottom + mi.rcWork.top) / 2) {
            info_.position = TaskbarPosition::BOTTOM;
        } else {
            info_.position = TaskbarPosition::TOP;
        }
    } else {
        if (tb.left >= (mi.rcWork.right + mi.rcWork.left) / 2) {
            info_.position = TaskbarPosition::RIGHT;
        } else {
            info_.position = TaskbarPosition::LEFT;
        }
    }

    // DPI
    info_.dpi = ::GetDpiForWindow(hTaskbar_);

    // APPBAR 自动隐藏检测
    APPBARDATA abdState{};
    abdState.cbSize = sizeof(abdState);
    const UINT appBarState = ::SHAppBarMessage(ABM_GETSTATE, &abdState);
    info_.autoHide = (appBarState & ABS_AUTOHIDE) != 0;
    taskbarAutoHide_ = info_.autoHide;
}

void TaskbarWindow::PositionLyricsInTaskbar() {
    if (!hwnd_ || !hTaskbar_) return;

    // 全屏隐藏期间跳过定位：SetWindowPos(SWP_SHOWWINDOW) 会重新显示已隐藏的窗口
    if (fullscreenHidden_) return;

    // Start Menu 激活期间：Explorer 内部布局不可靠（托盘重排、client width 微变、
    // 任务栏按钮区域右扩等）。所有几何输入（GetWindowRect、子窗口枚举）均不可靠。
    // 冻结期间完全跳过重定位：窗口已在前序非冻结调用中正确定位，保持不变即可。
    // 这同时修复了「弹开效果」—— taskListRect.right 受 Explorer 污染导致
    // availableWidth 收窄 → w 缩小 → x 右移（歌词被「弹开」到右侧）。
    const bool useFrozen = s_shellInteractionLocked_ && s_frozenTaskbarRect_.right != 0;
    if (useFrozen) {
        ::OutputDebugStringW(L"[TaskbarLyrics] PositionLyricsInTaskbar: frozen → skip\n");
        return;
    }

    // 记录旧方位，用于检测方向变化
    const TaskbarPosition oldPosition = info_.position;
    DetectTaskbarInfo();

    // 方位发生变化（如横屏→竖屏、底部→左侧）：
    // 旧的 dragOffset 是基于前一个方向计算的，直接复用会导致窗口坐标飞出屏幕。
    // 重置偏移量让窗口回到自动定位的默认位置。
    if (oldPosition != TaskbarPosition::UNKNOWN && oldPosition != info_.position) {
        dragOffsetX_ = 0;
        dragOffsetY_ = 0;
        ::OutputDebugStringW(L"[TaskbarLyrics] 任务栏方位变化，重置拖动偏移\n");
    }

    // ── 自动隐藏状态跟踪（不干预定位，仅记录状态）──
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
    // 避免 EVENT_SYSTEM_MENUPOPUPSTART 时 GetWindowRect 返回 Explorer 脏写的膨胀值
    s_lastGoodTaskbarRect_ = tbRect;
    const int tbWidth  = tbRect.right  - tbRect.left;
    const int tbHeight = tbRect.bottom - tbRect.top;

    // 使用 UI Automation 枚举任务栏子窗口，找到关键区域的屏幕矩形
    // 替代 EnumChildWindows + GetWindowRect：
    // Win 键按下时 Explorer 会瞬时修改子窗口内部布局（如 DesktopWindowContentBridge.right
    // 膨胀 +3~7px），GetWindowRect 直接暴露这些瞬时脏写导致歌词向右偏移。
    // UIA 的 BoundingRectangle 由 UIAutomationCore 维护、经过内部稳定化处理，
    // 不受 Explorer 瞬态脏写影响，从根本上解决偏移问题。
    RECT taskListRect = {};
    bool foundTaskList = false;
    RECT trayRect = {};
    bool foundTray = false;
    RECT rebarRect = {};
    bool foundRebar = false;

    if (!GetChildRectsByUIA(taskListRect, foundTaskList,
                            trayRect, foundTray,
                            rebarRect, foundRebar,
                            tbWidth)) {
        // 降级：UIA 不可用时回退到传统 EnumChildWindows 方式
        struct ChildInfo {
            HWND  hwnd;
            RECT  rect;
            std::wstring className;
        };
        std::vector<ChildInfo> children;
        HWND hChild = ::GetWindow(hTaskbar_, GW_CHILD);
        while (hChild) {
            if (::IsWindowVisible(hChild)) {
                ChildInfo ci{};
                ci.hwnd = hChild;
                ::GetWindowRect(hChild, &ci.rect);
                wchar_t name[256] = {};
                ::GetClassNameW(hChild, name, 256);
                ci.className = name;
                children.push_back(ci);
            }
            hChild = ::GetWindow(hChild, GW_HWNDNEXT);
        }
        for (const auto& ci : children) {
            if (ci.className == L"MSTaskListWClass") {
                taskListRect = ci.rect;
                foundTaskList = true;
            }
            if (ci.className == L"Windows.UI.Composition.DesktopWindowContentBridge") {
                int cw = ci.rect.right - ci.rect.left;
                if (cw < tbWidth - 10) {
                    taskListRect = ci.rect;
                    foundTaskList = true;
                }
            }
            if (ci.className == L"TrayNotifyWnd") {
                trayRect = ci.rect;
                foundTray = true;
            }
            if (ci.className == L"ReBarWindow32") {
                rebarRect = ci.rect;
                foundRebar = true;
            }
        }
    }

    // ── 帧锁定（扩展）：双采样检测任务栏本体 + 任务列表子窗口的稳定性 ──
    // Win11 Start Menu 不触发 EVENT_SYSTEM_MENUPOPUPSTART（独立进程），
    // 但 Win 键按下时 MSTaskListWClass / DesktopWindowContentBridge 子窗口
    // 会短暂偏移（right +N px），而 Shell_TrayWnd 本体不变。
    // 父窗口稳定性检测对此无感知 → 需同步检测子窗口。
    {
        RECT tbCheck{};
        ::Sleep(2);
        ::GetWindowRect(hTaskbar_, &tbCheck);

        // 复检任务列表子窗口，检测其是否在 2ms 内发生偏移
        RECT taskListCheck{};
        bool taskListCheckValid = false;
        if (foundTaskList && uia_) {
            // 使用 UIA 重新获取任务列表子窗口矩形（替代 GetWindow + GetWindowRect）
            IUIAutomationElement* tbElem = nullptr;
            if (SUCCEEDED(uia_->ElementFromHandle(hTaskbar_, &tbElem)) && tbElem) {
                IUIAutomationCondition* cond = nullptr;
                VARIANT var;
                VariantInit(&var);
                var.vt = VT_BSTR;

                // 第一轮：查找 MSTaskListWClass
                var.bstrVal = ::SysAllocString(L"MSTaskListWClass");
                if (SUCCEEDED(uia_->CreatePropertyCondition(
                        UIA_ClassNamePropertyId, var, &cond)) && cond) {
                    ::VariantClear(&var);
                    IUIAutomationElementArray* arr = nullptr;
                    if (SUCCEEDED(tbElem->FindAll(TreeScope_Children, cond, &arr)) && arr) {
                        int len = 0;
                        arr->get_Length(&len);
                        if (len > 0) {
                            IUIAutomationElement* match = nullptr;
                            arr->GetElement(0, &match);
                            if (match) {
                                match->get_CurrentBoundingRectangle(&taskListCheck);
                                taskListCheckValid = true;
                                match->Release();
                            }
                        }
                        arr->Release();
                    }
                    cond->Release();
                } else {
                    ::VariantClear(&var);
                }

                // 第二轮：如果未命中，尝试 DesktopWindowContentBridge
                if (!taskListCheckValid) {
                    var.vt = VT_BSTR;
                    var.bstrVal = ::SysAllocString(
                        L"Windows.UI.Composition.DesktopWindowContentBridge");
                    if (SUCCEEDED(uia_->CreatePropertyCondition(
                            UIA_ClassNamePropertyId, var, &cond)) && cond) {
                        ::VariantClear(&var);
                        IUIAutomationElementArray* arr = nullptr;
                        if (SUCCEEDED(tbElem->FindAll(TreeScope_Children, cond, &arr)) && arr) {
                            int len = 0;
                            arr->get_Length(&len);
                            for (int j = 0; j < len && !taskListCheckValid; j++) {
                                IUIAutomationElement* match = nullptr;
                                arr->GetElement(j, &match);
                                if (match) {
                                    RECT cr{};
                                    match->get_CurrentBoundingRectangle(&cr);
                                    if ((cr.right - cr.left) < tbWidth - 10) {
                                        taskListCheck = cr;
                                        taskListCheckValid = true;
                                    }
                                    match->Release();
                                }
                            }
                            arr->Release();
                        }
                        cond->Release();
                    } else {
                        ::VariantClear(&var);
                    }
                }
                tbElem->Release();
            }
        }
        // 降级：UIA 不可用时回退到传统 GetWindow 方式
        if (!taskListCheckValid && foundTaskList) {
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

        const int deltaW = abs((tbRect.right - tbRect.left) -
                               (tbCheck.right - tbCheck.left));
        const int deltaH = abs((tbRect.bottom - tbRect.top) -
                               (tbCheck.bottom - tbCheck.top));
        const int deltaTaskList = taskListCheckValid
            ? abs(taskListRect.right - taskListCheck.right) : 0;

        if (deltaW > 5 || deltaH > 5 || deltaTaskList > 3) {
            // 动画中 / Explorer 临时修改 → 回退到上次稳定值
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
            // 稳定 → 更新缓存
            stableTaskbarRect_ = tbRect;
            if (taskListCheckValid) {
                stableTaskListRect_ = taskListRect;
                stableTaskListValid_ = true;
            }
        }
    }

    // 歌词区尺寸（根据显示模式选择不同的高度）
    const bool isCardMode = (displayMode_ == "card");
    const int lyricH = ::MulDiv(
        isCardMode ? constants::CARD_HEIGHT_BASE_DP : constants::LYRIC_HEIGHT_BASE_DP,
        info_.dpi, 96);
    int w = 0, h = lyricH, x = 0, y = 0;

    switch (info_.position) {
    case TaskbarPosition::BOTTOM: {
        // 底部任务栏: 歌词定位在右侧（紧靠托盘左侧），不覆盖最小化窗口区域
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

        // 歌词窗口最大宽度
        const int maxLyricWidth = ::MulDiv(
            isCardMode ? constants::CARD_MIN_WIDTH_BASE_DP * 2 : constants::MAX_LYRIC_WIDTH_BASE_DP,
            info_.dpi, 96);
        int availableWidth = rightEdge - tbRect.left;
        if (foundTaskList) {
            availableWidth = rightEdge - taskListRect.right;
        }

        w = std::min(maxLyricWidth, std::max(availableWidth, constants::MIN_LYRIC_AVAILABLE_WIDTH));
        x = rightEdge - w + dragOffsetX_;
        y = tbRect.top + dragOffsetY_;
        h = tbHeight;   // 歌词高度填满任务栏
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
        x = rightEdge - w + dragOffsetX_;
        y = tbRect.top + dragOffsetY_;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::LEFT: {
        w = ::MulDiv(constants::VERTICAL_TASKBAR_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
        x = tbRect.right - w;

        // 垂直任务栏：考虑托盘区域（通常在底部），限制最大高度
        int availableBottom = tbRect.bottom;
        if (foundTray) {
            availableBottom = trayRect.top;  // 不覆盖托盘区域
        }

        // 最大高度约束：避免在纵向屏幕上窗口过高
        const int maxLyricHeight = ::MulDiv(
            isCardMode ? constants::CARD_HEIGHT_BASE_DP * 4 : constants::LYRIC_HEIGHT_BASE_DP * 6,
            info_.dpi, 96);
        int availableH = availableBottom - tbRect.top;
        h = std::min(maxLyricHeight, std::max(availableH, lyricH));

        // 垂直居中于可用空间
        y = tbRect.top + (availableH - h) / 2 + dragOffsetY_;
        break;
    }
    case TaskbarPosition::RIGHT: {
        w = ::MulDiv(constants::VERTICAL_TASKBAR_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
        x = tbRect.left;

        // 垂直任务栏：考虑托盘区域（通常在底部），限制最大高度
        int availableBottom = tbRect.bottom;
        if (foundTray) {
            availableBottom = trayRect.top;
        }

        const int maxLyricHeight = ::MulDiv(
            isCardMode ? constants::CARD_HEIGHT_BASE_DP * 4 : constants::LYRIC_HEIGHT_BASE_DP * 6,
            info_.dpi, 96);
        int availableH = availableBottom - tbRect.top;
        h = std::min(maxLyricHeight, std::max(availableH, lyricH));

        y = tbRect.top + (availableH - h) / 2 + dragOffsetY_;
        break;
    }
    default:
        x = tbRect.left; y = tbRect.top; w = tbWidth; h = tbHeight;
        break;
    }

    w = std::max(w, constants::MIN_WINDOW_WIDTH);
    h = std::max(h, lyricH);

    // 防御性检查：确保窗口坐标在任务栏范围内
    // 如果 x/y 超出任务栏矩形，强制拉回
    if (info_.position == TaskbarPosition::LEFT ||
        info_.position == TaskbarPosition::RIGHT) {
        // 垂直任务栏：确保 y 在任务栏垂直范围内
        if (y < tbRect.top) y = tbRect.top;
        if (y + h > tbRect.bottom) y = tbRect.bottom - h;
        if (y < tbRect.top) y = tbRect.top;  // h > tbHeight 时的兜底
    } else {
        // 水平任务栏：确保 x 在任务栏水平范围内
        if (x < tbRect.left) x = tbRect.left;
        if (x + w > tbRect.right) x = tbRect.right - w;
        if (x < tbRect.left) x = tbRect.left;  // w > tbWidth 时的兜底
    }

    // 调试日志：输出最终定位坐标
    {
        wchar_t dbg[256];
        swprintf_s(dbg, L"[TaskbarLyrics] Pos: pos=%d x=%d y=%d w=%d h=%d "
                   L"tbRect=(%d,%d,%d,%d) dragOffX=%d\n",
                   static_cast<int>(info_.position), x, y, w, h,
                   tbRect.left, tbRect.top, tbRect.right, tbRect.bottom,
                   dragOffsetX_);
        ::OutputDebugStringW(dbg);
    }

    // owned window 天然在 owner (任务栏) 之上，用 HWND_TOP 保持此关系
    ::SetWindowPos(
        hwnd_, HWND_TOP,
        x, y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

void TaskbarWindow::CheckResize() {
    if (!hTaskbar_) return;

    // 全屏隐藏期间跳过重检测：避免 WM_DELAYED_REPOSITION → PositionLyricsInTaskbar
    // 中 SetWindowPos(SWP_SHOWWINDOW) 重新显示已隐藏的窗口
    if (fullscreenHidden_) return;

    // Start Menu 激活期间跳过帧级重检测：冻结模式下已使用稳定快照定位，
    // 无需在此轮询 GetWindowRect 规避 Explorer 临时脏写。
    // 同时防止 MENUPOPUPSTART 尚未设置锁时 CheckResize 抢先触发未保护的 PositionLyricsInTaskbar。
    // 超时保护：若 s_shellInteractionLocked_ 超过 5 秒未解除则自动复位，防止永久锁死。
    if (s_shellInteractionLocked_) {
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kMaxLockDuration = std::chrono::seconds(5);
        if (now - s_shellInteractionLockedTime_ > kMaxLockDuration) {
            s_shellInteractionLocked_ = false;
            ::OutputDebugStringW(L"[TaskbarLyrics] CheckResize: s_shellInteractionLocked_ timeout (5s), force reset\n");
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

        // 使用 PostMessage 延迟定位：WinEvent out-of-context 钩子事件通过消息队列分发，
        // PostMessage 将 WM_DELAYED_REPOSITION 放入队列末尾，保证在此之前任何已排队的
        // MENUPOPUPSTART 事件优先处理并设置 s_shellInteractionLocked_ 锁。
        // 彻底消除 CheckResize() 在 MENUPOPUPSTART 之前抢先调用 PositionLyricsInTaskbar()
        // 导致 s_lastGoodTaskbarRect_ 被 Explorer 脏写污染的竞态窗口。
        if (hwnd_) {
            ::PostMessageW(hwnd_, WM_DELAYED_REPOSITION, 0, 0);
        }
    } else if (taskbarAutoHide_) {
        // 自动隐藏模式：即使矩形未变，任务栏可能已滑出/滑入（高度变化）
        // 重新检测以响应状态变更
        const int tbH = tb.bottom - tb.top;
        constexpr int kAutoHideThreshold = 10;
        const bool tbIsVisible = (tbH >= kAutoHideThreshold);
        if (tbIsVisible != taskbarVisible_) {
            PositionLyricsInTaskbar();
        }
    }
}

void TaskbarWindow::Reposition() {
    PositionLyricsInTaskbar();
}

HoverButton TaskbarWindow::HitTestButton(int x, int y) const {
    if (!hwnd_) return HoverButton::None;

    RECT rc{};
    ::GetWindowRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    const bool isVert = IsVerticalTaskbar();

    if (isVert) {
        // 垂直任务栏：按钮垂直堆叠
        const int btnSize = std::min(static_cast<int>(w * 0.7), 28);
        const int spacing = 2;
        const int totalBtnHeight = btnSize * 3 + spacing * 2;
        const int btnX = (w - btnSize) / 2;
        const int startY = (h - totalBtnHeight) / 2;

        // 检查 x 是否在按钮区域内
        if (x < btnX || x > btnX + btnSize) return HoverButton::None;

        // Next 按钮 (最底部)
        int nextY = startY + (btnSize + spacing) * 2;
        if (y >= nextY && y <= nextY + btnSize) return HoverButton::Next;

        // PlayPause 按钮 (中间)
        int ppY = startY + btnSize + spacing;
        if (y >= ppY && y <= ppY + btnSize) return HoverButton::PlayPause;

        // Prev 按钮 (最顶部)
        if (y >= startY && y <= startY + btnSize) return HoverButton::Prev;

        return HoverButton::None;
    }

    // 水平任务栏：按钮水平排列（原有逻辑）
    const int btnSize = static_cast<int>(h * 0.7);
    const int btnY = (h - btnSize) / 2;
    const int spacing = 2;
    const int totalBtnWidth = btnSize * 3 + spacing * 2;
    const int startX = (w - totalBtnWidth) / 2;

    // 检查 y 是否在按钮区域内
    if (y < btnY || y > btnY + btnSize) return HoverButton::None;

    // Next 按钮 (最右侧)
    int nextX = startX + (btnSize + spacing) * 2;
    if (x >= nextX && x <= nextX + btnSize) return HoverButton::Next;

    // PlayPause 按钮 (中间)
    int ppX = startX + btnSize + spacing;
    if (x >= ppX && x <= ppX + btnSize) return HoverButton::PlayPause;

    // Prev 按钮 (最左侧)
    if (x >= startX && x <= startX + btnSize) return HoverButton::Prev;

    return HoverButton::None;
}

// ═════════════════════════════════════════
// 拖动松开后的空闲位置检测与自动吸附
// ═════════════════════════════════════════

void TaskbarWindow::SnapToEmptySpace() {
    if (!hwnd_ || !hTaskbar_) return;

    // 全屏隐藏期间跳过吸附：避免 SetWindowPos 重新显示已隐藏的窗口
    if (fullscreenHidden_) return;

    RECT winRect{};
    ::GetWindowRect(hwnd_, &winRect);
    const int winW = winRect.right - winRect.left;
    const int winH = winRect.bottom - winRect.top;
    if (winW <= 0 || winH <= 0) return;

    RECT tbRect{};
    ::GetWindowRect(hTaskbar_, &tbRect);

    // ── 采样点检测：沿歌词窗口主轴方向取多个点，用 WindowFromPoint 检测障碍物 ──
    // Win11 任务栏中单个图标（Win按钮、运行中的应用等）不是独立子窗口，
    // 而是绘制在容器窗口内部，子窗口枚举无法精确获取。采样点可以穿透容器
    // 检测到任意有实际内容的区域。
    //
    // 关键：歌词窗口是 WS_EX_TOPMOST 覆盖在任务栏上方，直接调用 WindowFromPoint
    // 会返回自身。因此需要先将窗口临时移到屏幕外，采样完后再恢复。

    const int myStart = (info_.position == TaskbarPosition::LEFT || info_.position == TaskbarPosition::RIGHT)
                             ? winRect.top : winRect.left;
    const int myEnd = myStart + ((info_.position == TaskbarPosition::LEFT || info_.position == TaskbarPosition::RIGHT)
                                      ? winH : winW);

    // 采样步长：每 N 像素采一个点（兼顾精度和性能）
    constexpr int kSampleStep = 8;

    // 在移动窗口前预先计算中心坐标（基于原位置，用于采样点定位）
    const int centerX = (winRect.left + winRect.right) / 2;
    const int centerY = (winRect.top + winRect.bottom) / 2;

    // 临时将窗口移到屏幕外（避免遮挡采样）
    ::SetWindowPos(hwnd_, nullptr,
                   -winW * 2, -winH * 2, winW, winH,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    // 记录被占用的采样位置（用于合并成连续区间）
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

        if (hHit && hHit != hwnd_ && hHit != hTaskbar_) {
            // 排除歌词窗口自身和任务栏根窗口后，
            // 任何其他窗口都视为障碍物（图标、按钮、预览等）
            isObstacle = true;
        } else {
            isObstacle = false;
        }

        samples.push_back({pos, isObstacle});
    }

    // 立即恢复窗口原位（用户感知不到移动，因为发生在同一帧内鼠标松开时）
    ::SetWindowPos(hwnd_, nullptr,
                   winRect.left, winRect.top, winW, winH,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    if (samples.empty()) return;

    // 判断整体是否有重叠：任一采样点命中障碍物即认为需要弹开
    bool hasObstacle = false;
    for (const auto& s : samples) {
        if (s.occupied) { hasObstacle = true; break; }
    }
    if (!hasObstacle) return;  // 全部采样点都空闲，保持原位

    // ═════ 有障碍物 → 将采样结果合并为占用区间，寻找最近空闲间隙 ═════

    struct OccupiedRange { int start; int end; };
    std::vector<OccupiedRange> occupied;

    // 从连续的 occupied 采样点生成区间
    int rangeStart = -1;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].occupied && rangeStart < 0) {
            rangeStart = samples[i].pos;
        }
        if (!samples[i].occupied && rangeStart >= 0) {
            // 区间结束，扩展到下一个采样点的位置（覆盖间隙）
            int rangeEnd = (i > 0) ? samples[i - 1].pos + kSampleStep : rangeStart + kSampleStep;
            occupied.push_back({rangeStart, rangeEnd});
            rangeStart = -1;
        }
    }
    // 处理末尾连续的 occupied
    if (rangeStart >= 0) {
        occupied.push_back({rangeStart, samples.back().pos + kSampleStep});
    }

    if (occupied.empty()) return;

    // 按 start 排序
    std::sort(occupied.begin(), occupied.end(),
              [](const OccupiedRange& a, const OccupiedRange& b) {
                  return a.start < b.start;
              });

    // 合并重叠/相邻区间
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

    // 遍历所有间隙，找能容纳且离当前位置最近的
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

    // 间隙：任务栏起始 → 第一个障碍物
    if (!merged.empty()) {
        tryGap(tbStart, merged.front().start);
    }

    // 间隙：相邻障碍物之间 + 最后一个 → 任务栏末尾
    for (size_t i = 0; i < merged.size(); ++i) {
        int gapStart = merged[i].end;
        int gapEnd = (i + 1 < merged.size()) ? merged[i + 1].start : tbEnd;
        tryGap(gapStart, gapEnd);
    }

    if (bestPos < 0) return;  // 找不到足够大的空位

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

    // 更新拖动偏移量（使位置被持久化保存到配置）
    int autoX = dragStartWinPos_.x - dragOffsetX_;
    int autoY = dragStartWinPos_.y - dragOffsetY_;
    dragOffsetX_ = newX - autoX;
    dragOffsetY_ = newY - autoY;

    ::SetWindowPos(hwnd_, nullptr, newX, newY, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ═════════════════════════════════════════
// UI Automation: 子窗口几何信息获取
// ═════════════════════════════════════════

void TaskbarWindow::InitUIAutomation() {
    // COM 应在 WinMain 中已初始化（ole32 已链接），此处仅创建 IUIAutomation 实例
    HRESULT hr = CoCreateInstance(
        __uuidof(CUIAutomation), nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation),
        reinterpret_cast<void**>(&uia_));
    if (FAILED(hr)) {
        ::OutputDebugStringW(L"[TaskbarLyrics] InitUIAutomation FAILED\n");
        uia_ = nullptr;
    }
}

void TaskbarWindow::CleanupUIAutomation() {
    if (uia_) {
        uia_->Release();
        uia_ = nullptr;
    }
}

bool TaskbarWindow::GetChildRectsByUIA(RECT& taskListRect, bool& foundTaskList,
                                        RECT& trayRect,    bool& foundTray,
                                        RECT& rebarRect,   bool& foundRebar,
                                        int   tbWidth) {
    foundTaskList = false;
    foundTray = false;
    foundRebar = false;

    if (!uia_ || !hTaskbar_) return false;

    // 获取 Shell_TrayWnd 对应的 UIA 元素
    IUIAutomationElement* taskbarElement = nullptr;
    HRESULT hr = uia_->ElementFromHandle(hTaskbar_, &taskbarElement);
    if (FAILED(hr) || !taskbarElement) return false;

    // 获取所有直接子元素（TreeScope_Children）
    IUIAutomationCondition* trueCondition = nullptr;
    uia_->CreateTrueCondition(&trueCondition);
    if (!trueCondition) {
        taskbarElement->Release();
        return false;
    }

    IUIAutomationElementArray* children = nullptr;
    hr = taskbarElement->FindAll(TreeScope_Children, trueCondition, &children);
    trueCondition->Release();
    taskbarElement->Release();

    if (FAILED(hr) || !children) return false;

    int count = 0;
    children->get_Length(&count);

    for (int i = 0; i < count; i++) {
        IUIAutomationElement* child = nullptr;
        children->GetElement(i, &child);
        if (!child) continue;

        // 获取类名
        BSTR className = nullptr;
        child->get_CurrentClassName(&className);

        // 获取边界矩形（屏幕坐标，等同于 GetWindowRect）
        RECT rc{};
        child->get_CurrentBoundingRectangle(&rc);

        if (className) {
            if (wcscmp(className, L"MSTaskListWClass") == 0) {
                taskListRect = rc;
                foundTaskList = true;
            } else if (wcscmp(className,
                       L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
                // Win11: 占满全屏的 Bridge 忽略（通常是 Start Menu 背景层）
                int cw = rc.right - rc.left;
                if (cw < tbWidth - 10) {
                    taskListRect = rc;
                    foundTaskList = true;
                }
            } else if (wcscmp(className, L"TrayNotifyWnd") == 0) {
                trayRect = rc;
                foundTray = true;
            } else if (wcscmp(className, L"ReBarWindow32") == 0) {
                rebarRect = rc;
                foundRebar = true;
            }
            ::SysFreeString(className);
        }
        child->Release();
    }

    children->Release();
    return true;
}

LRESULT CALLBACK TaskbarWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TaskbarWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<TaskbarWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TaskbarWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_MOUSEMOVE: {
        // 完全锁定：跳过所有鼠标交互
        if (self->fullyLocked_) return 0;

        bool changed = false;
        if (!self->isHovering_) {
            self->isHovering_ = true;
            changed = true;
        }

        // 拖动中：根据任务栏方位约束移动方向
        if (self->isDragging_) {
            POINT cur{};
            ::GetCursorPos(&cur);
            int dx = cur.x - self->dragStartPos_.x;
            int dy = cur.y - self->dragStartPos_.y;
            RECT wr{};
            ::GetWindowRect(hwnd, &wr);

            int newWx = wr.left;
            int newWy = wr.top;

            // 根据任务栏方位决定允许的拖动方向
            switch (self->info_.position) {
            case TaskbarPosition::BOTTOM:
            case TaskbarPosition::TOP:
                // 水平任务栏：只允许水平拖动（X轴），Y锁定在任务栏内
                newWx = self->dragStartWinPos_.x + dx;
                break;
            case TaskbarPosition::LEFT:
            case TaskbarPosition::RIGHT:
                // 垂直任务栏：只允许垂直拖动（Y轴），X锁定在任务栏内
                newWy = self->dragStartWinPos_.y + dy;
                break;
            }

            // 约束在任务栏矩形范围内
            RECT tbRect{};
            if (self->hTaskbar_) ::GetWindowRect(self->hTaskbar_, &tbRect);
            const int winW = wr.right - wr.left;
            const int winH = wr.bottom - wr.top;

            // X 边界
            if (newWx < tbRect.left) newWx = tbRect.left;
            if (newWx + winW > tbRect.right) newWx = tbRect.right - winW;

            // Y 边界
            if (newWy < tbRect.top) newWy = tbRect.top;
            if (newWy + winH > tbRect.bottom) newWy = tbRect.bottom - winH;

            // 计算相对于"自动定位位置"的偏移量
            // 自动位置 = 拖动开始时的窗口位置 - 当时的偏移量
            // 新偏移量 = 当前实际位置 - 自动位置
            int autoX = self->dragStartWinPos_.x - self->dragOffsetX_;
            int autoY = self->dragStartWinPos_.y - self->dragOffsetY_;
            self->dragOffsetX_ = newWx - autoX;
            self->dragOffsetY_ = newWy - autoY;

            self->dragStartPos_ = cur;
            self->dragStartWinPos_.x = newWx;
            self->dragStartWinPos_.y = newWy;

            ::SetWindowPos(hwnd, nullptr, newWx, newWy, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (!self->trackingMouse_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = HOVER_DEFAULT;
            ::TrackMouseEvent(&tme);
            self->trackingMouse_ = true;
        }
        if (changed && self->onHoverChanged_) {
            self->onHoverChanged_();
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        self->isHovering_ = false;
        self->trackingMouse_ = false;
        if (self->onHoverChanged_) {
            self->onHoverChanged_();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        // 完全锁定：跳过所有鼠标交互（含按钮）
        if (self->fullyLocked_) return 0;

        HoverButton btn = self->HitTestButton(x, y);
        if (btn != HoverButton::None && self->onButtonClicked_) {
            // 按钮点击：即使位置锁定也可操作
            self->onButtonClicked_(btn);
        } else if (!self->positionLocked_) {
            // 非按钮区域 + 未锁定位置：开始拖动
            self->isDragging_ = true;
            ::GetCursorPos(&self->dragStartPos_);
            RECT wr{};
            ::GetWindowRect(hwnd, &wr);
            self->dragStartWinPos_.x = wr.left;
            self->dragStartWinPos_.y = wr.top;
            ::SetCapture(hwnd);
            if (self->onHoverChanged_) self->onHoverChanged_();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (self->isDragging_) {
            self->isDragging_ = false;
            ::ReleaseCapture();
            // 检测松开位置是否与其他任务栏子窗口重叠，自动弹到最近空闲位置
            self->SnapToEmptySpace();
            // 通知重绘以移除拖动边框
            if (self->onHoverChanged_) {
                self->onHoverChanged_();
            }
        }
        return 0;
    }
    case WM_DPICHANGED: {
        // DPI 变化：旧偏移量基于旧 DPI 的像素值，直接复用会导致窗口飞出屏幕
        self->dragOffsetX_ = 0;
        self->dragOffsetY_ = 0;
        self->PositionLyricsInTaskbar();
        return 0;
    }
    case WM_SETTINGCHANGE: {
        // SPI_SETWORKAREA: 分辨率/缩放/多显示器/自动隐藏任务栏/Start Menu 弹出时发送
        // 先交给 DefWindowProc 让系统处理工作区变化。
        // 使用 PostMessage 延迟定位：WinEvent out-of-context 钩子事件（如
        // MENUPOPUPSTART）通过消息队列分发，PostMessage 将重定位放入队列末尾，
        // 确保 MENUPOPUPSTART 优先设置 s_shellInteractionLocked_ 锁后再执行定位，
        // 消除 Start Menu 弹出时直接调用 PositionLyricsInTaskbar() 的竞态窗口。
        if (wParam == SPI_SETWORKAREA || wParam == SPI_SETNONCLIENTMETRICS) {
            ::DefWindowProcW(hwnd, msg, wParam, lParam);
            if (self && hwnd) {
                ::PostMessageW(hwnd, WM_DELAYED_REPOSITION, 0, 0);
            }
        }
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        // 分辨率变化：重置偏移并重新定位
        self->dragOffsetX_ = 0;
        self->dragOffsetY_ = 0;
        self->PositionLyricsInTaskbar();
        return 0;
    }
    case WM_DESTROY: {
        ::PostQuitMessage(0);
        return 0;
    }
    case WM_TIMER:
        if (wParam == 2 && self) {
            // SetWinEventHook → 16ms 延迟 → 此时 DWM 已稳定 → 正确定位
            ::KillTimer(hwnd, 2);
            self->PositionLyricsInTaskbar();
        } else if (wParam == 3) {
            // Start Menu 关闭 300ms 后解锁，Explorer Rect 已恢复
            ::KillTimer(hwnd, 3);
            ::OutputDebugStringW(L"[TaskbarLyrics] Timer3: unlocking s_shellInteractionLocked_\n");
            TaskbarWindow::s_shellInteractionLocked_ = false;
            if (self) self->PositionLyricsInTaskbar();
        } else if (wParam == 4) {
            // Win11 Start Menu 关闭 300ms 后解锁（ForegroundHook 触发）
            ::KillTimer(hwnd, 4);
            ::OutputDebugStringW(L"[TaskbarLyrics] Timer4: unlock (ForegroundHook)\n");
            TaskbarWindow::s_shellInteractionLocked_ = false;
            if (self) self->PositionLyricsInTaskbar();
        }
        return 0;
    case TaskbarWindow::WM_DELAYED_REPOSITION:
        // SetWinEventHook 通知：任务栏位置已变更
        // 延迟 16ms（一帧）等待 DWM 稳定后定位
        if (self) {
            ::KillTimer(hwnd, 2);
            ::SetTimer(hwnd, 2, 16, nullptr);
        }
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace moekoe
