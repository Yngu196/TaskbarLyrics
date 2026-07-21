// SPDX-License-Identifier: GPL-3.0
// taskbar_window.cpp - 任务栏歌词窗口实现
// Win11 兼容方案: 独立浮动窗口覆盖在任务栏上方
// (类似 TrafficMonitor / TranslucentTB 的实现方式)
//
// v0.5.x: Shell 层逻辑已迁移至 shell_companion.cpp
#include "taskbar_window.h"
#include "constants.h"
#include "tray_icon.h"

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

    // 6) 延迟首次定位：不在 Create 中调用 InternalPosition，
    //    由外部在 SetDragOffset + SetDisplayMode + SetPositionLocked 全部就绪后
    //    通过 Reposition() 统一触发，避免 dragOffset=(0,0) 的无效定位，
    //    以及 WinEvent 刚安装时 Explorer 瞬态事件导致冻结跳过。
    //    参见 main.cpp WinMain 中 "应用配置中的位置偏移" 之后的 Reposition() 调用。

    created_ = true;
    return true;
}

void TaskbarWindow::Destroy() {
    companion_.Shutdown();
    if (hwnd_) {
        // 先解除与任务栏的父子关系，避免 DestroyWindow 影响任务栏
        ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, 0);
        // 先隐藏窗口，避免销毁过程中闪烁
        ::ShowWindow(hwnd_, SW_HIDE);
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
        hwnd_, displayMode_, dragOffsetX_, dragOffsetY_, lastPosRect_, dynamicCardWidthDip_);
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

void TaskbarWindow::ShowLyricsContextMenu() {
    if (!hwnd_) return;

    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;

    ::AppendMenuW(hMenu, MF_STRING, ID_MENU_OPEN_MOEKOE, L"打开 MoeKoeMusic");
    ::AppendMenuW(hMenu, MF_STRING, ID_MENU_SETTINGS, L"设置...");

    // 导出日志
    ::AppendMenuW(hMenu, MF_STRING, ID_MENU_EXPORT_LOG, L"导出日志...");

    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(hMenu, MF_STRING, ID_MENU_RECONNECT, L"重新连接");

    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 翻译模式子菜单（根据显示模式显示不同选项）
    HMENU hTransMenu = ::CreatePopupMenu();
    if (displayMode_ == "card") {
        // 卡片模式：三态翻译选项
        ::AppendMenuW(hTransMenu, MF_STRING, ID_MENU_TRANSLATION_MODE,     L"仅原文");
        ::AppendMenuW(hTransMenu, MF_STRING, ID_MENU_TRANSLATION_MODE + 1, L"仅翻译");
        ::AppendMenuW(hTransMenu, MF_STRING, ID_MENU_TRANSLATION_MODE + 2, L"原文+翻译");
    } else {
        // 卡拉OK模式：仅"原/译"切换
        ::AppendMenuW(hTransMenu, MF_STRING, ID_MENU_TRANSLATION_MODE,     L"原（显示原文）");
        ::AppendMenuW(hTransMenu, MF_STRING, ID_MENU_TRANSLATION_MODE + 1, L"译（显示译文）");
    }
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTransMenu), L"翻译模式");

    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    UINT lockPosFlags = MF_STRING | (positionLocked_ ? MF_CHECKED : MF_UNCHECKED);
    ::AppendMenuW(hMenu, lockPosFlags, ID_MENU_LOCK_POS, L"锁定位置");

    UINT lockFullFlags = MF_STRING | (fullyLocked_ ? MF_CHECKED : MF_UNCHECKED);
    ::AppendMenuW(hMenu, lockFullFlags, ID_MENU_LOCK_FULL, L"完全锁定");

    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(hMenu, MF_STRING, ID_MENU_EXIT, L"退出");

    // 计算菜单位置：水平居中对齐到歌词窗口，垂直方向紧贴任务栏外侧
    // - 水平：以歌词窗口中心为锚点，用 TPM_CENTERALIGN 让菜单水平居中
    // - 垂直：菜单边缘紧贴任务栏外边缘（底任务栏→菜单底贴任务栏顶；顶任务栏→菜单顶贴任务栏底）
    POINT pt{};
    const RECT tbRect = companion_.GetTaskbarRect();
    const TaskbarPosition tbPos = companion_.GetTaskbarInfo().position;

    // 取歌词窗口矩形作为水平锚点（fallback 到任务栏矩形）
    RECT rcLyrics{};
    if (!::GetWindowRect(hwnd_, &rcLyrics)) {
        rcLyrics = tbRect;
    }

    switch (tbPos) {
    case TaskbarPosition::BOTTOM:
        pt.x = (rcLyrics.left + rcLyrics.right) / 2;  // 歌词窗口水平中心
        pt.y = tbRect.top; // 菜单底部紧贴任务栏顶部
        break;
    case TaskbarPosition::TOP:
        pt.x = (rcLyrics.left + rcLyrics.right) / 2;
        pt.y = tbRect.bottom; // 菜单顶部紧贴任务栏底部
        break;
    case TaskbarPosition::LEFT:
        pt.x = tbRect.right; // 菜单左边紧贴任务栏右边
        pt.y = (rcLyrics.top + rcLyrics.bottom) / 2;  // 歌词窗口垂直中心
        break;
    case TaskbarPosition::RIGHT:
        pt.x = tbRect.left; // 菜单右边紧贴任务栏左边
        pt.y = (rcLyrics.top + rcLyrics.bottom) / 2;
        break;
    default:
        ::GetCursorPos(&pt);
        break;
    }

    // 使用消息窗口（而非歌词窗口）作为菜单所有者
    // 原因：歌词窗口通过 GWLP_HWNDPARENT 绑定到 Shell_TrayWnd，
    // 若用歌词窗口做 TrackPopupMenuEx 的所有者，其内部模态消息循环
    // 可能与任务栏的消息处理产生死锁，导致程序卡死
    HWND hMenuOwner = hMsgWnd_ ? hMsgWnd_ : hwnd_;
    ::SetForegroundWindow(hMenuOwner);

    UINT trackFlags = TPM_RIGHTBUTTON | TPM_RETURNCMD;
    // 根据任务栏方向选择菜单展开方向
    switch (tbPos) {
    case TaskbarPosition::BOTTOM:
        trackFlags |= TPM_CENTERALIGN | TPM_BOTTOMALIGN | TPM_VERNEGANIMATION;
        break;
    case TaskbarPosition::TOP:
        trackFlags |= TPM_CENTERALIGN | TPM_TOPALIGN | TPM_VERNEGANIMATION;
        break;
    case TaskbarPosition::LEFT:
        trackFlags |= TPM_CENTERALIGN | TPM_LEFTALIGN | TPM_HORNEGANIMATION;
        break;
    case TaskbarPosition::RIGHT:
        trackFlags |= TPM_CENTERALIGN | TPM_RIGHTALIGN | TPM_HORNEGANIMATION;
        break;
    default:
        trackFlags |= TPM_VERNEGANIMATION;
        break;
    }

    const auto cmd = ::TrackPopupMenuEx(
        hMenu, trackFlags, pt.x, pt.y, hMenuOwner, nullptr);

    ::DestroyMenu(hMenu);

    if (cmd != 0 && onContextMenuCmd_) {
        onContextMenuCmd_(static_cast<UINT>(cmd));
    }
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
            if (self->onDragEnd_) {
                self->onDragEnd_();
            }
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        self->ShowLyricsContextMenu();
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
        // 仅当任务栏方位真正变化时才重置偏移，避免睡眠唤醒误清零
        const TaskbarPosition prevPos = self->companion_.GetTaskbarInfo().position;
        self->companion_.Geometry().Detect(self->companion_.GetTaskbarHandle());
        const TaskbarPosition newPos = self->companion_.GetTaskbarInfo().position;

        if (prevPos != newPos && prevPos != TaskbarPosition::UNKNOWN) {
            self->dragOffsetX_ = 0;
            self->dragOffsetY_ = 0;
            ::OutputDebugStringW(L"[TaskbarLyrics] WM_DISPLAYCHANGE: orientation changed, resetting offsets\n");
        } else {
            ::OutputDebugStringW(L"[TaskbarLyrics] WM_DISPLAYCHANGE: same orientation, preserving offsets\n");
        }
        self->InternalPosition();
        return 0;
    }
    case WM_POWERBROADCAST: {
        if (wParam == PBT_APMRESUMEAUTOMATIC) {
            ::OutputDebugStringW(L"[TaskbarLyrics] PBT_APMRESUMEAUTOMATIC: refreshing taskbar\n");
            HWND hNewTaskbar = ShellCompanion::FindTaskbarHandle();
            if (hNewTaskbar && hNewTaskbar != self->companion_.GetTaskbarHandle()) {
                ::SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hNewTaskbar));
                self->companion_.RefreshTaskbarHandle(hNewTaskbar);
            }
            self->InvalidatePositionCache();
            self->InternalPosition();
        }
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
