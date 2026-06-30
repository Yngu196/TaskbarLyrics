// SPDX-License-Identifier: GPL-3.0
// taskbar_window.cpp - 任务栏歌词窗口实现
// Win11 兼容方案: 独立浮动窗口覆盖在任务栏上方
// (类似 TrafficMonitor / TranslucentTB 的实现方式)
//
// v0.5.x: Shell 层逻辑已迁移至 shell_companion.cpp
#include "taskbar_window.h"
#include "constants.h"

#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

namespace moekoe {

// ═════════════════════════════════════════
// 静态：查找任务栏句柄（委托 ShellCompanion）
// ═════════════════════════════════════════

HWND TaskbarWindow::FindTaskbarHandle() {
    return ShellCompanion::FindTaskbarHandle();
}

// ═════════════════════════════════════════
// 构造函数 / 析构函数
// ═════════════════════════════════════════

TaskbarWindow::TaskbarWindow() = default;

TaskbarWindow::~TaskbarWindow() {
    Destroy();
}

// ═════════════════════════════════════════
// 创建 / 销毁
// ═════════════════════════════════════════

bool TaskbarWindow::Create(HINSTANCE hInstance, HWND hParent) {
    if (created_) return true;
    hInstance_ = hInstance;

    // 1) 解析任务栏
    HWND hTaskbar = hParent ? hParent : ShellCompanion::FindTaskbarHandle();
    if (!hTaskbar) return false;

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

    // 设为 Shell_TrayWnd 的 owned window
    ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hTaskbar));

    // 4) 初始化 ShellCompanion（UIA、WinEvent 钩子）
    companion_.Initialize(hTaskbar, hwnd_);

    // 5) 首次定位：记录 lastPosition_ 防止误判方位变化
    lastPosition_ = companion_.GetTaskbarInfo().position;

    // 6) 定位歌词窗口
    InternalPosition();

    created_ = true;
    return true;
}

void TaskbarWindow::Destroy() {
    companion_.Shutdown();
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    created_ = false;
}

// ═════════════════════════════════════════
// 定位
// ═════════════════════════════════════════

void TaskbarWindow::InternalPosition() {
    if (!hwnd_) return;

    // 检测方位变化 → 重置拖动偏移
    const TaskbarPosition curPos = companion_.GetTaskbarInfo().position;
    if (lastPosition_ != TaskbarPosition::UNKNOWN && lastPosition_ != curPos) {
        dragOffsetX_ = 0;
        dragOffsetY_ = 0;
        ::OutputDebugStringW(L"[TaskbarLyrics] 任务栏方位变化，重置拖动偏移\n");
    }
    lastPosition_ = curPos;

    companion_.PositionLyricsInTaskbar(
        hwnd_, displayMode_, dragOffsetX_, dragOffsetY_, lastPosRect_);
}

void TaskbarWindow::Reposition() {
    InternalPosition();
}

// ═════════════════════════════════════════
// 拖动后吸附
// ═════════════════════════════════════════

void TaskbarWindow::SnapToEmptySpace() {
    if (!hwnd_) return;

    // 委托 ShellCompanion 执行采样检测和 SetWindowPos
    companion_.SnapToEmptySpace(hwnd_);

    // 更新拖动偏移量（SnapToEmptySpace 不修改偏移量，
    // 偏移量更新在 WndProc WM_LBUTTONUP 中完成）
}

// ═════════════════════════════════════════
// 按钮命中测试
// ═════════════════════════════════════════

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

        if (x < btnX || x > btnX + btnSize) return HoverButton::None;

        int nextY = startY + (btnSize + spacing) * 2;
        if (y >= nextY && y <= nextY + btnSize) return HoverButton::Next;

        int ppY = startY + btnSize + spacing;
        if (y >= ppY && y <= ppY + btnSize) return HoverButton::PlayPause;

        if (y >= startY && y <= startY + btnSize) return HoverButton::Prev;

        return HoverButton::None;
    }

    // 水平任务栏：按钮水平排列
    const int btnSize = static_cast<int>(h * 0.7);
    const int btnY = (h - btnSize) / 2;
    const int spacing = 2;
    const int totalBtnWidth = btnSize * 3 + spacing * 2;
    const int startX = (w - totalBtnWidth) / 2;

    if (y < btnY || y > btnY + btnSize) return HoverButton::None;

    int nextX = startX + (btnSize + spacing) * 2;
    if (x >= nextX && x <= nextX + btnSize) return HoverButton::Next;

    int ppX = startX + btnSize + spacing;
    if (x >= ppX && x <= ppX + btnSize) return HoverButton::PlayPause;

    if (x >= startX && x <= startX + btnSize) return HoverButton::Prev;

    return HoverButton::None;
}

// ═════════════════════════════════════════
// 窗口过程
// ═════════════════════════════════════════

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
        if (self->fullyLocked_) return 0;

        bool changed = false;
        if (!self->isHovering_) {
            self->isHovering_ = true;
            changed = true;
        }

        if (self->isDragging_) {
            POINT cur{};
            ::GetCursorPos(&cur);
            int dx = cur.x - self->dragStartPos_.x;
            int dy = cur.y - self->dragStartPos_.y;
            RECT wr{};
            ::GetWindowRect(hwnd, &wr);

            int newWx = wr.left;
            int newWy = wr.top;

            switch (self->companion_.GetTaskbarInfo().position) {
            case TaskbarPosition::BOTTOM:
            case TaskbarPosition::TOP:
                newWx = self->dragStartWinPos_.x + dx;
                break;
            case TaskbarPosition::LEFT:
            case TaskbarPosition::RIGHT:
                newWy = self->dragStartWinPos_.y + dy;
                break;
            }

            RECT tbRect = self->companion_.GetTaskbarRect();
            const int winW = wr.right - wr.left;
            const int winH = wr.bottom - wr.top;

            if (newWx < tbRect.left) newWx = tbRect.left;
            if (newWx + winW > tbRect.right) newWx = tbRect.right - winW;
            if (newWy < tbRect.top) newWy = tbRect.top;
            if (newWy + winH > tbRect.bottom) newWy = tbRect.bottom - winH;

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

        if (self->fullyLocked_) return 0;

        HoverButton btn = self->HitTestButton(x, y);
        if (btn != HoverButton::None && self->onButtonClicked_) {
            self->onButtonClicked_(btn);
        } else if (!self->positionLocked_) {
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
            self->SnapToEmptySpace();
            if (self->onHoverChanged_) {
                self->onHoverChanged_();
            }
        }
        return 0;
    }
    case WM_DPICHANGED: {
        self->dragOffsetX_ = 0;
        self->dragOffsetY_ = 0;
        self->InternalPosition();
        return 0;
    }
    case WM_SETTINGCHANGE: {
        if (wParam == SPI_SETWORKAREA || wParam == SPI_SETNONCLIENTMETRICS) {
            ::DefWindowProcW(hwnd, msg, wParam, lParam);
            if (self && hwnd) {
                self->companion_.RequestReposition(hwnd);
            }
        }
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        self->dragOffsetX_ = 0;
        self->dragOffsetY_ = 0;
        self->InternalPosition();
        return 0;
    }
    case WM_DESTROY: {
        // 主消息循环已在 Cleanup 阶段退出，PostQuitMessage 不再需要
        return 0;
    }
    case WM_TIMER:
        if (wParam == 2 && self) {
            // SetWinEventHook → 16ms 延迟 → 此时 DWM 已稳定 → 正确定位
            ::KillTimer(hwnd, 2);
            self->InternalPosition();
        } else if (wParam == 3) {
            // Start Menu 关闭 300ms 后解锁，Explorer Rect 已恢复
            ::KillTimer(hwnd, 3);
            self->companion_.UnlockShellInteraction(3);
            if (self) self->InternalPosition();
        } else if (wParam == 4) {
            // Win11 Start Menu 关闭 300ms 后解锁（ForegroundHook 触发）
            ::KillTimer(hwnd, 4);
            self->companion_.UnlockShellInteraction(4);
            if (self) self->InternalPosition();
        }
        return 0;
    case TaskbarWindow::WM_DELAYED_REPOSITION:
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
