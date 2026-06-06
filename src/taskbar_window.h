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

    // 鼠标悬停状态
    bool IsHovering() const { return isHovering_; }

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

    // 状态
    HINSTANCE     hInstance_{nullptr};
    HWND          hwnd_{nullptr};
    HWND          hTaskbar_{nullptr};
    TaskbarInfo   info_{};
    RECT          lastTaskbarRect_{0, 0, 0, 0};
    bool          created_{false};
    bool          isHovering_{false};
    bool          trackingMouse_{false};

    ButtonCallback onButtonClicked_;
    HoverChangedCallback onHoverChanged_;
};

} // namespace moekoe
