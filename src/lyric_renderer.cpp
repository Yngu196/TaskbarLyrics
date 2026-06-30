// SPDX-License-Identifier: GPL-3.0
// lyric_renderer.cpp - 卡拉OK歌词渲染（逐字着色、翻译、频谱、悬停控件）
#include "renderer.h"
#include "constants.h"

#include <algorithm>
#include <vector>

namespace moekoe {

void TaskbarRenderer::DrawCentered(const std::wstring& text, ID2D1Brush* brush, float yOffset) {
    if (!renderTarget_ || !textFormat_ || !brush || text.empty()) return;
    
    // 水平偏移量（像素）
    const float paddingX = constants::TEXT_PADDING_X;
    
    D2D1_RECT_F layout = D2D1::RectF(
        paddingX, yOffset,
        static_cast<FLOAT>(width_) - paddingX, yOffset + static_cast<FLOAT>(height_));
    renderTarget_->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()),
        textFormat_.Get(), layout, brush);
}

void TaskbarRenderer::DrawHighlightedTextPerCharacter(const std::wstring& text,
                                                      double progress,
                                                      bool enableKaraoke,
                                                      float scrollOffset,
                                                      const float* overridePaddingX,
                                                      float opacity) {
    if (!renderTarget_ || !textFormat_ || text.empty() ||
        !highlightBrush_ || !normalBrush_) {
        return;
    }
    const UINT32 length = static_cast<UINT32>(text.size());
    if (length == 0) return;

    const float paddingX = overridePaddingX ? *overridePaddingX : constants::TEXT_PADDING_X;
    const float availableWidth = static_cast<FLOAT>(width_) - paddingX * 2.0f;

    D2D1_RECT_F layoutRect = D2D1::RectF(
        paddingX, 0.0f, static_cast<FLOAT>(width_) - paddingX, static_cast<FLOAT>(height_));

    // ── 缓存文本布局：仅在歌词内容变化时重建 CreateTextLayout ──
    bool layoutValid = false;
    DWRITE_TEXT_METRICS metrics{};
    if (cachedLayout_ && text == cachedKaraokeText_) {
        layoutValid = true;
    } else {
        // 文本变化 → 重建布局并缓存
        Microsoft::WRL::ComPtr<IDWriteTextLayout> newLayout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                text.c_str(), length, textFormat_.Get(),
                layoutRect.right - layoutRect.left, static_cast<FLOAT>(height_),
                newLayout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(newLayout->GetMetrics(&m))) {
                cachedLayout_ = newLayout;
                cachedKaraokeText_ = text;
                cachedTextWidth_ = m.width;
                layoutValid = true;
            }
        }
    }

    const float textWidth = layoutValid ? cachedTextWidth_ : 0.0f;

    // ── 判断是否需要跑马灯滚动 ──
    const bool needsMarquee = (layoutValid && textWidth > availableWidth + 1.0f
                               && marqueeState_ != MarqueeState::Idle);

    // P3-①: 传入 opacity<1 时为歌词行切换 fade 过渡，用 PushLayer 整体做透明度
    Microsoft::WRL::ComPtr<ID2D1Layer> fadeLayer;
    bool layerPushed = false;
    if (opacity < 1.0f) {
        // 创建匿名 layer（不指定 geometry → 覆盖整个 render target 区域）
        HRESULT hrLayer = renderTarget_->CreateLayer(nullptr, fadeLayer.GetAddressOf());
        if (SUCCEEDED(hrLayer) && fadeLayer) {
            D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
                D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(),
                opacity, nullptr,
                D2D1_LAYER_OPTIONS_NONE);
            renderTarget_->PushLayer(layerParams, fadeLayer.Get());
            layerPushed = true;
        }
    }

    if (needsMarquee) {
        cachedLayout_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const float textLeft = paddingX - scrollOffset;

        renderTarget_->DrawTextLayout(
            D2D1::Point2F(textLeft, 0.0f), cachedLayout_.Get(), normalBrush_.Get());

        if (enableKaraoke && progress > 0.0) {
            const float highlightWidth = std::min(textWidth * static_cast<float>(progress), textWidth);
            if (highlightWidth > 0.0f) {
                D2D1_RECT_F clipRect = D2D1::RectF(
                    textLeft, 0.0f,
                    textLeft + highlightWidth,
                    static_cast<FLOAT>(height_));
                renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                renderTarget_->DrawTextLayout(
                    D2D1::Point2F(textLeft, 0.0f), cachedLayout_.Get(), highlightBrush_.Get());
                renderTarget_->PopAxisAlignedClip();
            }
        }
    } else {
        // ═══════ 非滚动模式：居中显示 ═══════
        renderTarget_->DrawTextW(
            text.c_str(), length, textFormat_.Get(), layoutRect, normalBrush_.Get());

        if (enableKaraoke && progress > 0.0 && layoutValid) {
            const float highlightWidth = std::min(textWidth * static_cast<float>(progress), textWidth);
            if (highlightWidth > 0.0f) {
                const float centeredLeft = paddingX + (availableWidth - textWidth) / 2.0f;
                D2D1_RECT_F clipRect = D2D1::RectF(
                    centeredLeft, 0.0f,
                    centeredLeft + highlightWidth,
                    static_cast<FLOAT>(height_));
                renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                renderTarget_->DrawTextW(
                    text.c_str(), length, textFormat_.Get(), layoutRect, highlightBrush_.Get());
                renderTarget_->PopAxisAlignedClip();
            }
        }
    }

    if (layerPushed) {
        renderTarget_->PopLayer();
    }
}

void TaskbarRenderer::DrawTranslatedText(const std::wstring& text, const float* overridePaddingX, float opacity) {
    if (!translationFormat_ || !translationBrush_ || text.empty()) return;

    // 水平偏移量（像素）：支持垂直模式下的自定义内边距
    const float paddingX = overridePaddingX ? *overridePaddingX : constants::TEXT_PADDING_X;

    // P3-①: 歌词行切换 fade 过渡期间，旧行翻译通过 opacity<1 渐隐
    if (opacity < 1.0f) {
        translationBrush_->SetOpacity(opacity);
    }
    
    D2D1_RECT_F layout = D2D1::RectF(
        paddingX, static_cast<FLOAT>(height_) * 0.55f,
        static_cast<FLOAT>(width_) - paddingX, static_cast<FLOAT>(height_));
    renderTarget_->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()),
        translationFormat_.Get(),
        layout, translationBrush_.Get());

    if (opacity < 1.0f) {
        translationBrush_->SetOpacity(1.0f); // 恢复默认
    }
}
void TaskbarRenderer::DrawHoverControls(bool isPlaying) {
    if (!renderTarget_ || !normalBrush_) return;

    const FLOAT w = static_cast<FLOAT>(width_);
    const FLOAT h = static_cast<FLOAT>(height_);

    if (isVerticalTaskbar_) {
        // ── 垂直任务栏：按钮垂直堆叠（窄窗口放不下水平排列）──
        const FLOAT btnSize = std::min(w * 0.7f, 28.0f);
        const FLOAT spacing = constants::BUTTON_SPACING;
        const FLOAT totalBtnHeight = btnSize * 3.0f + spacing * 2.0f;
        const FLOAT btnX = (w - btnSize) / 2.0f;
        const FLOAT startY = (h - totalBtnHeight) / 2.0f;

        // 半透明背景（竖条）
        D2D1_RECT_F bgRect = D2D1::RectF(
            btnX - constants::BUTTON_BG_PADDING_X, startY - constants::BUTTON_BG_PADDING_Y,
            btnX + btnSize + constants::BUTTON_BG_PADDING_X, startY + totalBtnHeight + constants::BUTTON_BG_PADDING_Y);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f),
            bgBrush.GetAddressOf());
        if (bgBrush) {
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(bgRect, constants::BUTTON_BG_BORDER_RADIUS, constants::BUTTON_BG_BORDER_RADIUS), bgBrush.Get());
        }

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f),
            iconBrush.GetAddressOf());
        if (!iconBrush || !btnFormat_) return;

        // 上一首 ⏮ (顶部)
        D2D1_RECT_F prevRect = D2D1::RectF(btnX, startY, btnX + btnSize, startY + btnSize);
        renderTarget_->DrawTextW(L"\u23EE", 1, btnFormat_.Get(), prevRect, iconBrush.Get());

        // 暂停/播放 ⏸/▶ (中间)
        FLOAT ppY = startY + btnSize + spacing;
        D2D1_RECT_F ppRect = D2D1::RectF(btnX, ppY, btnX + btnSize, ppY + btnSize);
        renderTarget_->DrawTextW(isPlaying ? L"\u23F8" : L"\u25B6", 1, btnFormat_.Get(), ppRect, iconBrush.Get());

        // 下一首 ⏭ (底部)
        FLOAT nextY = startY + (btnSize + spacing) * 2.0f;
        D2D1_RECT_F nextRect = D2D1::RectF(btnX, nextY, btnX + btnSize, nextY + btnSize);
        renderTarget_->DrawTextW(L"\u23ED", 1, btnFormat_.Get(), nextRect, iconBrush.Get());
    } else {
        // ── 水平任务栏：按钮水平排列（原有逻辑）──
        const FLOAT btnSize = h * 0.7f;
        const FLOAT spacing = constants::BUTTON_SPACING;
        const FLOAT totalBtnWidth = btnSize * 3.0f + spacing * 2.0f;
        const FLOAT startX = (w - totalBtnWidth) / 2.0f;
        const FLOAT btnY = (h - btnSize) / 2.0f;

        // 半透明背景
        D2D1_RECT_F bgRect = D2D1::RectF(
            startX - constants::BUTTON_BG_PADDING_X, btnY - constants::BUTTON_BG_PADDING_Y,
            startX + totalBtnWidth + constants::BUTTON_BG_PADDING_X, btnY + btnSize + constants::BUTTON_BG_PADDING_Y);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f),
            bgBrush.GetAddressOf());
        if (bgBrush) {
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(bgRect, constants::BUTTON_BG_BORDER_RADIUS, constants::BUTTON_BG_BORDER_RADIUS), bgBrush.Get());
        }

        // 按钮符号颜色
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f),
            iconBrush.GetAddressOf());
        if (!iconBrush || !btnFormat_) return;

        // 上一首 ⏮ (U+23EE)
        D2D1_RECT_F prevRect = D2D1::RectF(startX, btnY, startX + btnSize, btnY + btnSize);
        renderTarget_->DrawTextW(L"\u23EE", 1, btnFormat_.Get(), prevRect, iconBrush.Get());

        // 暂停/播放 ⏸ (U+23F8) / ▶ (U+25B6)
        FLOAT ppX = startX + btnSize + spacing;
        D2D1_RECT_F ppRect = D2D1::RectF(ppX, btnY, ppX + btnSize, btnY + btnSize);
        renderTarget_->DrawTextW(isPlaying ? L"\u23F8" : L"\u25B6", 1, btnFormat_.Get(), ppRect, iconBrush.Get());

        // 下一首 ⏭ (U+23ED)
        FLOAT nextX = startX + (btnSize + spacing) * 2.0f;
        D2D1_RECT_F nextRect = D2D1::RectF(nextX, btnY, nextX + btnSize, btnY + btnSize);
        renderTarget_->DrawTextW(L"\u23ED", 1, btnFormat_.Get(), nextRect, iconBrush.Get());
    }
}

// ═════════════════════════════════════════
// 卡片模式渲染（无卡拉OK效果）
// ═════════════════════════════════════════

void TaskbarRenderer::DrawSpectrumBars(const std::vector<float>& bands, float x, float width, float y, float height, float alpha) {
    if (bands.empty() || !renderTarget_) return;

    const size_t n = bands.size();
    const float totalGap = constants::SPECTRUM_BAR_GAP * (static_cast<float>(n) - 1);
    const float availW = width;
    const float barWidth = (std::max)(2.0f, (availW - totalGap) / static_cast<float>(n));
    const float step = barWidth + constants::SPECTRUM_BAR_GAP;
    const float startX = x;

    for (size_t i = 0; i < n; ++i) {
        float barH = bands[i] * height;
        if (barH < constants::SPECTRUM_BAR_MIN_HEIGHT) barH = constants::SPECTRUM_BAR_MIN_HEIGHT;
        const float barX = startX + static_cast<float>(i) * step;
        const float barY = y + height - barH;

        D2D1_RECT_F rect = D2D1::RectF(barX, barY, barX + barWidth, barY + barH);
        spectrumBrush_->SetOpacity(alpha * (0.12f + bands[i] * 0.88f));
        renderTarget_->FillRectangle(rect, spectrumBrush_.Get());
    }
}

// ═════ P1-②: 封面 fade-in 过渡动画 ═════
// 使用 ease-out cubic 缓动：1 - (1 - t)^3。
// 350ms 内 coverFadeAlpha_ 从 0→1，后续帧跳过不计算。
// 返回 true 表示动画进行中（调用方可据此触发额外重绘）。

} // namespace moekoe
