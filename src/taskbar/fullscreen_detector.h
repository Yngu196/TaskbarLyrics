// SPDX-License-Identifier: GPL-3.0
// fullscreen_detector.h - 全屏检测与防抖
//
// 职责: 检测前台窗口是否处于全屏状态 + 8 帧防抖
// 拆分自 main.cpp (A1)
#pragma once

namespace moekoe {

class FullscreenDetector {
public:
    FullscreenDetector() = default;

    // 每帧调用。debounce 命中阈值时返回 true。
    // outShouldHide: 输出参数，表示当前检测到的全屏状态
    bool Update(bool enableFullscreenHide, bool debugLog, bool& outShouldHide);

    // Shell 交互时强制重置防抖
    void ForceReset() { forceDebounceReset_ = true; }
    bool ConsumeForceDebounceReset();

    // Shell 菜单激活期间抑制全屏检测（阻止 MENUPOPUPSTART 恢复后被立即再次隐藏）
    void SetShellMenuSuppress(bool v) { shellMenuSuppress_ = v; }
    bool IsShellMenuSuppress() const { return shellMenuSuppress_; }

private:
    static bool IsForegroundFullscreen(bool debugLog);

    int  debounceCnt_{0};
    bool lastFullscreenState_{false};
    bool forceDebounceReset_{false};
    bool shellMenuSuppress_{false};  // Shell 菜单激活期间抑制全屏隐藏
};

} // namespace moekoe
