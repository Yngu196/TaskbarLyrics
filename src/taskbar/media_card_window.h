// SPDX-License-Identifier: GPL-3.0
// media_card_window.h - 点击任务栏歌词窗口后弹出的歌曲卡片窗口
//
// 独立的分层弹窗（owned by Shell_TrayWnd），内部持有第二个 TaskbarRenderer。
// 之所以不复用歌词窗口：歌词窗口每帧被 ShellCompanion::PositionLyricsInTaskbar
// 重新定位/改变尺寸，卡片若共用会被覆盖，无法稳定显示在任务栏上方。
#pragma once

#include "config/config.h"
#include "lyrics/lyrics_data.h"
#include "taskbar/taskbar_geometry.h"

#include <windows.h>

#include <functional>
#include <memory>

namespace moekoe {

class TaskbarRenderer;

class MediaCardWindow {
public:
    // 按钮回调：HoverButton::Prev / PlayPause / Next
    using ButtonCallback = std::function<void(HoverButton)>;
    // 拖动进度条结束后的 seek 回调（秒）
    using SeekCallback = std::function<void(double)>;

    MediaCardWindow();
    ~MediaCardWindow();

    MediaCardWindow(const MediaCardWindow&) = delete;
    MediaCardWindow& operator=(const MediaCardWindow&) = delete;

    // hLyricsOwner：任务栏歌词窗口（用于计算锚点与 DPI）
    bool Create(HINSTANCE hInstance, HWND hLyricsOwner, const AppearanceConfig& settings);
    void Destroy();

    bool IsVisible() const { return visible_; }

    // 以当前任务栏信息计算位置并显示；已显示时仅刷新位置
    void ShowFor(const RenderState& state, const TaskbarInfo& info);
    void Hide();

    // 每帧刷新（位置跟随任务栏/歌词窗口变化，并重绘）
    void RenderFrame(const RenderState& state, const TaskbarInfo& info);

    void ApplySettings(const AppearanceConfig& settings);

    void SetOnButton(ButtonCallback cb) { onButton_ = std::move(cb); }
    void SetOnSeek(SeekCallback cb) { onSeek_ = std::move(cb); }

    static constexpr const wchar_t* kWindowClass = L"MoeKoeMediaCardClass";

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // 命中测试：-1=无, 0=上一首, 1=播放/暂停, 2=下一首, 3=进度条
    int HitTest(int x, int y) const;
    float DpiScale() const;

    // 计算并应用窗口位置/尺寸（坐标仅作缓存，尺寸变化时触发 SetWindowPos）
    void UpdateBounds(const TaskbarInfo& info);
    // 立即隐藏（用于外部点击检测）
    void CheckOutsideClick();

    HINSTANCE hInstance_{nullptr};
    HWND      hwnd_{nullptr};
    HWND      hLyricsOwner_{nullptr};

    bool visible_{false};
    bool trackingMouse_{false};
    bool ignoreOutsideUntilUp_{false};  // 开卡那次点击尚未抬起，避免误关闭

    int  hoverButton_{-1};      // 悬停按钮（渲染器高亮用）
    int  pressedButton_{-1};    // 按下的按钮
    bool seekDragging_{false};
    double seekPreviewSeconds_{0.0};
    double duration_{0.0};

    RECT lastBounds_{-1, -1, -1, -1};
    UINT lastDpi_{0};

    std::unique_ptr<TaskbarRenderer> renderer_;

    ButtonCallback onButton_;
    SeekCallback   onSeek_;

    AppearanceConfig settings_{};
};

} // namespace moekoe
