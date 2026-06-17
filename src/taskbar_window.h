// SPDX-License-Identifier: GPL-2.0
// taskbar_window.h - 任务栏嵌入窗口管理
//
// 职责:
//   - 查找 Shell_TrayWnd
//   - 创建作为其子窗口的 Layered Window
//   - 监听任务栏变化并自适应调整位置 / 尺寸
//   - 处理鼠标悬停/离开事件，显示控制按钮
//
#pragma once

#include "lyrics_data.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace moekoe {

// 任务栏方位
enum class TaskbarPosition {
    BOTTOM,
    TOP,
    LEFT,
    RIGHT,
    UNKNOWN,
};

struct TaskbarInfo {
    RECT             rect{0, 0, 0, 0};
    TaskbarPosition  position{TaskbarPosition::UNKNOWN};
    UINT             dpi{96};
    bool             autoHide{false};    // 任务栏是否开启自动隐藏
};

class TaskbarWindow {
public:
    TaskbarWindow();
    ~TaskbarWindow();

    TaskbarWindow(const TaskbarWindow&) = delete;
    TaskbarWindow& operator=(const TaskbarWindow&) = delete;

    // 创建嵌入任务栏内部的歌词窗口
    //   hInstance : 当前进程实例
    //   hParent   : 可选,任务栏句柄;若为 nullptr,内部自动查找
    // 返回 true 表示成功
    bool Create(HINSTANCE hInstance, HWND hParent = nullptr);

    void Destroy();

    // 句柄访问
    HWND GetHandle() const { return hwnd_; }

    // 主循环每帧调用,检查任务栏尺寸变化并自适应
    void CheckResize();

    // 强制重新计算位置(WM_DPICHANGED / WM_SETTINGCHANGE 时)
    void Reposition();

    // 任务栏信息
    TaskbarInfo GetTaskbarInfo() const { return info_; }

    // 静态:查找任务栏窗口
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
    bool IsAutoHideHidden() const { return taskbarAutoHide_ && !taskbarVisible_; }

    // 拖动偏移（用户手动拖动调整的位置）
    int GetDragOffsetX() const { return dragOffsetX_; }
    int GetDragOffsetY() const { return dragOffsetY_; }
    void SetDragOffset(int x, int y) { dragOffsetX_ = x; dragOffsetY_ = y; }

    // 显示模式（影响窗口尺寸计算）
    std::string GetDisplayMode() const { return displayMode_; }
    void SetDisplayMode(const std::string& mode) { displayMode_ = mode; }

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

private:
    // 窗口过程(静态 + 实例)
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // 内部
    void DetectTaskbarInfo();
    void PositionLyricsInTaskbar();
    HoverButton HitTestButton(int x, int y) const;

    /// 拖动松开后检测是否与其他任务栏子窗口重叠，若重叠则弹到最近的空闲位置
    void SnapToEmptySpace();

    // 状态
    HINSTANCE     hInstance_{nullptr};
    HWND          hwnd_{nullptr};
    HWND          hTaskbar_{nullptr};
    TaskbarInfo   info_{};
    RECT          lastTaskbarRect_{0, 0, 0, 0};
    bool          created_{false};
    bool          isHovering_{false};
    bool          trackingMouse_{false};
    bool          isDragging_{false};
    bool          positionLocked_{false};   // 位置锁定：禁止拖动
    bool          fullyLocked_{false};     // 完全锁定：禁止拖动+按钮交互
    bool          taskbarAutoHide_{false}; // 任务栏自动隐藏状态
    bool          taskbarVisible_{true};   // 自动隐藏模式下任务栏当前是否可见
    POINT         dragStartPos_{0, 0};     // 拖动开始时鼠标屏幕坐标
    POINT         dragStartWinPos_{0, 0};  // 拖动开始时窗口屏幕坐标
    int           dragOffsetX_{0};         // 用户拖动产生的累积偏移
    int           dragOffsetY_{0};

    std::string   displayMode_{"karaoke"};  // 显示模式: "karaoke" | "card"

    ButtonCallback onButtonClicked_;
    HoverChangedCallback onHoverChanged_;

    // Z-order 恢复: 每 kTopmostRestoreInterval 帧强制断言一次 HWND_TOPMOST
    // 原因: 任务栏(Shell_TrayWnd)也是系统级 TOPMOST 窗口，点击任务栏/展开托盘时
    //       Windows 会将任务栏提升到普通 TOPMOST 窗口之上，需定期恢复
    static constexpr int kTopmostRestoreInterval = 30;  // 约 0.5 秒 @60fps
    int           topmostFrameCounter_{0};
};

} // namespace moekoe
