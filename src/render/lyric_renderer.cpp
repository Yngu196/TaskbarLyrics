// SPDX-License-Identifier: GPL-3.0
// lyric_renderer.cpp - 卡拉OK歌词渲染（逐字着色、翻译、频谱、悬停控件）
#include "render/renderer.h"
#include "core/constants.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace moekoe {

void TaskbarRenderer::DrawCentered(const std::wstring& text, ID2D1Brush* brush, float yOffset) {
    if (!renderTarget_ || !textFormat_ || !brush || text.empty()) return;
    
    // 水平偏移量（像素）
    const float paddingX = constants::TEXT_PADDING_X * static_cast<float>(dpi_) / 96.0f;
    
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
                                                      const float* overridePaddingLeft,
                                                      float opacity,
                                                      const float* overridePaddingRight,
                                                      const D2D1_RECT_F* overrideLayout) {
    if (!renderTarget_ || !textFormat_ || text.empty() ||
        !highlightBrush_ || !normalBrush_) {
        return;
    }
    const UINT32 length = static_cast<UINT32>(text.size());
    if (length == 0) return;

    const float paddingX = overridePaddingLeft ? *overridePaddingLeft
        : constants::TEXT_PADDING_X * static_cast<float>(dpi_) / 96.0f;
    const float rightPaddingX = overridePaddingRight ? *overridePaddingRight : paddingX;
    const float defaultAvailableWidth = static_cast<FLOAT>(width_) - paddingX - rightPaddingX;
    // 应用用户设置的垂直偏移（dp → px）
    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float userOffsetY = static_cast<float>(settings_.lyricOffsetY) * dpiScale;

    D2D1_RECT_F layoutRect = overrideLayout ? *overrideLayout : D2D1::RectF(
        paddingX, 0.0f,
        static_cast<FLOAT>(width_) - rightPaddingX,
        static_cast<FLOAT>(height_));
    layoutRect.top += userOffsetY;
    layoutRect.bottom += userOffsetY;
    const float availableWidth = overrideLayout
        ? layoutRect.right - layoutRect.left : defaultAvailableWidth;

    // ── 缓存文本布局：仅在歌词内容变化时重建 CreateTextLayout ──
    bool layoutValid = false;
    DWRITE_TEXT_METRICS metrics{};
    const float layoutHeight = layoutRect.bottom - layoutRect.top;
    if (cachedLayout_ && text == cachedKaraokeText_ &&
        std::abs(cachedLayoutWidth_ - availableWidth) < 0.5f &&
        std::abs(cachedLayoutHeight_ - layoutHeight) < 0.5f) {
        layoutValid = true;
    } else {
        // 文本变化 → 重建布局并缓存
        Microsoft::WRL::ComPtr<IDWriteTextLayout> newLayout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                text.c_str(), length, textFormat_.Get(),
                availableWidth, layoutHeight,
                newLayout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(newLayout->GetMetrics(&m))) {
                cachedLayout_ = newLayout;
                cachedKaraokeText_ = text;
                cachedTextWidth_ = m.width;
                cachedLayoutWidth_ = availableWidth;
                cachedLayoutHeight_ = layoutHeight;
                layoutValid = true;
            }
        }
    }

    const float textWidth = layoutValid ? cachedTextWidth_ : 0.0f;

    // 垂直任务栏的歌词采用逐字上下排布，不能再把窄栏当成横向跑马灯区域。
    const bool verticalText = isVerticalTaskbar_ && overrideLayout;

    // ── 判断是否需要跑马灯滚动 ──
    const bool needsMarquee = (!verticalText && layoutValid && textWidth > availableWidth + 1.0f
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

    if (verticalText) {
        // DirectWrite 的常规文本布局仍按左到右书写；在左右侧任务栏中，
        // 逐字绘制才能确保歌词从上到下排列，并且与高亮进度保持一致。
        const float glyphHeight = std::max(8.0f,
            static_cast<float>(settings_.fontSize) * dpiScale * 1.15f);
        const float totalHeight = glyphHeight * static_cast<float>(length);
        float glyphY = layoutRect.top - scrollOffset;
        if (totalHeight < layoutHeight) {
            glyphY = layoutRect.top + (layoutHeight - totalHeight) * 0.5f;
        }

        renderTarget_->PushAxisAlignedClip(layoutRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // 先完整绘制普通文字，再叠加一个连续的高亮裁剪区。不能按 ceil(progress * length)
        // 直接换整字颜色，否则侧栏纵排文字会在每个字的起点突然跳亮。
        for (UINT32 i = 0; i < length; ++i) {
            const D2D1_RECT_F glyphRect = D2D1::RectF(
                layoutRect.left, glyphY + glyphHeight * static_cast<float>(i),
                layoutRect.right, glyphY + glyphHeight * static_cast<float>(i + 1));
            renderTarget_->DrawTextW(&text[i], 1, textFormat_.Get(), glyphRect, normalBrush_.Get());
        }

        if (enableKaraoke && progress > 0.0) {
            const float highlightBottom = glyphY + totalHeight * static_cast<float>(std::clamp(progress, 0.0, 1.0));
            const UINT32 highlightGlyphCount = static_cast<UINT32>(std::ceil(
                std::clamp(progress, 0.0, 1.0) * length));
            if (highlightGlyphCount > 0) {
                renderTarget_->PushAxisAlignedClip(
                    D2D1::RectF(layoutRect.left, glyphY, layoutRect.right, highlightBottom),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                for (UINT32 i = 0; i < highlightGlyphCount; ++i) {
                    const D2D1_RECT_F glyphRect = D2D1::RectF(
                        layoutRect.left, glyphY + glyphHeight * static_cast<float>(i),
                        layoutRect.right, glyphY + glyphHeight * static_cast<float>(i + 1));
                    renderTarget_->DrawTextW(&text[i], 1, textFormat_.Get(), glyphRect, highlightBrush_.Get());
                }
                renderTarget_->PopAxisAlignedClip();
            }
        }
        renderTarget_->PopAxisAlignedClip();
    } else if (needsMarquee) {
        cachedLayout_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const float textLeft = layoutRect.left - scrollOffset;

        // 跑马灯文本会向左移动，必须裁剪到实际歌词区域；否则有封面时，
        // 滚动后的文字会穿透到封面左侧/下方（封面并不能可靠遮住透明区域）。
        renderTarget_->PushAxisAlignedClip(layoutRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        renderTarget_->DrawTextLayout(
            D2D1::Point2F(textLeft, layoutRect.top), cachedLayout_.Get(), normalBrush_.Get());

        if (enableKaraoke && progress > 0.0) {
            const float highlightWidth = std::min(textWidth * static_cast<float>(progress), textWidth);
            if (highlightWidth > 0.0f) {
                D2D1_RECT_F clipRect = D2D1::RectF(
                    textLeft, layoutRect.top,
                    textLeft + highlightWidth,
                    layoutRect.bottom);
                renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                renderTarget_->DrawTextLayout(
                    D2D1::Point2F(textLeft, layoutRect.top), cachedLayout_.Get(), highlightBrush_.Get());
                renderTarget_->PopAxisAlignedClip();
            }
        }
        renderTarget_->PopAxisAlignedClip();
    } else {
        // ═══════ 非滚动模式：居中显示 ═══════
        // 裁剪到 layoutRect，避免长歌词文本溢出覆盖封面
        renderTarget_->PushAxisAlignedClip(layoutRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        renderTarget_->DrawTextW(
            text.c_str(), length, textFormat_.Get(), layoutRect, normalBrush_.Get());

        if (enableKaraoke && progress > 0.0 && layoutValid) {
            const float highlightWidth = std::min(textWidth * static_cast<float>(progress), textWidth);
            if (highlightWidth > 0.0f) {
                const float centeredLeft = layoutRect.left + (availableWidth - textWidth) / 2.0f;
                // ── 硬切换：瞬间颜色切换 ──
                D2D1_RECT_F clipRect = D2D1::RectF(
                    centeredLeft, layoutRect.top,
                    centeredLeft + highlightWidth,
                    layoutRect.bottom);
                renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                renderTarget_->DrawTextW(
                    text.c_str(), length, textFormat_.Get(), layoutRect, highlightBrush_.Get());
                renderTarget_->PopAxisAlignedClip();
            }
        }
        renderTarget_->PopAxisAlignedClip();
    }

    if (layerPushed) {
        renderTarget_->PopLayer();
    }
}

void TaskbarRenderer::DrawTranslatedText(const std::wstring& text,
                                         const float* overridePaddingLeft,
                                         float opacity,
                                         const float* overridePaddingRight,
                                         const D2D1_RECT_F* overrideLayout) {
    if (!translationFormat_ || !translationBrush_ || text.empty()) return;

    // 水平偏移量（像素）：支持垂直模式下的自定义内边距
    const float paddingX = overridePaddingLeft ? *overridePaddingLeft
        : constants::TEXT_PADDING_X * static_cast<float>(dpi_) / 96.0f;
    const float rightPaddingX = overridePaddingRight ? *overridePaddingRight : paddingX;
    // 应用用户设置的垂直偏移
    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float userOffsetY = static_cast<float>(settings_.lyricOffsetY) * dpiScale;

    // P3-①: 歌词行切换 fade 过渡期间，旧行翻译通过 opacity<1 渐隐
    if (opacity < 1.0f) {
        translationBrush_->SetOpacity(opacity);
    }
    
    D2D1_RECT_F layout = overrideLayout ? *overrideLayout : D2D1::RectF(
        paddingX, static_cast<FLOAT>(height_) * 0.55f,
        static_cast<FLOAT>(width_) - rightPaddingX,
        static_cast<FLOAT>(height_));
    layout.top += userOffsetY;
    layout.bottom += userOffsetY;
    if (isVerticalTaskbar_ && overrideLayout) {
        const float glyphHeight = std::max(8.0f,
            (static_cast<float>(settings_.fontSize) - constants::TRANSLATION_FONT_SIZE_DELTA) * dpiScale * 1.15f);
        const float totalHeight = glyphHeight * static_cast<float>(text.size());
        float glyphY = layout.top;
        if (totalHeight < layout.bottom - layout.top) {
            glyphY += ((layout.bottom - layout.top) - totalHeight) * 0.5f;
        }
        renderTarget_->PushAxisAlignedClip(layout, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < text.size(); ++i) {
            const D2D1_RECT_F glyphRect = D2D1::RectF(
                layout.left, glyphY + glyphHeight * static_cast<float>(i),
                layout.right, glyphY + glyphHeight * static_cast<float>(i + 1));
            renderTarget_->DrawTextW(&text[i], 1, translationFormat_.Get(), glyphRect, translationBrush_.Get());
        }
        renderTarget_->PopAxisAlignedClip();
    } else {
        renderTarget_->DrawTextW(
            text.c_str(), static_cast<UINT32>(text.size()),
            translationFormat_.Get(),
            layout, translationBrush_.Get());
    }

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
        const FLOAT dpiScale = static_cast<FLOAT>(dpi_) / 96.0f;
        const FLOAT spacing = constants::BUTTON_SPACING * dpiScale;
        const FLOAT totalBtnHeight = btnSize * 3.0f + spacing * 2.0f;
        const FLOAT btnX = (w - btnSize) / 2.0f;
        const FLOAT startY = (h - totalBtnHeight) / 2.0f;

        // 半透明背景（竖条）
        D2D1_RECT_F bgRect = D2D1::RectF(
            btnX - constants::BUTTON_BG_PADDING_X * dpiScale, startY - constants::BUTTON_BG_PADDING_Y * dpiScale,
            btnX + btnSize + constants::BUTTON_BG_PADDING_X * dpiScale, startY + totalBtnHeight + constants::BUTTON_BG_PADDING_Y * dpiScale);
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
        if (!iconBrush || !dwriteFactory_) return;

        // btnFormat_ 的字号按窗口高度生成；对于纵向窗口会远大于实际按钮。
        // 单独创建按按钮尺寸缩放的格式，保证三个图标清晰且互不重叠。
        Microsoft::WRL::ComPtr<IDWriteTextFormat> verticalBtnFormat;
        dwriteFactory_->CreateTextFormat(
            L"Segoe UI Symbol", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            std::max(8.0f, btnSize * 0.66f), L"en-US", verticalBtnFormat.GetAddressOf());
        if (!verticalBtnFormat) return;
        verticalBtnFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        verticalBtnFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // 上一首 ⏮ (顶部)
        D2D1_RECT_F prevRect = D2D1::RectF(btnX, startY, btnX + btnSize, startY + btnSize);
        renderTarget_->DrawTextW(L"\u23EE", 1, verticalBtnFormat.Get(), prevRect, iconBrush.Get());

        // 暂停/播放 ⏸/▶ (中间)
        FLOAT ppY = startY + btnSize + spacing;
        D2D1_RECT_F ppRect = D2D1::RectF(btnX, ppY, btnX + btnSize, ppY + btnSize);
        renderTarget_->DrawTextW(isPlaying ? L"\u23F8" : L"\u25B6", 1, verticalBtnFormat.Get(), ppRect, iconBrush.Get());

        // 下一首 ⏭ (底部)
        FLOAT nextY = startY + (btnSize + spacing) * 2.0f;
        D2D1_RECT_F nextRect = D2D1::RectF(btnX, nextY, btnX + btnSize, nextY + btnSize);
        renderTarget_->DrawTextW(L"\u23ED", 1, verticalBtnFormat.Get(), nextRect, iconBrush.Get());
    } else {
        // ── 水平任务栏：按钮水平排列（原有逻辑）──
        const FLOAT btnSize = h * 0.7f;
        const FLOAT dpiScale = static_cast<FLOAT>(dpi_) / 96.0f;
        const FLOAT spacing = constants::BUTTON_SPACING * dpiScale;
        const FLOAT totalBtnWidth = btnSize * 3.0f + spacing * 2.0f;
        const FLOAT startX = (w - totalBtnWidth) / 2.0f;
        const FLOAT btnY = (h - btnSize) / 2.0f;

        // 半透明背景
        D2D1_RECT_F bgRect = D2D1::RectF(
            startX - constants::BUTTON_BG_PADDING_X * dpiScale, btnY - constants::BUTTON_BG_PADDING_Y * dpiScale,
            startX + totalBtnWidth + constants::BUTTON_BG_PADDING_X * dpiScale, btnY + btnSize + constants::BUTTON_BG_PADDING_Y * dpiScale);
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
    if (bands.empty() || !renderTarget_ || !spectrumBrush_) return;

    const size_t n = bands.size();
    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float gap = std::min(constants::SPECTRUM_BAR_GAP * dpiScale,
                               n > 1 ? width / static_cast<float>(n - 1) : 0.0f);
    const float totalGap = gap * (static_cast<float>(n) - 1);
    // 柱宽：配置值 > 0 时使用固定宽度，否则自动计算
    const float autoWidth = (std::max)(3.0f, (width - totalGap) / static_cast<float>(n));
    const float maxBarWidth = std::max(1.0f,
        (width - totalGap) / static_cast<float>(n));
    const float barWidth = (settings_.spectrumBarWidth > 0.5f)
        ? std::min(settings_.spectrumBarWidth * dpiScale, maxBarWidth)
        : std::min(autoWidth, maxBarWidth);
    const float step = barWidth + gap;
    const float radius = barWidth * 0.5f;
    const float centerY = y + height * 0.5f;

    // 双层光晕：外层扩散大、透明度低；内层紧贴条身
    const float glowInner = 1.5f;
    const float glowOuter = 3.0f;

    // 居中：计算所有柱子的总宽度，在给定区域内居中排列
    const float totalBarsWidth = static_cast<float>(n) * barWidth + static_cast<float>(n - 1) * gap;
    const float startX = x + (width - totalBarsWidth) * 0.5f;

    for (size_t i = 0; i < n; ++i) {
        // 最低高度 = 条宽 → 安静频段显示为圆点（胶囊短到极限即圆形）
        const float barH = (std::max)(barWidth, bands[i] * height);
        const float barX = startX + static_cast<float>(i) * step;
        const float barY = centerY - barH * 0.5f;

        // 外层光晕
        D2D1_ROUNDED_RECT rrOuter = D2D1::RoundedRect(
            D2D1::RectF(barX - glowOuter, barY - glowOuter,
                        barX + barWidth + glowOuter, barY + barH + glowOuter),
            radius + glowOuter, radius + glowOuter);
        spectrumBrush_->SetOpacity(alpha * 0.06f);
        renderTarget_->FillRoundedRectangle(rrOuter, spectrumBrush_.Get());

        // 内层光晕
        D2D1_ROUNDED_RECT rrInner = D2D1::RoundedRect(
            D2D1::RectF(barX - glowInner, barY - glowInner,
                        barX + barWidth + glowInner, barY + barH + glowInner),
            radius + glowInner, radius + glowInner);
        spectrumBrush_->SetOpacity(alpha * 0.14f);
        renderTarget_->FillRoundedRectangle(rrInner, spectrumBrush_.Get());

        // 主胶囊条：能量越高越不透明
        D2D1_ROUNDED_RECT barRR = D2D1::RoundedRect(
            D2D1::RectF(barX, barY, barX + barWidth, barY + barH),
            radius, radius);
        spectrumBrush_->SetOpacity(alpha * (0.80f + bands[i] * 0.20f));
        renderTarget_->FillRoundedRectangle(barRR, spectrumBrush_.Get());
    }
    spectrumBrush_->SetOpacity(1.0f);
}

// ═════ P1-②: 封面 fade-in 过渡动画 ═════
// 使用 ease-out cubic 缓动：1 - (1 - t)^3。
// 350ms 内 coverFadeAlpha_ 从 0→1，后续帧跳过不计算。
// 返回 true 表示动画进行中（调用方可据此触发额外重绘）。

} // namespace moekoe
