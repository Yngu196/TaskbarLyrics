// SPDX-License-Identifier: GPL-3.0
// taskbar_selector.h - 多任务栏选择器
//
// 职责:
//   - 枚举系统全部任务栏：主任务栏 Shell_TrayWnd + 各副屏任务栏 Shell_SecondaryTrayWnd
//   - 根据活动(前台)窗口 / 鼠标所在显示器，动态决策应绑定的目标任务栏
//   - 输出带防抖的切换建议，避免鼠标扫过/焦点瞬变导致任务栏来回跳动
//
// 优先级策略:
//   L1 活动(前台)窗口所在显示器 —— 用户任务上下文所在屏幕
//   L2 鼠标所在显示器            —— 无有效活动窗口(焦点在桌面/Shell)时
//   L3 主任务栏                  —— 兜底（目标显示器无任务栏等边界）
//
// 边界处理:
//   - 全屏应用在前台: 仍按其所在显示器选择（歌词显隐由 FullscreenDetector 独立处理，互不影响）
//   - 焦点在桌面(Progman/WorkerW/Shell)或为本歌词窗口: 视为无活动窗口信号，降级到鼠标位置
//   - 目标显示器没有任务栏(关闭"在所有显示器显示任务栏"): 强制刷新枚举后回退主任务栏
#pragma once

#include <windows.h>

#include <chrono>
#include <string>
#include <vector>

namespace moekoe {

class TaskbarSelector {
public:
    // 重置枚举缓存与防抖状态（显示器布局变化 / Explorer 重启 / 初始化时调用）
    void Reset();

    // 决策目标任务栏句柄。
    //   lyricsWnd:    歌词窗口句柄（用于排除自身）
    //   currentBound: 当前已绑定句柄
    //   outShouldSwitch: 输出，防抖确认后为 true 表示调用方应执行切换
    // 返回: 当前应绑定的任务栏句柄（currentBound 或新的目标）
    HWND SelectTarget(HWND lyricsWnd, HWND currentBound, bool& outShouldSwitch);

    // 最近一次决策是否使用了鼠标位置（日志/调试用）
    bool LastUsedMouse() const { return lastUsedMouse_; }

    // ── 手动指定模式 ──
    // 手动模式: 锁定到用户所选显示器的任务栏，不再按活动窗口/鼠标自动跟随。
    // 调用 SetManualMode/SetManualIndex 后，下一次 SelectTarget 会立即刷新枚举并按新目标重绑。
    void SetManualMode(bool manual) {
        manualMode_ = manual;
        if (manual) ForceReenum();   // 进入手动模式时刷新显示器列表，保证索引有效
    }
    void SetManualIndex(int index) {
        manualIndex_ = index;
        ForceReenum();               // 目标显示器变化时刷新显示器列表
    }
    bool ManualMode() const { return manualMode_; }

    // 系统显示器总数（供设置页判断是否多显示器）
    static int MonitorCount();

    // 显示器标签列表（"显示器 N (宽x高)"，序号 0-based，与 manualIndex 对应）
    static std::vector<std::string> MonitorLabels();

private:
    struct TaskbarEntry {
        HWND     hwnd{nullptr};
        HMONITOR monitor{nullptr};
        bool     primary{false};
    };

    void EnumerateTaskbars();                 // 带节流的全量枚举（EnumWindows 收集任务栏 + 显示器列表）
    void ForceReenum() { enumerated_ = false; lastEnumTime_ = {}; }  // 立即刷新枚举缓存
    HWND FindByMonitor(HMONITOR mon) const;   // 精确匹配该显示器的任务栏句柄
    bool IsShellOrDesktop(HWND hwnd) const;   // 桌面/Shell 窗口判断（不作为活动窗口信号）
    HWND PrimaryTaskbar() const;              // 主任务栏兜底

    std::vector<TaskbarEntry> taskbars_;
    std::vector<HMONITOR>     displays_;      // 全部显示器（EnumDisplayMonitors 顺序，0-based 序号）
    std::chrono::steady_clock::time_point lastEnumTime_{};
    bool enumerated_{false};

    // 手动指定模式状态
    bool manualMode_{false};
    int  manualIndex_{0};

    // 防抖状态（跨屏切换需持续稳定才生效）
    HWND    debounceTarget_{nullptr};
    std::chrono::steady_clock::time_point debounceSince_{};
    bool    debouncing_{false};
    bool    lastUsedMouse_{false};
};

} // namespace moekoe
