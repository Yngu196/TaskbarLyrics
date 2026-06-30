// SPDX-License-Identifier: GPL-3.0
// taskbar_embedder.h - 窗口创建/销毁/显示/隐藏/SetWindowPos
//
// 职责:
//   - 创建/销毁 Layered Window
//   - SetWindowPos 定位
//   - 鼠标悬停/拖动检测
//   - WndProc 窗口过程
//
// 拆分自 taskbar_window.h/cpp (A1)
#pragma once

#include "lyrics_data.h"

#include <windows.h>

#include <functional>
#include <string>

namespace moekoe {

enum class HoverButton;

class TaskbarEmbedder {
public:
    TaskbarEmbedder();
    ~TaskbarEmbedder();

    TaskbarEmbedder(const TaskbarEmbedder&) = delete;
    TaskbarEmbedder& operator=(const TaskbarEmbedder&) = delete;

    // 创建 WS_EX_LAYERED + WS_POPUP 浮动窗口（无父窗口，通过 GWLP_HWNDPARENT 绑定 Z-order）
    bool Create(HINSTANCE hInstance, HWND hParent, const wchar_t* className, int dpi);
    void Destroy();

    HWND GetHandle() const { return hwnd_; }
    void SetHandle(HWND hwnd) { hwnd_ = hwnd; }

    // 定位窗口到任务栏指定坐标；forceTopmost 用于全屏恢复时将窗口从 offscreen 拉回可见区域
    void Position(int x, int y, int w, int h, bool forceTopmost = false);

    // 全屏隐藏：移到 (-32000,-32000) + SWP_HIDEWINDOW，双重保险防止闪现
    // 全屏恢复：SWP_SHOWWINDOW + HWND_TOPMOST 脱离 owned Z-order 限制
    void SetFullscreenHidden(bool hidden);
    bool IsFullscreenHidden() const { return fullscreenHidden_; }

    // 拖动偏移（用户手动拖动的累积偏移量，持久化到配置文件）
    int  GetDragOffsetX() const { return dragOffsetX_; }
    int  GetDragOffsetY() const { return dragOffsetY_; }
    void SetDragOffset(int x, int y) { dragOffsetX_ = x; dragOffsetY_ = y; }

    // 悬停/拖动状态（驱动渲染层按钮显隐和拖动边框）
    bool IsHovering() const { return isHovering_; }
    bool IsDragging() const { return isDragging_; }

    // 位置锁定：禁止拖动调整位置（仅锁定拖动，不锁定按钮交互）
    bool IsPositionLocked() const { return positionLocked_; }
    void SetPositionLocked(bool locked) { positionLocked_ = locked; }
    // 完全锁定：禁止拖动 + 禁止悬停按钮交互
    bool IsFullyLocked() const { return fullyLocked_; }
    void SetFullyLocked(bool locked) { fullyLocked_ = locked; }

    // 按钮命中测试（实际实现由 TaskbarWindow 完成，此处为占位）
    HoverButton HitTestButton(int x, int y, int w, int h, bool isVertical, bool isPlaying) const;

    // 拖动开始时记录鼠标屏幕坐标和窗口屏幕坐标作为基准点
    void BeginDrag(int mouseScreenX, int mouseScreenY);
    void EndDrag();

    // 回调
    using ButtonCallback = std::function<void(HoverButton)>;
    void OnButtonClicked(ButtonCallback cb) { onButtonClicked_ = std::move(cb); }
    using HoverChangedCallback = std::function<void()>;
    void OnHoverChanged(HoverChangedCallback cb) { onHoverChanged_ = std::move(cb); }

    // 静态: 窗口类注册
    static constexpr const wchar_t* kWindowClass = L"MoeKoeTaskbarLyricsClass";

    // 静态 WndProc（占位，实际窗口过程由 TaskbarWindow 注册和接管）
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    HINSTANCE     hInstance_{nullptr};
    HWND          hwnd_{nullptr};
    bool          isHovering_{false};
    bool          trackingMouse_{false};
    bool          isDragging_{false};
    bool          positionLocked_{false};
    bool          fullyLocked_{false};
    bool          fullscreenHidden_{false};
    int           dragOffsetX_{0};
    int           dragOffsetY_{0};
    POINT         dragStartPos_{0, 0};
    POINT         dragStartWinPos_{0, 0};

    ButtonCallback       onButtonClicked_;
    HoverChangedCallback onHoverChanged_;
};

} // namespace moekoe
