// SPDX-License-Identifier: GPL-2.0
// taskbar_window.h - 任务栏嵌入窗口管理（门面类）
//
// 职责: 组合以下子模块，保持外部接口不变
//   - shell_companion:    任务栏几何/全屏检测/WinEvent 钩子/定位计算
//   - taskbar_embedder:   窗口创建/销毁/显隐/SetWindowPos / 拖动/悬停状态
//
// A1 拆分重构 → v0.5.x Shell Companion 独立化
//
#pragma once

#include "fullscreen_detector.h"
#include "shell_companion.h"
#include "taskbar_embedder.h"
#include "taskbar_geometry.h"
#include "lyrics_data.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace moekoe {

// 类型别名：TaskbarPosition / TaskbarInfo 由 taskbar_geometry.h 统一定义
using TaskbarPosition = moekoe::TaskbarPosition;
using TaskbarInfo     = moekoe::TaskbarInfo;

class TaskbarWindow {
public:
    TaskbarWindow();
    ~TaskbarWindow();

    TaskbarWindow(const TaskbarWindow&) = delete;
    TaskbarWindow& operator=(const TaskbarWindow&) = delete;

    // ── 子模块访问（供需要深入控制的外部代码使用）──
    FullscreenDetector& Fullscreen() { return companion_.Fullscreen(); }
    TaskbarGeometry&    Geometry()   { return companion_.Geometry(); }
    TaskbarEmbedder&    Embedder()   { return companion_.Embedder(); }

    // 创建嵌入任务栏内部的歌词窗口
    //   hInstance : 当前进程实例
    //   hParent   : 可选,任务栏句柄;若为 nullptr,内部自动查找
    // 返回 true 表示成功
    bool Create(HINSTANCE hInstance, HWND hParent = nullptr);

    void Destroy();

    // 句柄访问（委托 embedder_）
    HWND GetHandle() const { return hwnd_; }

    // 主循环每帧调用,检查任务栏尺寸变化并自适应
    void CheckResize() { companion_.CheckResize(hwnd_); }

    // 强制重新计算位置(WM_DPICHANGED / WM_SETTINGCHANGE 时)
    void Reposition();

    // 任务栏信息
    TaskbarInfo GetTaskbarInfo() const { return companion_.GetTaskbarInfo(); }

    // 静态:查找任务栏窗口（委托 ShellCompanion）
    static HWND FindTaskbarHandle();

    // 悬停状态
    bool IsHovering() const { return isHovering_; }
    bool IsDragging() const { return isDragging_; }

    // 位置锁定：禁止拖动调整位置
    bool IsPositionLocked() const { return positionLocked_; }
    void SetPositionLocked(bool locked) { positionLocked_ = locked; }

    // 完全锁定：禁止拖动+禁止悬停按钮交互
    bool IsFullyLocked() const { return fullyLocked_; }
    void SetFullyLocked(bool locked) { fullyLocked_ = locked; }

    // APPBAR 自动隐藏状态查询（供主循环判断是否应跳过渲染）
    bool IsAutoHideHidden() const { return companion_.IsAutoHideHidden(); }

    // 全屏检测隐藏状态查询（供主循环判断是否应跳过渲染）
    bool IsFullscreenHidden() const { return companion_.IsFullscreenHidden(); }
    void SetFullscreenHidden(bool hidden) { companion_.SetFullscreenHidden(hidden, hwnd_); }
    void InvalidatePositionCache() { lastPosRect_ = {-1, -1, -1, -1}; }

    // 拖动偏移（用户手动拖动调整的位置）
    int GetDragOffsetX() const { return dragOffsetX_; }
    int GetDragOffsetY() const { return dragOffsetY_; }
    void SetDragOffset(int x, int y) { dragOffsetX_ = x; dragOffsetY_ = y; }

    // 显示模式（影响窗口尺寸计算）
    std::string GetDisplayMode() const { return displayMode_; }
    void SetDisplayMode(const std::string& mode) { displayMode_ = mode; }

    // 是否处于垂直任务栏模式（LEFT / RIGHT）
    bool IsVerticalTaskbar() const { return companion_.IsVerticalTaskbar(); }

    // 按钮点击回调
    using ButtonCallback = std::function<void(HoverButton)>;
    void OnButtonClicked(ButtonCallback cb) { onButtonClicked_ = std::move(cb); }

    // 悬停状态变化回调（用于立即触发重绘）
    using HoverChangedCallback = std::function<void()>;
    void OnHoverChanged(HoverChangedCallback cb) { onHoverChanged_ = std::move(cb); }

    // 窗口类名
    static constexpr const wchar_t* kWindowClass = L"MoeKoeTaskbarLyricsClass";

    // 自定义消息:通知外部(主循环)执行每帧任务
    static constexpr UINT WM_FRAME_TICK = WM_USER + 0x100;
    static constexpr UINT WM_DELAYED_REPOSITION = WM_USER + 0x101;  // WinEvent 触发的延迟定位

    // Shell 交互解锁（供 WndProc 定时器回调）
    void UnlockShell(int timerId) { companion_.UnlockShellInteraction(timerId); }

private:
    // 窗口过程(静态 + 实例)
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // 内部定位
    void InternalPosition();
    HoverButton HitTestButton(int x, int y) const;
    void SnapToEmptySpace();

    // 状态
    HINSTANCE     hInstance_{nullptr};
    HWND          hwnd_{nullptr};
    bool          created_{false};
    bool          isHovering_{false};
    bool          trackingMouse_{false};
    bool          isDragging_{false};
    bool          positionLocked_{false};   // 位置锁定：禁止拖动
    bool          fullyLocked_{false};     // 完全锁定：禁止拖动+按钮交互
    RECT          lastPosRect_{-1, -1, -1, -1};  // 上次 SetWindowPos 的坐标，避免无变化时重复提交 DWM
    POINT         dragStartPos_{0, 0};     // 拖动开始时鼠标屏幕坐标
    POINT         dragStartWinPos_{0, 0};  // 拖动开始时窗口屏幕坐标
    int           dragOffsetX_{0};         // 用户拖动产生的累积偏移
    int           dragOffsetY_{0};
    TaskbarPosition lastPosition_{TaskbarPosition::UNKNOWN};  // 用于检测方位变化（重置拖动偏移）

    std::string   displayMode_{"karaoke"};  // 显示模式: "karaoke" | "card"

    ButtonCallback onButtonClicked_;
    HoverChangedCallback onHoverChanged_;

    // ── v0.5.x: ShellCompanion 封装 Shell 层逻辑 ──
    ShellCompanion companion_;
};

} // namespace moekoe
