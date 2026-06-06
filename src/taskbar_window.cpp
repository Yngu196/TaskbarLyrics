// SPDX-License-Identifier: GPL-2.0
// taskbar_window.cpp - 任务栏歌词窗口实现
// Win11 兼容方案: 独立浮动窗口覆盖在任务栏上方
// (类似 TrafficMonitor / TranslucentTB 的实现方式)
#include "taskbar_window.h"

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
}

void TaskbarWindow::PositionLyricsInTaskbar() {
    if (!hwnd_ || !hTaskbar_) return;
    DetectTaskbarInfo();

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
    const int lyricH = ::MulDiv(28, info_.dpi, 96);
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

    // 调试
    {
        FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
        if (f) {
            fprintf(f, "[TASKBAR] children=%zu tbRect=(%ld,%ld,%ld,%ld) pos=%d\n",
                    children.size(), tbRect.left, tbRect.top, tbRect.right, tbRect.bottom,
                    static_cast<int>(info_.position));
            for (size_t i = 0; i < children.size() && i < 10; i++) {
                fprintf(f, "[TASKBAR]   child[%zu] class=%ls rect=(%ld,%ld,%ld,%ld)\n",
                        i, children[i].className.c_str(),
                        children[i].rect.left, children[i].rect.top,
                        children[i].rect.right, children[i].rect.bottom);
            }
            fclose(f);
        }
    }

    switch (info_.position) {
    case TaskbarPosition::BOTTOM: {
        // 底部任务栏: 歌词浮动在任务栏上方
        int rightEdge = tbRect.left;
        if (foundRebar) rightEdge = std::max(rightEdge, (int)rebarRect.right);
        if (foundTaskList) rightEdge = std::max(rightEdge, (int)taskListRect.right);

        int leftEdge = tbRect.right;
        if (foundTray) leftEdge = std::min(leftEdge, (int)trayRect.left);

        int gapWidth = leftEdge - rightEdge;
        if (gapWidth > 50) {
            x = rightEdge;
            w = gapWidth;
        } else if (foundTaskList && (taskListRect.right - taskListRect.left) > 100) {
            x = taskListRect.left;
            w = taskListRect.right - taskListRect.left;
        } else {
            x = foundRebar ? rebarRect.right : tbRect.left;
            w = foundTray ? (trayRect.left - x) : (tbRect.right - x);
        }
        // 浮动在任务栏顶部
        y = tbRect.top;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::TOP: {
        int rightEdge = tbRect.left;
        if (foundRebar) rightEdge = std::max(rightEdge, (int)rebarRect.right);
        if (foundTaskList) rightEdge = std::max(rightEdge, (int)taskListRect.right);

        int leftEdge = tbRect.right;
        if (foundTray) leftEdge = std::min(leftEdge, (int)trayRect.left);

        int gapWidth = leftEdge - rightEdge;
        if (gapWidth > 50) {
            x = rightEdge;
            w = gapWidth;
        } else {
            x = foundRebar ? rebarRect.right : tbRect.left;
            w = foundTray ? (trayRect.left - x) : (tbRect.right - x);
        }
        y = tbRect.top;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::LEFT: {
        w = ::MulDiv(180, info_.dpi, 96);
        x = tbRect.right - w;
        y = tbRect.top;
        h = tbHeight;
        break;
    }
    case TaskbarPosition::RIGHT: {
        w = ::MulDiv(180, info_.dpi, 96);
        x = tbRect.left;
        y = tbRect.top;
        h = tbHeight;
        break;
    }
    default:
        x = tbRect.left; y = tbRect.top; w = tbWidth; h = tbHeight;
        break;
    }

    w = std::max(w, 50);
    h = std::max(h, lyricH);

    // 使用屏幕坐标定位独立窗口
    ::SetWindowPos(
        hwnd_, HWND_TOPMOST,
        x, y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    // 调试
    {
        FILE* f = fopen("D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log", "a");
        if (f) {
            fprintf(f, "[TASKBAR] final screen pos: x=%d y=%d w=%d h=%d foundTaskList=%d foundTray=%d foundRebar=%d\n",
                    x, y, w, h, foundTaskList, foundTray, foundRebar);
            fclose(f);
        }
    }
}

void TaskbarWindow::CheckResize() {
    if (!hTaskbar_) return;
    RECT tb{};
    ::GetWindowRect(hTaskbar_, &tb);
    if (!::EqualRect(&tb, &lastTaskbarRect_)) {
        PositionLyricsInTaskbar();
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

    // 按钮区域: 在窗口右侧，三个按钮水平排列
    // 每个按钮宽度约为高度的 1.2 倍，间距 2px
    const int btnSize = static_cast<int>(h * 0.7);
    const int btnY = (h - btnSize) / 2;
    const int spacing = 2;
    const int totalBtnWidth = btnSize * 3 + spacing * 2;
    const int startX = w - totalBtnWidth - 8; // 右侧留 8px 边距

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
        bool changed = false;
        if (!self->isHovering_) {
            self->isHovering_ = true;
            changed = true;
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
        HoverButton btn = self->HitTestButton(x, y);
        if (btn != HoverButton::None && self->onButtonClicked_) {
            self->onButtonClicked_(btn);
        }
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
