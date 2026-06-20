// SPDX-License-Identifier: GPL-2.0
// taskbar_window.h - 任务栏嵌入窗口管理（门面类）
//
// 职责: 组合以下子模块，保持外部接口不变
//   - taskbar_embedder:   窗口创建/销毁/显隐/SetWindowPos 定位
//   - taskbar_geometry:   任务栏尺寸/位置/DPI/AutoHide 检测、UIA 子窗口枚举
//   - fullscreen_detector: 全屏检测 + 防抖
//
// A1 拆分重构
//
#pragma once

#include "fullscreen_detector.h"
#include "taskbar_embedder.h"
#include "taskbar_geometry.h"
#include "lyrics_data.h"

#include <windows.h>

#include <chrono>
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
    FullscreenDetector& Fullscreen() { return fullscreenDetector_; }
    TaskbarGeometry&    Geometry()   { return geometry_; }
    TaskbarEmbedder&    Embedder()   { return embedder_; }

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

    // 全屏检测隐藏状态查询（供主循环判断是否应跳过渲染）
    bool IsFullscreenHidden() const { return fullscreenHidden_; }
    void SetFullscreenHidden(bool hidden);

    // 拖动偏移（用户手动拖动调整的位置）
    int GetDragOffsetX() const { return dragOffsetX_; }
    int GetDragOffsetY() const { return dragOffsetY_; }
    void SetDragOffset(int x, int y) { dragOffsetX_ = x; dragOffsetY_ = y; }

    // 显示模式（影响窗口尺寸计算）
    std::string GetDisplayMode() const { return displayMode_; }
    void SetDisplayMode(const std::string& mode) { displayMode_ = mode; }

    // 是否处于垂直任务栏模式（LEFT / RIGHT）
    bool IsVerticalTaskbar() const {
        return info_.position == TaskbarPosition::LEFT ||
               info_.position == TaskbarPosition::RIGHT;
    }

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
    static constexpr UINT WM_DELAYED_REPOSITION = WM_USER + 0x101;  // SetWinEventHook 触发的延迟定位

    // SetWinEventHook: 监听 Shell_TrayWnd 位置变化，解决 auto-hide/Win 键偏移
    static void InstallTaskbarHook(HWND lyricsWnd);
    static void RemoveTaskbarHook();
    static HWINEVENTHOOK s_taskbarHook_;
    static HWINEVENTHOOK s_shellMenuHook_;    // Start Menu 弹出/关闭事件
    static HWND s_lyricsWnd_;
    static bool s_shellInteractionLocked_;    // Start Menu 激活期间锁定定位
    static std::chrono::steady_clock::time_point s_shellInteractionLockedTime_;  // 锁设置时间戳（超时自动复位）
    static RECT s_frozenTaskbarRect_;         // 锁定时冻结的任务栏几何快照
    static RECT s_lastGoodTaskbarRect_;      // 非冻结期间已知稳定 rect（供冻结快照使用，避免 Explorer 脏写污染）
    static HWINEVENTHOOK s_foregroundHook_;      // Win11 Start Menu 前台检测
    static bool          s_lockedByStartMenuFg_; // 前台 Hook 设置的锁（与 MENUPOPUPSTART 区分来源）
    static TaskbarWindow* s_instance_;            // 指向唯一实例，供静态回调访问实例状态

    friend void CALLBACK ShellMenuWinEventProc(HWINEVENTHOOK, DWORD event, HWND,
                                               LONG, LONG, DWORD, DWORD);
    friend void CALLBACK ForegroundWinEventProc(HWINEVENTHOOK, DWORD, HWND,
                                                 LONG, LONG, DWORD, DWORD);

private:
    // 窗口过程(静态 + 实例)
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // ═══ 内部定位逻辑 ═══
    void DetectTaskbarInfo();
    void PositionLyricsInTaskbar();
    HoverButton HitTestButton(int x, int y) const;
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
    bool          taskbarVisible_{false};   // 任务栏当前是否可见
    bool          fullscreenHidden_{false};  // 全屏检测导致的窗口隐藏状态
    RECT          stableTaskbarRect_{};     // 稳定帧矩形（动画期间不更新）
    RECT          stableTaskListRect_{};    // 任务列表子窗口稳定矩形（动画期间不更新）
    bool          stableTaskListValid_{false};
    int           cachedRightEdgeOffset_{0}; // tbRect.right - rightEdge 缓存（动画期间防射线位置跳动）
    POINT         dragStartPos_{0, 0};     // 拖动开始时鼠标屏幕坐标
    POINT         dragStartWinPos_{0, 0};  // 拖动开始时窗口屏幕坐标
    int           dragOffsetX_{0};         // 用户拖动产生的累积偏移
    int           dragOffsetY_{0};

    std::string   displayMode_{"karaoke"};  // 显示模式: "karaoke" | "card"

    ButtonCallback onButtonClicked_;
    HoverChangedCallback onHoverChanged_;

    // ── A1 子模块：窗口嵌入 / 几何检测 / 全屏检测 ──
    FullscreenDetector fullscreenDetector_;
    TaskbarGeometry    geometry_;
    TaskbarEmbedder    embedder_;

};

} // namespace moekoe
