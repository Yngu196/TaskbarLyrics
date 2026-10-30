// SPDX-License-Identifier: GPL-3.0
// media_card_renderer.cpp - 媒体卡片（点击任务栏歌词窗口弹出的歌曲卡片）渲染
//
// 布局参考 Windows 11 媒体控制卡片：
//   ┌──────────────────────────────────────────┐
//   │ [封面]  歌曲名                  ⏮  ▶  ⏭  │
//   │         歌手                              │
//   │         来源                              │
//   │  ────────●────────────────────────       │
//   │  0:01                          3:45      │
//   └──────────────────────────────────────────┘
//
#include "render/renderer.h"
#include "render/media_card_layout.h"
#include "render/renderer_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace moekoe {

using renderer_utils::FirstUtf8CharAsWide;
using renderer_utils::Utf8ToWide;

namespace {

/// 把秒数格式化为 m:ss / h:mm:ss
std::wstring FormatClock(double seconds) {
    if (!(seconds >= 0.0)) seconds = 0.0;
    if (seconds > 359999.0) seconds = 359999.0;
    const int total = static_cast<int>(std::floor(seconds));
    wchar_t buf[32] = {};
    if (total >= 3600) {
        std::swprintf(buf, 32, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    } else {
        std::swprintf(buf, 32, L"%d:%02d", total / 60, total % 60);
    }
    return buf;
}

} // namespace

void TaskbarRenderer::DrawMediaCardIcon(const wchar_t* icon, const D2D1_RECT_F& rect,
                                        IDWriteTextFormat* format, ID2D1Brush* brush) {
    if (!renderTarget_ || !icon || !format || !brush) return;
    renderTarget_->DrawTextW(icon, 1, format, rect, brush);
}

void TaskbarRenderer::DrawMediaCardText(const std::wstring& text, IDWriteTextFormat* format,
                                        const D2D1_RECT_F& rect, ID2D1Brush* brush,
                                        DWRITE_TEXT_ALIGNMENT alignment, bool ellipsis) {
    if (!renderTarget_ || !format || !brush || text.empty()) return;

    std::wstring draw = text;
    const float maxWidth = rect.right - rect.left;
    const float maxHeight = rect.bottom - rect.top;
    if (ellipsis && dwriteFactory_ && maxWidth > 1.0f) {
        auto fits = [&](const std::wstring& candidate) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwriteFactory_->CreateTextLayout(
                    candidate.c_str(), static_cast<UINT32>(candidate.size()),
                    format, maxWidth, maxHeight, layout.GetAddressOf())) || !layout) {
                return true;  // 测量失败时不截断，避免文本消失
            }
            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(layout->GetMetrics(&metrics))) return true;
            return metrics.widthIncludingTrailingWhitespace <= maxWidth;
        };

        if (!fits(draw)) {
            // 二分查找最长可容纳前缀，尾部补省略号
            size_t lo = 0, hi = draw.size();
            while (lo < hi) {
                const size_t mid = (lo + hi + 1) / 2;
                if (fits(draw.substr(0, mid) + L"\u2026")) lo = mid;
                else hi = mid - 1;
            }
            draw = lo == 0 ? std::wstring(L"\u2026") : draw.substr(0, lo) + L"\u2026";
        }
    }

    const DWRITE_TEXT_ALIGNMENT oldAlign = format->GetTextAlignment();
    format->SetTextAlignment(alignment);
    renderTarget_->DrawTextW(draw.c_str(), static_cast<UINT32>(draw.size()),
                             format, rect, brush);
    format->SetTextAlignment(oldAlign);
}

void TaskbarRenderer::RenderMediaCard(const RenderState& state, int hoverButton,
                                      double seekPreviewSeconds, bool seekDragging) {
    if (!initialized_ || !renderTarget_ || !mediaPanelBrush_) return;

    RECT rc{};
    ::GetWindowRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>(std::max<LONG>(rc.right - rc.left, 1));
    const UINT h = static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 1));
    if (w != width_ || h != height_) {
        Resize(w, h, dpi_);
    }

    const float s = static_cast<float>(dpi_) / 96.0f;
    const MediaCardLayout L = MediaCardLayout::Compute(s);

    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // ── 卡片底（深色玻璃 + 细描边） ──
    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)),
        16.0f * s, 16.0f * s);
    renderTarget_->FillRoundedRectangle(panel, mediaPanelBrush_.Get());
    renderTarget_->DrawRoundedRectangle(panel, mediaBorderBrush_.Get(), 1.0f);

    // ── 封面 ──
    const std::wstring songNameW = Utf8ToWide(
        state.songName.empty() ? state.songTitle : state.songName);
    const wchar_t fallbackChar = FirstUtf8CharAsWide(
        state.songName.empty() ? state.songTitle : state.songName);
    DrawCoverArt(state.coverArtUrl, fallbackChar, L.coverX, L.coverY, L.coverSize);

    // ── 文本信息 ──
    const std::wstring titleW = songNameW.empty() ? L"未知歌曲" : songNameW;
    DrawMediaCardText(titleW, mediaTitleFormat_.Get(),
                      D2D1::RectF(L.textX, L.titleTop, L.textRight, L.titleTop + L.titleHeight),
                      mediaTextBrush_.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);

    const std::wstring artistW = state.artist.empty() ? L"未知歌手" : Utf8ToWide(state.artist);
    DrawMediaCardText(artistW, mediaArtistFormat_.Get(),
                      D2D1::RectF(L.textX, L.artistTop, L.textRight, L.artistTop + L.artistHeight),
                      mediaSubTextBrush_.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);

    // 来源行：仅显示文本（不再绘制红色音符图标）
    DrawMediaCardText(L"MoeKoe 音乐", mediaMetaFormat_.Get(),
                      D2D1::RectF(L.textX, L.metaTop,
                                  L.textRight, L.metaTop + L.metaHeight),
                      mediaSubTextBrush_.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, false);

    // ── 控制按钮 ──
    const float cy = L.controlsCy;
    auto iconRect = [&](float cx, float half) {
        return D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
    };

    if (hoverButton == 0 && mediaHoverBrush_) {
        renderTarget_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(L.prevCx, cy), L.sideRadius * 1.25f, L.sideRadius * 1.25f),
            mediaHoverBrush_.Get());
    }
    DrawMediaCardIcon(L"\u23EE", iconRect(L.prevCx, L.sideRadius * 0.9f),
                      mediaIconFormat_.Get(), mediaTextBrush_.Get());

    // 播放/暂停：白色实心圆 + 深色图标（与 Win11 媒体卡片一致）
    if (mediaKnobBrush_) {
        renderTarget_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(L.playCx, cy), L.playRadius, L.playRadius),
            mediaKnobBrush_.Get());
    }
    if (hoverButton == 1 && mediaHoverBrush_) {
        renderTarget_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(L.playCx, cy), L.playRadius, L.playRadius),
            mediaHoverBrush_.Get(), 2.0f);
    }
    DrawMediaCardIcon(state.isPlaying ? L"\u23F8" : L"\u25B6",
                      iconRect(L.playCx, L.playRadius * 0.75f),
                      mediaIconFormat_.Get(), mediaIconBrush_.Get());

    if (hoverButton == 2 && mediaHoverBrush_) {
        renderTarget_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(L.nextCx, cy), L.sideRadius * 1.25f, L.sideRadius * 1.25f),
            mediaHoverBrush_.Get());
    }
    DrawMediaCardIcon(L"\u23ED", iconRect(L.nextCx, L.sideRadius * 0.9f),
                      mediaIconFormat_.Get(), mediaTextBrush_.Get());

    // ── 进度条 ──
    const double total = state.duration;
    const double position = (seekPreviewSeconds >= 0.0) ? seekPreviewSeconds : state.currentTime;
    float fraction = 0.0f;
    if (total > 0.0) {
        fraction = static_cast<float>(std::clamp(position / total, 0.0, 1.0));
    }

    const float trackTop = L.seekCy - L.seekThickness * 0.5f;
    const D2D1_ROUNDED_RECT trackRR = D2D1::RoundedRect(
        D2D1::RectF(L.seekX0, trackTop, L.seekX1, trackTop + L.seekThickness),
        L.seekThickness * 0.5f, L.seekThickness * 0.5f);
    renderTarget_->FillRoundedRectangle(trackRR, mediaTrackBrush_.Get());

    const float knobX = L.seekX0 + (L.seekX1 - L.seekX0) * fraction;
    if (fraction > 0.0f && mediaAccentBrush_) {
        const D2D1_ROUNDED_RECT playedRR = D2D1::RoundedRect(
            D2D1::RectF(L.seekX0, trackTop, knobX, trackTop + L.seekThickness),
            L.seekThickness * 0.5f, L.seekThickness * 0.5f);
        renderTarget_->FillRoundedRectangle(playedRR, mediaAccentBrush_.Get());
    }
    if (mediaKnobBrush_) {
        const float knobR = L.knobRadius * (seekDragging ? 1.3f : 1.0f);
        renderTarget_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(knobX, L.seekCy), knobR, knobR),
            mediaKnobBrush_.Get());
    }

    // ── 时间标签 ──
    const std::wstring elapsedW = FormatClock(position);
    const std::wstring totalW = (total > 0.0) ? FormatClock(total) : L"--:--";
    const float timeW = 90.0f * s;
    DrawMediaCardText(elapsedW, mediaTimeFormat_.Get(),
                      D2D1::RectF(L.seekX0, L.timeTop, L.seekX0 + timeW, L.timeTop + L.timeHeight),
                      mediaSubTextBrush_.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, false);
    DrawMediaCardText(totalW, mediaTimeFormat_.Get(),
                      D2D1::RectF(L.seekX1 - timeW, L.timeTop, L.seekX1, L.timeTop + L.timeHeight),
                      mediaSubTextBrush_.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING, false);

    const HRESULT hr = renderTarget_->EndDraw();
    if (SUCCEEDED(hr)) {
        PresentToLayeredWindow();
    } else if (hr == D2DERR_RECREATE_TARGET) {
        CreateRenderTarget();
        RecreateBrushes();
    }
}

} // namespace moekoe
