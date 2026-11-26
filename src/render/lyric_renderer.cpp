// SPDX-License-Identifier: Apache-2.0
// lyric_renderer.cpp - 卡拉OK歌词渲染（逐字着色、翻译、频谱、悬停控件）
#include "render/renderer.h"
#include "render/renderer_utils.h"
#include "core/constants.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace moekoe {

using renderer_utils::GetCurrentTimeSeconds;

// ═══════════════════════════════════════
// 跟唱流光：当前演唱行卡拉OK进度前沿的流动光晕
// ═══════════════════════════════════════

bool TaskbarRenderer::SingGlowActive(bool enableKaraoke, bool useHighlightColor,
                                     double progress, bool layoutValid) const {
    // 仅当前演唱行（useHighlightColor=false 表示非悬停歌曲信息场景），
    // 进度位于行中（>0 且 <1）时才绘制前沿光带
    return settings_.enableSingAlongGlow && singGlowBrush_ &&
           !useHighlightColor && enableKaraoke && layoutValid &&
           progress > 0.0 && progress < 1.0;
}

void TaskbarRenderer::CreateSingGlowGradient(const D2D1_COLOR_F& base,
                                             ID2D1LinearGradientBrush** out) {
    if (!renderTarget_ || !out) return;
    // 亮核：基准色向白色提亮 85%，模拟发光
    const D2D1_COLOR_F bright{
        base.r + (1.0f - base.r) * 0.85f,
        base.g + (1.0f - base.g) * 0.85f,
        base.b + (1.0f - base.b) * 0.85f, 1.0f};
    // 暗带：亮度压到 30%，形成亮核两侧的影子夹层
    const D2D1_COLOR_F dark{base.r * 0.30f, base.g * 0.30f, base.b * 0.30f, 0.85f};
    const D2D1_GRADIENT_STOP stops[5] = {
        {0.0f,  {base.r,  base.g,  base.b,  0.0f}},
        {0.25f, dark.r,   dark.g,   dark.b,   dark.a},
        {0.5f,  bright.r, bright.g, bright.b, bright.a},
        {0.75f, dark.r,   dark.g,   dark.b,   dark.a},
        {1.0f,  {base.r,  base.g,  base.b,  0.0f}},
    };
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(renderTarget_->CreateGradientStopCollection(
            stops, 5, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            stopCollection.GetAddressOf())) && stopCollection) {
        // d2d1.h 内联重载：(const props&, 停止点集合, 输出画刷)
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgbp =
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(1.0f, 0.0f));
        renderTarget_->CreateLinearGradientBrush(lgbp, stopCollection.Get(), out);
    }
}

bool TaskbarRenderer::SetupSingGlowBrush(float front, bool vertical, float fontSizeOverride,
                                         ID2D1LinearGradientBrush* brush) {
    ID2D1LinearGradientBrush* b = brush ? brush : singGlowBrush_.Get();
    if (!b) return false;
    // 对称窄带（参考 taskbar-lyrics-displayer）：半宽 = 字号×0.65，夹紧到 [7,18] px，
    // 亮核居中于进度前沿，两侧对称展开暗带
    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float baseFont = fontSizeOverride > 0.0f ? fontSizeOverride
                                                   : static_cast<float>(settings_.fontSize);
    const float fontPx = std::max(8.0f, baseFont) * dpiScale;
    const float halfWidth = std::clamp(fontPx * 0.65f, 7.0f, 18.0f);
    if (vertical) {
        b->SetStartPoint(D2D1::Point2F(0.0f, front - halfWidth));
        b->SetEndPoint(D2D1::Point2F(0.0f, front + halfWidth));
    } else {
        b->SetStartPoint(D2D1::Point2F(front - halfWidth, 0.0f));
        b->SetEndPoint(D2D1::Point2F(front + halfWidth, 0.0f));
    }
    return true;
}

// ═══════════════════════════════════════
// 星尘跟唱：五角星沿字形顶边随逐字进度跳跃旋转 + 流星粒子拖尾
// （移植自 taskbar-lyrics-displayer 的 LyricEffect::Stardust）
// ═══════════════════════════════════════

void TaskbarRenderer::DrawStardust(const std::wstring& text, const D2D1_RECT_F& lineRect,
                                   IDWriteTextLayout* layout, float textWidth, float textLeft,
                                   double progress, double charProgress, double songPositionMs,
                                   bool playing, float opacity) {
    if (!renderTarget_ || !stardustBrush_ || !layout || textWidth <= 0.0f || opacity <= 0.001f) {
        return;
    }
    constexpr float pi = 3.14159265358979323846f;
    const double now = GetCurrentTimeSeconds();

    // ── 暂停淡出：暂停后星星 300ms 内淡出，拖尾随寿命自然消散 ──
    if (playing) {
        stardustPauseStart_ = 0.0;
    } else if (stardustPauseStart_ == 0.0) {
        stardustPauseStart_ = now;
    }
    const float liveAlpha = playing
        ? 1.0f
        : static_cast<float>(std::max(0.0, 1.0 - (now - stardustPauseStart_) / 0.3));
    if (liveAlpha <= 0.0f) return;

    // 行文本变化 → 清空轨迹（旧行拖尾不跨行延续）
    if (text != stardustLine_) {
        stardustLine_ = text;
        stardustTrailHead_ = 0;
        stardustTrailCount_ = 0;
    }

    // ── 星星参数（与参考实现同式，单位 DIP） ──
    const float radius = std::clamp(static_cast<float>(settings_.fontSize) * 0.40f, 5.5f, 8.5f);
    const float gap = 1.25f + (settings_.enableSingAlongGlow ? 1.0f : 0.0f);
    const float inset = std::min(radius, textWidth * 0.5f);
    const float clamped = static_cast<float>(std::clamp(progress, 0.0, 1.0));
    const float along = std::clamp(clamped * textWidth, inset, textWidth - inset);
    // 跳跃：当前字符内进度推进时星星上抛再落下（每字一跳）
    const float hop = std::sin(pi * static_cast<float>(std::clamp(charProgress, 0.0, 1.0))) * 3.0f;

    // ── 锚点：命中测试最近字框，取字形顶边（参考实现取 token 字框顶边） ──
    const float layoutHeight = lineRect.bottom - lineRect.top;
    DWRITE_HIT_TEST_METRICS hit{};
    BOOL trailing = FALSE, inside = FALSE;
    float glyphTop = 0.0f;
    if (SUCCEEDED(layout->HitTestPoint(along, layoutHeight * 0.5f,
                                       &trailing, &inside, &hit)) &&
        hit.width > 0.0f && hit.height > 0.0f) {
        glyphTop = hit.top;
    }
    const float starX = textLeft + along;
    const float starY = lineRect.top + glyphTop - gap - radius - hop;

    // ── 记录轨迹采样（渲染线程每帧一条，供拖尾回溯） ──
    stardustTrail_[stardustTrailHead_] = {now, starX, starY};
    stardustTrailHead_ = (stardustTrailHead_ + 1) % kStardustTrailSize;
    stardustTrailCount_ = std::min(stardustTrailCount_ + 1, kStardustTrailSize);
    auto findSample = [&](double targetTime) -> const StardustSample* {
        const StardustSample* best = nullptr;
        double bestDiff = 1e9;
        for (int i = 0; i < stardustTrailCount_; ++i) {
            const int slot = (stardustTrailHead_ - 1 - i + kStardustTrailSize * 2) %
                             kStardustTrailSize;
            const double diff = std::abs(stardustTrail_[slot].time - targetTime);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = &stardustTrail_[slot];
            }
        }
        return best;
    };

    // ── 画刷颜色：高亮歌词色（复用画刷，逐粒子改 alpha，结束恢复） ──
    const D2D1_COLOR_F baseColor = stardustBrush_->GetColor();
    auto color = [&](float alpha) {
        D2D1_COLOR_F c = baseColor;
        c.a = std::clamp(alpha * opacity * liveAlpha, 0.0f, 1.0f);
        stardustBrush_->SetColor(c);
    };

    // ── 流星拖尾：每 24ms 一粒，寿命 432ms；越老越向后拉、横向散开 ──
    // 速度/散开方向由时间槽哈希决定，粒子轨迹逐帧稳定不抖动
    const uint32_t lineHash = static_cast<uint32_t>(std::hash<std::wstring>{}(text) & 0xFFFFFFFFu);
    for (int i = 17; i >= 1; --i) {
        const double sampleTime = now - static_cast<double>(i) * 0.024;
        const StardustSample* sample = findSample(sampleTime);
        if (!sample) continue;
        const double ageD = (now - sample->time) / 0.432;
        if (ageD <= 0.0 || ageD >= 1.0) continue;
        const float age = static_cast<float>(ageD);
        // 路径方向：取相邻较旧样本连线（指向行进方向）
        const StardustSample* older = findSample(sample->time - 0.024);
        float dirX = 1.0f, dirY = 0.0f;
        if (older) {
            const float dx = sample->x - older->x;
            const float dy = sample->y - older->y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.05f) {
                dirX = dx / len;
                dirY = dy / len;
            }
        }
        // 时间槽哈希 → 散开侧与速度（与参考实现的 seed 派生同构）
        uint32_t seed = static_cast<uint32_t>(static_cast<int64_t>(sampleTime / 0.024)) ^
                        (lineHash + 1u) * 0x9e3779b9u;
        seed ^= seed >> 16;
        seed *= 0x7feb352du;
        seed ^= seed >> 15;
        const float side = static_cast<float>(seed & 1023u) / 1023.0f;
        const float speed = static_cast<float>((seed >> 10) & 1023u) / 1023.0f;
        // 越老越向后、向外散开
        const float lag = age * (radius + 9.0f * speed);
        const float scatter = (side * 12.0f - 7.5f) * age;
        const float dotX = sample->x - dirX * lag - dirY * scatter;
        const float dotY = sample->y - dirY * lag + dirX * scatter;
        // 离星星太近的粒子跳过，保持星星周围留白
        const float ddx = dotX - starX;
        const float ddy = dotY - starY;
        if (ddx * ddx + ddy * ddy < radius * radius * 0.49f) continue;
        const float fade = 1.0f - age;
        const float dotRadius = 0.55f + fade * (0.8f + speed * 0.6f);
        color(fade * fade * 0.85f);
        renderTarget_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotRadius, dotRadius),
            stardustBrush_.Get());
    }

    // ── 五角星本体：单位路径只建一次，旋转/缩放/平移全走绘制矩阵 ──
    if (!starGeometry_) {
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        renderTarget_->GetFactory(factory.GetAddressOf());
        if (factory && SUCCEEDED(factory->CreatePathGeometry(starGeometry_.GetAddressOf()))) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(starGeometry_->Open(sink.GetAddressOf()))) {
                for (int k = 0; k < 10; ++k) {
                    const float a = -pi * 0.5f + k * pi * 0.2f;
                    const float r = (k % 2 == 0) ? 1.0f : 0.45f;
                    const auto p = D2D1::Point2F(std::cos(a) * r, std::sin(a) * r);
                    if (k == 0) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
                    else sink->AddLine(p);
                }
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (FAILED(sink->Close())) starGeometry_.Reset();
            } else {
                starGeometry_.Reset();
            }
        }
    }
    if (starGeometry_) {
        color(1.0f);
        // 角度只依赖播放位置：暂停冻结，seek 直接对齐，不积累动画状态
        const float angle = static_cast<float>(
            std::fmod(std::max(0.0, songPositionMs), 3000.0)) / 3000.0f * 2.0f * pi;
        D2D1_MATRIX_3X2_F previous{};
        renderTarget_->GetTransform(&previous);
        const float c = std::cos(angle) * radius;
        const float s = std::sin(angle) * radius;
        renderTarget_->SetTransform(
            D2D1::Matrix3x2F(c, s, -s, c, starX, starY) * previous);
        renderTarget_->FillGeometry(starGeometry_.Get(), stardustBrush_.Get());
        renderTarget_->SetTransform(previous);
    }
    stardustBrush_->SetColor(baseColor);
}

void TaskbarRenderer::DrawHighlightedTextPerCharacter(const std::wstring& text,
                                                      double progress,
                                                      bool enableKaraoke,
                                                      float scrollOffset,
                                                      const float* overridePaddingLeft,
                                                      float opacity,
                                                      const float* overridePaddingRight,
                                                      const D2D1_RECT_F* overrideLayout,
                                                      bool useHighlightColor,
                                                      double charProgress,
                                                      double songPositionMs,
                                                      bool isPlaying,
                                                      bool allowStardust) {
    if (!renderTarget_ || !textFormat_ || text.empty() ||
        !highlightBrush_ || !normalBrush_) {
        return;
    }
    const UINT32 length = static_cast<UINT32>(text.size());
    if (length == 0) return;

    // 悬停歌曲信息：整段文字直接使用高亮色绘制
    ID2D1Brush* baseBrush = useHighlightColor ? highlightBrush_.Get() : normalBrush_.Get();

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
                cachedTextHeight_ = m.height;
                cachedLayoutWidth_ = availableWidth;
                cachedLayoutHeight_ = layoutHeight;
                layoutValid = true;
            }
        }
    }

    const float textWidth = layoutValid ? cachedTextWidth_ : 0.0f;

    // 垂直任务栏的歌词采用逐字上下排布，不能再把窄栏当成横向跑马灯区域。
    const bool verticalText = isVerticalTaskbar_ && overrideLayout;

    // ── 星尘跟唱：仅单行水平逐字歌词，与翻译显示/悬停歌曲信息互斥 ──
    // （allowStardust 由 Render() 按翻译显示状态传入）
    const bool stardust = allowStardust && settings_.enableStardust &&
                          !useHighlightColor && enableKaraoke && layoutValid &&
                          !verticalText && progress > 0.0 && progress < 1.0;
    // 歌词下沉：给顶边的星星留出活动空间，最多 8dp（参考实现同式）
    float stardustShift = 0.0f;
    float starPad = 0.0f;
    if (stardust) {
        const float starRadius = std::clamp(static_cast<float>(settings_.fontSize) * 0.40f,
                                            5.5f, 8.5f);
        starPad = starRadius * 2.0f + 10.0f;
        const float textHeight = cachedTextHeight_ > 0.0f
            ? cachedTextHeight_
            : std::max(8.0f, static_cast<float>(settings_.fontSize)) * dpiScale * 1.4f;
        stardustShift = std::min(8.0f,
            std::max(0.0f, (static_cast<float>(height_) - textHeight) * 0.5f - 2.0f));
        layoutRect.top += stardustShift;
        layoutRect.bottom += stardustShift;
    }

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
            renderTarget_->DrawTextW(&text[i], 1, textFormat_.Get(), glyphRect, baseBrush);
        }

        if (!useHighlightColor && enableKaraoke && progress > 0.0) {
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

        // 跟唱流光：进度前沿的垂直光带（外层 layoutRect 裁剪仍在生效）
        if (SingGlowActive(enableKaraoke, useHighlightColor, progress, true)) {
            const float front = glyphY + totalHeight * static_cast<float>(std::clamp(progress, 0.0, 1.0));
            if (SetupSingGlowBrush(front, true)) {
                for (UINT32 i = 0; i < length; ++i) {
                    const D2D1_RECT_F glyphRect = D2D1::RectF(
                        layoutRect.left, glyphY + glyphHeight * static_cast<float>(i),
                        layoutRect.right, glyphY + glyphHeight * static_cast<float>(i + 1));
                    renderTarget_->DrawTextW(&text[i], 1, textFormat_.Get(), glyphRect, singGlowBrush_.Get());
                }
            }
        }
        renderTarget_->PopAxisAlignedClip();
    } else if (needsMarquee) {
        cachedLayout_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const float textLeft = layoutRect.left - scrollOffset;

        // 跑马灯文本会向左移动，必须裁剪到实际歌词区域；否则有封面时，
        // 滚动后的文字会穿透到封面左侧/下方（封面并不能可靠遮住透明区域）。
        // 星尘跟唱时裁剪上沿扩展 starPad，给字形顶边的星星留出可见空间。
        {
            D2D1_RECT_F clip = layoutRect;
            if (stardust) clip.top -= starPad;
            renderTarget_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        renderTarget_->DrawTextLayout(
            D2D1::Point2F(textLeft, layoutRect.top), cachedLayout_.Get(), baseBrush);

        if (!useHighlightColor && enableKaraoke && progress > 0.0) {
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

        // 跟唱流光：进度前沿的横向光带（随跑马灯滚动位置同步移动）
        if (SingGlowActive(enableKaraoke, useHighlightColor, progress, layoutValid)) {
            const float front = textLeft + textWidth * static_cast<float>(std::clamp(progress, 0.0, 1.0));
            if (SetupSingGlowBrush(front, false)) {
                renderTarget_->DrawTextLayout(
                    D2D1::Point2F(textLeft, layoutRect.top), cachedLayout_.Get(), singGlowBrush_.Get());
            }
        }

        // 星尘跟唱：星星沿字形顶边跳跃旋转 + 流星粒子拖尾
        if (stardust) {
            DrawStardust(text, layoutRect, cachedLayout_.Get(), textWidth, textLeft,
                         progress, charProgress, songPositionMs, isPlaying, opacity);
        }
        renderTarget_->PopAxisAlignedClip();
    } else {
        // ═══════ 非滚动模式：居中显示 ═══════
        // 裁剪到 layoutRect，避免长歌词文本溢出覆盖封面
        // 星尘跟唱时裁剪上沿扩展 starPad，给字形顶边的星星留出可见空间
        {
            D2D1_RECT_F clip = layoutRect;
            if (stardust) clip.top -= starPad;
            renderTarget_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        renderTarget_->DrawTextW(
            text.c_str(), length, textFormat_.Get(), layoutRect, baseBrush);

        if (!useHighlightColor && enableKaraoke && progress > 0.0 && layoutValid) {
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

        // 跟唱流光：进度前沿的横向光带（居中文本）
        if (SingGlowActive(enableKaraoke, useHighlightColor, progress, layoutValid)) {
            const float centeredLeft = layoutRect.left + (availableWidth - textWidth) / 2.0f;
            const float front = centeredLeft + textWidth * static_cast<float>(std::clamp(progress, 0.0, 1.0));
            if (SetupSingGlowBrush(front, false)) {
                renderTarget_->DrawTextW(
                    text.c_str(), length, textFormat_.Get(), layoutRect, singGlowBrush_.Get());
            }
        }

        // 星尘跟唱：星星沿字形顶边跳跃旋转 + 流星粒子拖尾
        if (stardust) {
            const float centeredLeft = layoutRect.left + (availableWidth - textWidth) / 2.0f;
            DrawStardust(text, layoutRect, cachedLayout_.Get(), textWidth, centeredLeft,
                         progress, charProgress, songPositionMs, isPlaying, opacity);
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
