// SPDX-License-Identifier: GPL-2.0
// taskbar_window.cpp - 任务栏歌词窗口实现
// Win11 兼容方案: 独立浮动窗口覆盖在任务栏上方
// (类似 TrafficMonitor / TranslucentTB 的实现方式)
#include "taskbar_window.h"
#include "constants.h"

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
RECT TaskbarWindow::s_frozenTaskbarRect_{};

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
        // 锁定定位 + 冻结当前任务栏几何快照
        // Explorer 在 Start Menu 激活期间会临时改动任务栏内部布局（托盘重排、
        // client width 微变），导致 GetWindowRect 返回值虽不变但子区域偏移了。
        // 快照冻结确保后续所有布局计算基于锁定前的稳定值。
        TaskbarWindow::s_shellInteractionLocked_ = true;
        HWND tb = ::FindWindowW(L"Shell_TrayWnd", nullptr);
        if (tb) {
            ::GetWindowRect(tb, &TaskbarWindow::s_frozenTaskbarRect_);
        }
    } else if (event == EVENT_SYSTEM_MENUPOPUPEND) {
        if (TaskbarWindow::s_lyricsWnd_) {
            ::SetTimer(TaskbarWindow::s_lyricsWnd_, 3, 300, nullptr);
        }
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

    // 4) 初始化信息并定位
    DetectTaskbarInfo();
    PositionLyricsInTaskbar();

    // 5) 监听任务栏位置变化（auto-hide 滑出、Win 键、DPI 变化等）
    InstallTaskbarHook(hwnd_);

    created_ = true;
    return true;
}

void TaskbarWindow::Destroy() {
    RemoveTaskbarHook();
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    created_ = false;
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

    // Start Menu 激活期间：Explorer 内部布局不可靠（托盘重排、client width 微变）
    // 使用锁定前捕获的冻结快照，完全隔离 Explorer 的临时变动
    const bool useFrozen = s_shellInteractionLocked_ && s_frozenTaskbarRect_.right != 0;

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
    // 冻结模式下跳过实时状态检测
    if (!useFrozen) {
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
    }

    RECT tbRect{};
    if (useFrozen) {
        tbRect = s_frozenTaskbarRect_;
    } else {
        ::GetWindowRect(hTaskbar_, &tbRect);
    }

    // ── 帧锁定：动画期间拒绝使用中间态矩形 ──
    // 两次采样间隔 2ms，若差异 > 5px 说明任务栏正在滑入/滑出动画中
    // 直接回退到上次稳定值，避免歌词跳动到动画中间位置
    // 注意：冻结模式下跳过（s_frozenTaskbarRect_ 已是稳定值）
    if (!useFrozen && taskbarAutoHide_) {
        RECT tbCheck{};
        ::Sleep(2);
        ::GetWindowRect(hTaskbar_, &tbCheck);
        const int delta = (info_.position == TaskbarPosition::LEFT ||
                           info_.position == TaskbarPosition::RIGHT)
                              ? abs((tbRect.right - tbRect.left) - (tbCheck.right - tbCheck.left))
                              : abs(tbRect.bottom - tbCheck.bottom);
        if (delta > 5) {
            // 动画中 → 使用上次稳定矩形
            if (stableTaskbarRect_.right != 0) {
                tbRect = stableTaskbarRect_;
            }
        } else {
            // 稳定 → 更新缓存
            stableTaskbarRect_ = tbRect;
        }
    } else {
        stableTaskbarRect_ = tbRect;
    }

    lastTaskbarRect_ = tbRect;
    const int tbWidth  = tbRect.right  - tbRect.left;
    const int tbHeight = tbRect.bottom - tbRect.top;

    // 枚举任务栏子窗口，找到空闲区域
    // 冻结模式下跳过：子窗口状态不可靠
    struct ChildInfo {
        HWND  hwnd;
        RECT  rect;   // 屏幕坐标
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

    // 歌词区尺寸（根据显示模式选择不同的高度）
    const bool isCardMode = (displayMode_ == "card");
    const int lyricH = ::MulDiv(
        isCardMode ? constants::CARD_HEIGHT_BASE_DP : constants::LYRIC_HEIGHT_BASE_DP,
        info_.dpi, 96);
    int w = 0, h = lyricH, x = 0, y = 0;

    // 找到关键子窗口的位置 (屏幕坐标)
    RECT taskListRect = {};
    bool foundTaskList = false;
    RECT trayRect = {};
    bool foundTray = false;
    RECT rebarRect = {};
    bool foundRebar = false;

    for (const auto& ci : children) {
        if (ci.className == L"MSTaskListWClass") {
            taskListRect = ci.rect;
            foundTaskList = true;
        }
        if (ci.className == L"Windows.UI.Composition.DesktopWindowContentBridge") {
            // Win11: 占满全屏则忽略
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

    switch (info_.position) {
    case TaskbarPosition::BOTTOM: {
        // 底部任务栏: 歌词定位在右侧（紧靠托盘左侧），不覆盖最小化窗口区域
        int rightEdge = tbRect.right;

        // 冻结模式：用缓存偏移；正常模式：实时检测托盘位置
        if (useFrozen) {
            rightEdge -= cachedRightEdgeOffset_;
        } else if (foundTray) {
            rightEdge = trayRect.left;
        }

        // ── 缓存托盘偏移，动画期间复用稳定值 ──
        const int offsetFromRight = tbRect.right - rightEdge;
        const bool isAnimating = !useFrozen && taskbarAutoHide_ && (
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
        swprintf_s(dbg, L"[TaskbarLyrics] Pos: pos=%d x=%d y=%d w=%d h=%d tbRect=(%d,%d,%d,%d)\n",
                   static_cast<int>(info_.position), x, y, w, h,
                   tbRect.left, tbRect.top, tbRect.right, tbRect.bottom);
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

    RECT tb{};
    ::GetWindowRect(hTaskbar_, &tb);
    if (!::EqualRect(&tb, &lastTaskbarRect_)) {
        PositionLyricsInTaskbar();
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
        // 先交给 DefWindowProc 让系统处理工作区变化，再立即重新定位覆盖系统调整
        if (wParam == SPI_SETWORKAREA || wParam == SPI_SETNONCLIENTMETRICS) {
            ::DefWindowProcW(hwnd, msg, wParam, lParam);
            if (self) self->PositionLyricsInTaskbar();
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
