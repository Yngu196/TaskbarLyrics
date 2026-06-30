// SPDX-License-Identifier: GPL-3.0
// taskbar_embedder.cpp - 窗口创建/销毁/显隐/SetWindowPos 实现
//
// 注意：此文件是 A1 拆分的骨架模块。窗口创建/销毁/全屏显隐已迁移至此，
// 但 WndProc、HitTestButton、拖动逻辑仍由 TaskbarWindow 直接接管。
// Create() 传入 this 指针作为 lpParam，由 TaskbarWindow::WndProc 在 WM_NCCREATE
// 中读取并替换为 TaskbarWindow*，因此 embedder 自身的 WndProc 不会被调用。
#include "taskbar_embedder.h"
#include "constants.h"

#include <windowsx.h>

namespace moekoe {

TaskbarEmbedder::TaskbarEmbedder() = default;

TaskbarEmbedder::~TaskbarEmbedder() {
    Destroy();
}

bool TaskbarEmbedder::Create(HINSTANCE hInstance, HWND hParent, const wchar_t* className, int /*dpi*/) {
    if (hwnd_) return true;

    // WS_EX_LAYERED → UpdateLayeredWindow 透明渲染
    // WS_EX_TOOLWINDOW → 不在 Alt+Tab 列表中出现
    // WS_EX_NOACTIVATE  → 不抢夺焦点
    const DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    const DWORD style   = WS_POPUP;

    hwnd_ = ::CreateWindowExW(exStyle, className, L"", style,
        0, 0, 0, 0, nullptr, nullptr, hInstance, this);
    if (!hwnd_) return false;

    // GWLP_HWNDPARENT → owned window 关系：窗口始终在 Shell_TrayWnd Z-order 组内，
    // 渲染在任务栏上方，无需 HWND_TOPMOST 竞争，不受 Win 键 Start Menu 影响
    if (hParent) {
        ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hParent));
    }

    hInstance_ = hInstance;
    return true;
}

void TaskbarEmbedder::Destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TaskbarEmbedder::Position(int x, int y, int w, int h, bool forceTopmost) {
    if (!hwnd_) return;
    HWND after = forceTopmost ? HWND_TOPMOST : nullptr;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    // 全屏恢复路径：窗口此前被 SWP_HIDEWINDOW 隐藏，此处需显式 SWP_SHOWWINDOW
    if (forceTopmost && !::IsWindowVisible(hwnd_)) flags |= SWP_SHOWWINDOW;
    ::SetWindowPos(hwnd_, after, x, y, w, h, flags);
}

void TaskbarEmbedder::SetFullscreenHidden(bool hidden) {
    if (fullscreenHidden_ == hidden) return;
    fullscreenHidden_ = hidden;
    if (!hwnd_) return;

    if (hidden) {
        // 双重保险：移到屏幕外 + SWP_HIDEWINDOW。
        // ShowWindow(SW_HIDE) 会发送 WM_SHOWWINDOW，owned window 的 owner 可能拦截；
        // SWP_HIDEWINDOW 直接操作 WS_VISIBLE 位，更可靠。
        ::SetWindowPos(hwnd_, nullptr,
                       -32000, -32000, 0, 0,
                       SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        // 恢复显示：HWND_TOPMOST 脱离 owned Z-order 限制（SWP_SHOWWINDOW 从 hidden 状态恢复时
        // owned window 层级可能受限），SWP_NOMOVE 保持现有位置（调用者负责随后定位到正确坐标）。
        // SWP_FRAMECHANGED 触发 WM_NCCALCSIZE 使窗口矩形重新生效。
        ::SetWindowPos(hwnd_, HWND_TOPMOST,
                       0, 0, 0, 0,
                       SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

HoverButton TaskbarEmbedder::HitTestButton(int, int, int, int, bool, bool) const {
    // 占位实现——实际按钮命中测试由 TaskbarWindow::HitTestButton 完成，
    // embedder 不拥有 displayMode / constants 等按钮几何所需上下文
    return static_cast<HoverButton>(0);
}

void TaskbarEmbedder::BeginDrag(int mouseScreenX, int mouseScreenY) {
    isDragging_ = true;
    dragStartPos_.x = mouseScreenX;
    dragStartPos_.y = mouseScreenY;
}

void TaskbarEmbedder::EndDrag() {
    isDragging_ = false;
}

// 占位 WndProc——CreateWindowExW 传入 this 作为 lpParam，TaskbarWindow::WndProc 在
// WM_NCCREATE 中读取 lpParam 并替换 GWLP_USERDATA 为 TaskbarWindow*，因此实际窗口过程
// 由 TaskbarWindow::WndProc 接管。此函数仅在未正确替换时作为兜底。
LRESULT CALLBACK TaskbarEmbedder::WndProc(HWND, UINT, WPARAM, LPARAM) {
    return ::DefWindowProc(nullptr, 0, 0, 0);
}

} // namespace moekoe
