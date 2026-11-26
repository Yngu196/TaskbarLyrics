// SPDX-License-Identifier: Apache-2.0
// taskbar_embedder.cpp - 歌词窗口句柄与全屏显隐状态实现
#include "taskbar/taskbar_embedder.h"

namespace moekoe {

TaskbarEmbedder::TaskbarEmbedder() = default;

TaskbarEmbedder::~TaskbarEmbedder() {
    Destroy();
}

void TaskbarEmbedder::Destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
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

} // namespace moekoe
