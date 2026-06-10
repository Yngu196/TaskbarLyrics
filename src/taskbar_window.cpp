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
    //   WS_EX_NOACTIVATE   : 不抢夺焦点
    //   WS_EX_TOOLWINDOW   : 不在任务栏显示
    //   WS_EX_LAYERED      : 支持透明 (配合 UpdateLayeredWindow)
    //   WS_EX_TOPMOST      : 始终在任务栏上方
    //   注意: 不使用 WS_EX_TRANSPARENT，以便接收鼠标消息
    const DWORD exStyle = WS_EX_NOACTIVATE |
                          WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST;
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

    // 4) 初始化信息并定位
    DetectTaskbarInfo();
    PositionLyricsInTaskbar();

    created_ = true;
    return true;
}

void TaskbarWindow::Destroy() {
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
    DetectTaskbarInfo();

    // ── APPBAR 自动隐藏处理 ──
    // 当任务栏开启自动隐藏时，检测任务栏当前是否可见（鼠标是否在任务栏区域）
    // 任务栏不可见时隐藏歌词窗口，避免遮挡桌面内容
    if (taskbarAutoHide_) {
        POINT pt{};
        ::GetCursorPos(&pt);
        // 判断鼠标是否在任务栏矩形范围内
        const bool cursorOnTaskbar = (::PtInRect(&info_.rect, pt) != FALSE);
        const bool shouldShow = cursorOnTaskbar;

        if (shouldShow && !taskbarVisible_) {
            taskbarVisible_ = true;
            // 继续下面的正常定位流程
        } else if (!shouldShow && taskbarVisible_) {
            taskbarVisible_ = false;
            ::ShowWindow(hwnd_, SW_HIDE);
            return;  // 隐藏后不执行定位
        } else if (!shouldShow && !taskbarVisible_) {
            ::ShowWindow(hwnd_, SW_HIDE);
            return;  // 保持隐藏
        }
        // shouldShow == true && taskbarVisible_ == true: 正常显示
    } else {
        // 非自动隐藏模式：确保窗口可见
        if (!taskbarVisible_) {
            taskbarVisible_ = true;
        }
    }

    RECT tbRect{};
    ::GetWindowRect(hTaskbar_, &tbRect);
    lastTaskbarRect_ = tbRect;
    const int tbWidth  = tbRect.right  - tbRect.left;
    const int tbHeight = tbRect.bottom - tbRect.top;

    // 枚举任务栏子窗口，找到空闲区域
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

    // 歌词区尺寸
    const int lyricH = ::MulDiv(constants::LYRIC_HEIGHT_BASE_DP, info_.dpi, 96);
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
        if (foundTray) rightEdge = trayRect.left;

        // 歌词窗口最大宽度
        const int maxLyricWidth = ::MulDiv(constants::MAX_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
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
    case TaskbarPosition::TOP: {
        int rightEdge = tbRect.right;
        if (foundTray) rightEdge = trayRect.left;

        const int maxLyricWidth = ::MulDiv(constants::MAX_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
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
        y = tbRect.top;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::RIGHT: {
        w = ::MulDiv(constants::VERTICAL_TASKBAR_LYRIC_WIDTH_BASE_DP, info_.dpi, 96);
        x = tbRect.left;
        y = tbRect.top;
        h = tbHeight;
        break;
    }
    default:
        x = tbRect.left; y = tbRect.top; w = tbWidth; h = tbHeight;
        break;
    }

    w = std::max(w, constants::MIN_WINDOW_WIDTH);
    h = std::max(h, lyricH);

    // 使用屏幕坐标定位独立窗口
    ::SetWindowPos(
        hwnd_, HWND_TOPMOST,
        x, y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

void TaskbarWindow::CheckResize() {
    if (!hTaskbar_) return;

    // 定期强制恢复 TOPMOST Z-order
    // 任务栏(Shell_TrayWnd)是系统级 TOPMOST 窗口，点击任务栏按钮/展开托盘时
    // Windows 会将其提升到普通 TOPMOST 窗口之上，仅靠 WM_ACTIVATE 恢复存在时序竞争
    ++topmostFrameCounter_;
    if (topmostFrameCounter_ >= kTopmostRestoreInterval) {
        topmostFrameCounter_ = 0;
        ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    RECT tb{};
    ::GetWindowRect(hTaskbar_, &tb);
    if (!::EqualRect(&tb, &lastTaskbarRect_)) {
        PositionLyricsInTaskbar();
    } else if (taskbarAutoHide_) {
        // 自动隐藏模式：即使任务栏矩形未变，鼠标进出任务栏区域也需要响应
        POINT pt{};
        ::GetCursorPos(&pt);
        const bool cursorOnTaskbar = (::PtInRect(&tb, pt) != FALSE);
        if (cursorOnTaskbar != taskbarVisible_) {
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

    // 按钮区域: 居中排列，三个按钮水平排列
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
            // 通知重绘以移除拖动边框
            if (self->onHoverChanged_) {
                self->onHoverChanged_();
            }
        }
        return 0;
    }
    case WM_ACTIVATE: {
        // 始终恢复 TOPMOST：点击任务栏/托盘时窗口会收到 WA_INACTIVE
        // 若不恢复，任务栏等系统窗口可能将歌词窗口覆盖到下方
        ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DPICHANGED: {
        self->PositionLyricsInTaskbar();
        return 0;
    }
    case WM_SETTINGCHANGE: {
        if (wParam == SPI_SETWORKAREA) {
            self->PositionLyricsInTaskbar();
        }
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        self->PositionLyricsInTaskbar();
        return 0;
    }
    case WM_DESTROY: {
        ::PostQuitMessage(0);
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace moekoe
