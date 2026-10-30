// SPDX-License-Identifier: GPL-3.0
// media_card_window.cpp - 歌曲卡片弹窗实现
#include "taskbar/media_card_window.h"

#include "core/constants.h"
#include "render/media_card_layout.h"
#include "render/renderer.h"
#include "taskbar/shell_companion.h"
#include "util/logger.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace moekoe {

namespace {

constexpr UINT_PTR kOutsideCheckTimerId = 1;

// 进度条命中判定的额外纵向容差（DIP）
constexpr float kSeekHitPadDp = 12.0f;

} // namespace

MediaCardWindow::MediaCardWindow() = default;

MediaCardWindow::~MediaCardWindow() {
    Destroy();
}

bool MediaCardWindow::Create(HINSTANCE hInstance, HWND hLyricsOwner,
                             const AppearanceConfig& settings) {
    if (hwnd_) return true;
    if (!hInstance || !hLyricsOwner) return false;

    hInstance_ = hInstance;
    hLyricsOwner_ = hLyricsOwner;
    settings_ = settings;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = &MediaCardWindow::WndProc;
    wc.cbWndExtra    = sizeof(MediaCardWindow*);
    wc.hInstance     = hInstance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    // WS_EX_NOACTIVATE：不抢焦点；WS_EX_LAYERED：UpdateLayeredWindow 透明渲染
    const DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    hwnd_ = ::CreateWindowExW(exStyle, kWindowClass, L"", WS_POPUP,
                              0, 0, 0, 0,
                              nullptr, nullptr, hInstance, this);
    if (!hwnd_) return false;

    // 与歌词窗口同样挂到任务栏 owned 窗口链，保证 Z-order 在任务栏之上
    HWND hTaskbar = ShellCompanion::FindTaskbarHandle();
    if (hTaskbar) {
        ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hTaskbar));
    }

    renderer_ = std::make_unique<TaskbarRenderer>();
    renderer_->ApplySettings(settings_);
    if (!renderer_->Initialize(hwnd_)) {
        LogError("[MEDIA-CARD] renderer initialize failed\n");
        renderer_.reset();
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    lastDpi_ = ::GetDpiForWindow(hwnd_);

    return true;
}

void MediaCardWindow::Destroy() {
    Hide();
    if (hwnd_) {
        ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (renderer_) {
        renderer_->Shutdown();
        renderer_.reset();
    }
}

void MediaCardWindow::ApplySettings(const AppearanceConfig& settings) {
    settings_ = settings;
    if (renderer_) renderer_->ApplySettings(settings_);
}

float MediaCardWindow::DpiScale() const {
    UINT dpi = hwnd_ ? ::GetDpiForWindow(hwnd_) : 96;
    if (!dpi) dpi = 96;
    return static_cast<float>(dpi) / 96.0f;
}

void MediaCardWindow::UpdateBounds(const TaskbarInfo& info) {
    if (!hwnd_) return;

    const float s = DpiScale();
    const int w = static_cast<int>(std::lround(constants::MEDIA_CARD_WIDTH_DP * s));
    const int h = static_cast<int>(std::lround(constants::MEDIA_CARD_HEIGHT_DP * s));
    const int gap = static_cast<int>(std::lround(constants::MEDIA_CARD_TASKBAR_GAP_DP * s));

    // 锚点：优先以歌词窗口中心对齐，窗口尚未显示时退回任务栏中心
    RECT anchor{};
    if (hLyricsOwner_ && ::IsWindow(hLyricsOwner_)) {
        ::GetWindowRect(hLyricsOwner_, &anchor);
    }
    if (anchor.right <= anchor.left || anchor.bottom <= anchor.top) {
        anchor = info.rect;
    }
    const int anchorCx = (anchor.left + anchor.right) / 2;
    const int anchorCy = (anchor.top + anchor.bottom) / 2;

    int x = 0, y = 0;
    switch (info.position) {
    case TaskbarPosition::TOP:
        x = anchorCx - w / 2;
        y = info.rect.bottom + gap;
        break;
    case TaskbarPosition::LEFT:
        x = info.rect.right + gap;
        y = anchorCy - h / 2;
        break;
    case TaskbarPosition::RIGHT:
        x = info.rect.left - w - gap;
        y = anchorCy - h / 2;
        break;
    case TaskbarPosition::BOTTOM:
    case TaskbarPosition::UNKNOWN:
    default:
        x = anchorCx - w / 2;
        y = info.rect.top - h - gap;
        break;
    }

    // 限制在工作区内，避免贴边显示器上溢出屏幕
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = ::MonitorFromRect(&info.rect, MONITOR_DEFAULTTONEAREST);
    if (monitor && ::GetMonitorInfoW(monitor, &mi)) {
        x = std::clamp(x, static_cast<int>(mi.rcWork.left),
                       static_cast<int>(mi.rcWork.right) - w);
        y = std::clamp(y, static_cast<int>(mi.rcWork.top),
                       static_cast<int>(mi.rcWork.bottom) - h);
    }

    const RECT target{x, y, x + w, y + h};
    if (::EqualRect(&target, &lastBounds_)) return;
    lastBounds_ = target;

    ::SetWindowPos(hwnd_, nullptr, x, y, w, h,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

void MediaCardWindow::ShowFor(const RenderState& state, const TaskbarInfo& info) {
    if (!hwnd_ || !renderer_) return;

    UpdateBounds(info);

    if (!visible_) {
        visible_ = true;
        // 开卡这次点击可能仍按住：直到左键抬起前不执行外部点击关闭
        ignoreOutsideUntilUp_ = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_SHOWWINDOW);
        ::SetTimer(hwnd_, kOutsideCheckTimerId,
                   static_cast<UINT>(constants::MEDIA_CARD_OUTSIDE_CHECK_MS), nullptr);
    }

    RenderFrame(state, info);
}

void MediaCardWindow::Hide() {
    if (!hwnd_ || !visible_) return;
    visible_ = false;
    hoverButton_ = -1;
    pressedButton_ = -1;
    seekDragging_ = false;
    ::KillTimer(hwnd_, kOutsideCheckTimerId);
    if (::GetCapture() == hwnd_) ::ReleaseCapture();
    ::ShowWindow(hwnd_, SW_HIDE);
}

void MediaCardWindow::RenderFrame(const RenderState& state, const TaskbarInfo& info) {
    if (!visible_ || !renderer_ || !hwnd_) return;

    duration_ = state.duration;
    UpdateBounds(info);

    // DPI 变化（跨显示器）时重建渲染资源：Initialize 会按新窗口 DPI 重建
    // 文本格式与画刷，避免文字/图标尺寸与物理像素布局不一致
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    if (dpi && dpi != lastDpi_) {
        lastDpi_ = dpi;
        renderer_->ApplySettings(settings_);
    }

    const double preview = seekDragging_ ? seekPreviewSeconds_ : -1.0;
    renderer_->RenderMediaCard(state, hoverButton_, preview, seekDragging_);
}

int MediaCardWindow::HitTest(int x, int y) const {
    if (!hwnd_) return -1;

    const float s = DpiScale();
    const MediaCardLayout L = MediaCardLayout::Compute(s);

    auto inCircle = [&](float cx, float cy, float radius) {
        const float dx = static_cast<float>(x) - cx;
        const float dy = static_cast<float>(y) - cy;
        return (dx * dx + dy * dy) <= radius * radius;
    };

    if (inCircle(L.playCx, L.controlsCy, L.playRadius + 2.0f * s)) return 1;
    if (inCircle(L.prevCx, L.controlsCy, L.sideRadius * 1.25f + 2.0f * s)) return 0;
    if (inCircle(L.nextCx, L.controlsCy, L.sideRadius * 1.25f + 2.0f * s)) return 2;

    const float padY = kSeekHitPadDp * s;
    if (static_cast<float>(y) >= L.seekCy - padY &&
        static_cast<float>(y) <= L.seekCy + padY &&
        static_cast<float>(x) >= L.seekX0 - padY &&
        static_cast<float>(x) <= L.seekX1 + padY) {
        return 3;
    }
    return -1;
}

void MediaCardWindow::CheckOutsideClick() {
    if (!visible_ || !hwnd_) return;
    // 卡片自己持有捕获时，点击从卡片内部开始，不视为“外部点击”
    if (::GetCapture() == hwnd_) return;

    if (!(::GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        ignoreOutsideUntilUp_ = false;
        return;
    }
    if (ignoreOutsideUntilUp_) return;

    POINT pt{};
    ::GetCursorPos(&pt);

    RECT card{};
    ::GetWindowRect(hwnd_, &card);
    if (::PtInRect(&card, pt)) return;

    // 点在歌词窗口上：交给 TaskbarWindow 的单击回调决定开/关，避免先隐藏再显示闪烁
    if (hLyricsOwner_ && ::IsWindow(hLyricsOwner_)) {
        RECT lyrics{};
        ::GetWindowRect(hLyricsOwner_, &lyrics);
        if (::PtInRect(&lyrics, pt)) return;
    }

    Hide();
}

LRESULT CALLBACK MediaCardWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MediaCardWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MediaCardWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MediaCardWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        const int hit = self->HitTest(x, y);
        const int newHover = (hit >= 0 && hit <= 2) ? hit : -1;
        if (newHover != self->hoverButton_) self->hoverButton_ = newHover;

        if (self->seekDragging_) {
            const float s = self->DpiScale();
            const MediaCardLayout L = MediaCardLayout::Compute(s);
            const float span = L.seekX1 - L.seekX0;
            if (span > 1.0f && self->duration_ > 0.0) {
                const float frac = std::clamp(
                    (static_cast<float>(x) - L.seekX0) / span, 0.0f, 1.0f);
                self->seekPreviewSeconds_ = frac * self->duration_;
            }
        }

        if (!self->trackingMouse_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            ::TrackMouseEvent(&tme);
            self->trackingMouse_ = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        self->trackingMouse_ = false;
        self->hoverButton_ = -1;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int hit = self->HitTest(x, y);
        if (hit >= 0 && hit <= 2) {
            self->pressedButton_ = hit;
            ::SetCapture(hwnd);
        } else if (hit == 3 && self->duration_ > 0.0) {
            self->seekDragging_ = true;
            const float s = self->DpiScale();
            const MediaCardLayout L = MediaCardLayout::Compute(s);
            const float span = L.seekX1 - L.seekX0;
            if (span > 1.0f) {
                const float frac = std::clamp(
                    (static_cast<float>(x) - L.seekX0) / span, 0.0f, 1.0f);
                self->seekPreviewSeconds_ = frac * self->duration_;
            } else {
                self->seekPreviewSeconds_ = 0.0;
            }
            ::SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (self->pressedButton_ >= 0) {
            const int btn = self->pressedButton_;
            self->pressedButton_ = -1;
            if (self->onButton_) {
                HoverButton mapped = HoverButton::None;
                if (btn == 0) mapped = HoverButton::Prev;
                else if (btn == 1) mapped = HoverButton::PlayPause;
                else if (btn == 2) mapped = HoverButton::Next;
                self->onButton_(mapped);
            }
            ::ReleaseCapture();
        } else if (self->seekDragging_) {
            const double target = self->seekPreviewSeconds_;
            self->seekDragging_ = false;
            ::ReleaseCapture();
            if (self->onSeek_ && self->duration_ > 0.0) self->onSeek_(target);
        }
        return 0;
    }
    case WM_CAPTURECHANGED: {
        // 捕获意外丢失（Alt+Tab 等）：放弃半程操作
        self->pressedButton_ = -1;
        self->seekDragging_ = false;
        return 0;
    }
    case WM_TIMER: {
        if (wParam == kOutsideCheckTimerId) {
            self->CheckOutsideClick();
            return 0;
        }
        break;
    }
    case WM_DESTROY: {
        ::KillTimer(hwnd, kOutsideCheckTimerId);
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace moekoe
