// SPDX-License-Identifier: GPL-2.0
// taskbar_geometry.h - 任务栏几何信息与 UIA 枚举
//
// 职责:
//   - 任务栏尺寸/位置/DPI/AutoHide 检测
//   - UIA 子窗口枚举（GetChildRectsByUIA）
//   - SetWinEventHook 监听任务栏变化
//   - TaskbarInfo 结构体
//
// 拆分自 taskbar_window.h/cpp (A1)
#pragma once

#include <windows.h>
#include <UIAutomation.h>

#include <chrono>
#include <cstdint>

namespace moekoe {

enum class TaskbarPosition { BOTTOM, TOP, LEFT, RIGHT, UNKNOWN };

struct TaskbarInfo {
    RECT            rect{0, 0, 0, 0};
    TaskbarPosition position{TaskbarPosition::UNKNOWN};
    UINT            dpi{96};
    bool            autoHide{false};
};

class TaskbarGeometry {
public:
    TaskbarGeometry();
    ~TaskbarGeometry();

    TaskbarGeometry(const TaskbarGeometry&) = delete;
    TaskbarGeometry& operator=(const TaskbarGeometry&) = delete;

    // 查找 Shell_TrayWnd
    static HWND FindTaskbarHandle();

    // 获取任务栏信息：位置方位（基于 rcWork 中心线判断水平/垂直 + 偏左/偏右/偏上/偏下）、
    // DPI、AutoHide 状态（SHAppBarMessage ABS_AUTOHIDE）
    TaskbarInfo Detect(HWND hTaskbar);
    TaskbarInfo GetInfo() const { return info_; }

    // UIA 初始化/清理
    void InitUIA();
    void CleanupUIA();
    IUIAutomation* GetUIA() const { return uia_; }

    // 使用 UIA 枚举 Shell_TrayWnd 子窗口，获取关键区域的屏幕矩形
    bool GetChildRectsByUIA(RECT& taskListRect, bool& foundTaskList,
                            RECT& trayRect,    bool& foundTray,
                            RECT& rebarRect,   bool& foundRebar,
                            int   tbWidth);

    // UIA 节流缓存：返回 true 表示应重新枚举
    bool IsUiaCacheExpired(int intervalMs = 200);

    // 更新/读取 UIA 缓存
    void CacheUiaResults(RECT taskList, bool hasTL, RECT tray, bool hasTray, RECT rebar, bool hasRebar);
    void LoadUiaCache(RECT& taskList, bool& hasTL, RECT& tray, bool& hasTray, RECT& rebar, bool& hasRebar);

    // 自动隐藏状态
    bool IsAutoHide() const { return info_.autoHide; }
    void SetAutoHideVisible(bool v) { taskbarVisible_ = v; }
    bool IsAutoHideVisible() const { return taskbarVisible_; }

    // 方位判断
    bool IsVertical() const {
        return info_.position == TaskbarPosition::LEFT ||
               info_.position == TaskbarPosition::RIGHT;
    }

    // 静态事件钩子引用（实际定义在 taskbar_window.h/cpp 中，此处为兼容引用）
    // InstallTaskbarHook/RemoveTaskbarHook 委托给 TaskbarWindow 的实现

private:
    TaskbarInfo info_{};
    bool        taskbarVisible_{true};

    IUIAutomation* uia_{nullptr};

    // UIA 缓存
    std::chrono::steady_clock::time_point lastUiaRefreshTime_{};
    RECT  cachedTaskListRect_{};
    RECT  cachedTrayRect_{};
    RECT  cachedRebarRect_{};
    bool  cachedTaskListValid_{false};
    bool  cachedTrayValid_{false};
    bool  cachedRebarValid_{false};
    bool  cachedUiaValid_{false};
};

} // namespace moekoe
