// SPDX-License-Identifier: Apache-2.0
// taskbar_embedder.h - 歌词窗口句柄与全屏显隐状态
//
// 窗口创建、定位、拖动/悬停与 WndProc 均由 TaskbarWindow / ShellCompanion
// 直接管理；此处只保留二者实际使用的句柄访问和全屏隐藏状态。
#pragma once

#include <windows.h>

namespace moekoe {

class TaskbarEmbedder {
public:
    TaskbarEmbedder();
    ~TaskbarEmbedder();

    TaskbarEmbedder(const TaskbarEmbedder&) = delete;
    TaskbarEmbedder& operator=(const TaskbarEmbedder&) = delete;

    void Destroy();

    HWND GetHandle() const { return hwnd_; }
    void SetHandle(HWND hwnd) { hwnd_ = hwnd; }

    // 全屏隐藏：移到 (-32000,-32000) + SWP_HIDEWINDOW，双重保险防止闪现
    // 全屏恢复：SWP_SHOWWINDOW + HWND_TOPMOST 脱离 owned Z-order 限制
    void SetFullscreenHidden(bool hidden);
    bool IsFullscreenHidden() const { return fullscreenHidden_; }

private:
    HWND hwnd_{nullptr};
    bool fullscreenHidden_{false};
};

} // namespace moekoe
