// SPDX-License-Identifier: GPL-3.0
// ui_elements.cpp - 具体控件实现（Fluent Design 暗色主题 + Linear 紫色风格）
#include "ui/ui_elements.h"
#include "ui/color_utils.h"
#include "core/constants.h"

#include <cmath>

// Utf8ToWide 在 color_utils.h 中声明（moekoe 命名空间）
using moekoe::Utf8ToWide;

namespace moekoe::ui {

// ── 辅助：获取 DWrite 工厂（共享实例，避免重复创建） ──
static Microsoft::WRL::ComPtr<IDWriteFactory>& GetDWriteFactory() {
    static Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    if (!factory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    }
    return factory;
}

// ── 辅助：创建临时画刷 ──
static Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> MakeBrush(
    ID2D1RenderTarget* rt, const D2D1_COLOR_F& color)
{
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(color, brush.GetAddressOf());
    return brush;
}

// ── 辅助：创建文本格式 ──
static Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeTextFormat(
    const wchar_t* fontFamily, float fontSize,
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL)
{
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    GetDWriteFactory()->CreateTextFormat(
        fontFamily, nullptr, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        fontSize, L"zh-Hans", fmt.GetAddressOf());
    return fmt;
}

// ═══════════════════════════════════════
// TextBlock
// ═══════════════════════════════════════

float TextBlock::MeasureWidth(float availWidth) {
    return availWidth;  // 文本占满可用宽度
}

float TextBlock::MeasureHeight(float availWidth) {
    switch (style) {
    case Style::Title:          return 32;
    case Style::SectionHeader:  return 28;
    case Style::Caption:        return 20;
    default:                    return 24;
    }
}

void TextBlock::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void TextBlock::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_ || text.empty()) return;

    float fontSize = 14.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    D2D1_COLOR_F color = ctx.TextSecondary();

    switch (style) {
    case Style::Title:
        fontSize = 28;
        weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
        color = ctx.Text();
        break;
    case Style::SectionHeader:
        fontSize = 13;
        weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
        color = ctx.Text();
        break;
    case Style::Caption:
        fontSize = 12;
        color = ctx.TextSecondary();
        break;
    default:
        fontSize = 12;
        color = ctx.TextSecondary();
        break;
    }

    auto fmt = MakeTextFormat(L"Segoe UI Variable", fontSize, weight);
    if (!fmt) return;

    auto brush = MakeBrush(rt, color);
    std::wstring wtext = Utf8ToWide(text);
    rt->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size()),
                  fmt.Get(), D2D1::RectF(x_, y_, x_ + w_, y_ + h_),
                  brush.Get());
}

// ═══════════════════════════════════════
// Button — 圆角按钮（增强版：更明显的悬停效果+阴影+按下缩放）
// ═══════════════════════════════════════

float Button::MeasureWidth(float availWidth) {
    return 100;  // 固定宽度
}

float Button::MeasureHeight(float availWidth) {
    return 34;  // 稍微增高
}

void Button::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void Button::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    float hoverT = hoverT_;

    // 按钮背景色（使用紫色主题）
    D2D1_COLOR_F bgDefault, bgHover, bgPress, textColor;
    if (isPrimary) {
        bgDefault = ctx.accent;                // #6C5CE7
        bgHover   = ctx.accentHover;           // #7F70F0
        bgPress   = D2D1::ColorF(ctx.accent.r * 0.8f, ctx.accent.g * 0.8f, ctx.accent.b * 0.8f, 1.0f);
        textColor = D2D1::ColorF(1, 1, 1, 1);
    } else if (isDanger) {
        bgDefault = ctx.danger;
        bgHover   = D2D1::ColorF(1.0f, 0.478f, 0.478f, 1.0f);
        bgPress   = D2D1::ColorF(0.867f, 0.294f, 0.294f, 1.0f);
        textColor = D2D1::ColorF(1, 1, 1, 1);
    } else {
        // 默认按钮：基于主题 surface 色派生（使用访问器以兼容亮色模式）
        auto s = ctx.Surface();
        bgDefault = D2D1::ColorF(s.r * 0.9f, s.g * 0.9f, s.b * 0.9f, 1.0f);
        bgHover   = D2D1::ColorF(s.r * 1.1f, s.g * 1.1f, s.b * 1.1f, 1.0f);
        bgPress   = D2D1::ColorF(s.r * 0.75f, s.g * 0.75f, s.b * 0.75f, 1.0f);
        textColor = ctx.Text();
    }

    D2D1_COLOR_F bgColor = bgDefault;
    if (pressed) {
        bgColor = bgPress;
    } else if (hoverT > 0) {
        bgColor = LerpColor(bgDefault, bgHover, hoverT);
    }

    // 按钮阴影（悬停/主按钮时更明显）
    float shadowAlpha = isPrimary ? 0.3f : (0.15f + 0.15f * hoverT);
    if (shadowAlpha > 0.01f) {
        auto shadowBrush = MakeBrush(rt, D2D1::ColorF(0, 0, 0, shadowAlpha));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x_ + 1, y_ + 2, x_ + w_ + 1, y_ + h_ + 3), 6, 6),
            shadowBrush.Get());
    }

    // 圆角6px按钮背景
    auto bgBrush = MakeBrush(rt, bgColor);
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x_, y_, x_ + w_, y_ + h_), 6, 6};
    rt->FillRoundedRectangle(rr, bgBrush.Get());

    // 悬停时加一个微妙的顶部高光
    if (hoverT > 0.01f && !pressed) {
        auto highlightBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 0.06f * hoverT));
        D2D1_ROUNDED_RECT hlRR = {D2D1::RectF(x_, y_, x_ + w_, y_ + h_ / 2), 6, 6};
        rt->FillRoundedRectangle(hlRR, highlightBrush.Get());
    }

    // 边框（主按钮用亮色边框，默认用半透明边框；亮色模式自适应）
    D2D1_COLOR_F borderColor = isPrimary
        ? LerpColor(ctx.accent, ctx.Border(), hoverT)
        : ctx.Border();
    auto borderBrush = MakeBrush(rt, borderColor);
    rt->DrawRoundedRectangle(rr, borderBrush.Get(), 1);

    // 文本
    auto fmt = MakeTextFormat(L"Segoe UI Variable", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto txtBrush = MakeBrush(rt, textColor);
        std::wstring wtext = Utf8ToWide(text);
        rt->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_, y_ + h_),
                      txtBrush.Get());
    }
}

UIElement* Button::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    return this;
}

bool Button::TickAnimation(float deltaMs) {
    // 悬停动画：启动/更新
    if (hovered && !hoverAnim_.active) {
        hoverAnim_.Start(hoverT_, 1.0f, 180.0f, EaseOutCubic);
    } else if (!hovered && !hoverAnim_.active && hoverT_ > 0.001f) {
        hoverAnim_.Start(hoverT_, 0.0f, 200.0f, EaseOutCubic);
    }

    if (hoverAnim_.active) {
        hoverAnim_.Tick(deltaMs);
        hoverT_ = hoverAnim_.Current();
        if (hoverAnim_.IsDone()) {
            hoverT_ = hoverAnim_.to;
        }
    }

    return hoverAnim_.active || UIElement::TickAnimation(deltaMs);
}

// ═══════════════════════════════════════
// Toggle — 开关（增强版：更大尺寸+阴影+发光效果）
// ═══════════════════════════════════════

float Toggle::MeasureWidth(float availWidth) {
    return availWidth;
}

float Toggle::MeasureHeight(float availWidth) {
    return 40;  // 增高以容纳更大的开关
}

void Toggle::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void Toggle::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 标签
    auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 56, y_ + h_),
                      txtBrush.Get());
    }

    // 开关轨道（更大的胶囊形）
    const float trackW = 44, trackH = 24;
    const float trackX = x_ + w_ - trackW - 6;
    const float trackY = y_ + (h_ - trackH) / 2;
    const float radius = trackH / 2;

    float thumbPos = thumbPos_;

    // 轨道颜色：关闭用边框色（亮色模式自适应），开启用强调色
    D2D1_COLOR_F trackOff = ctx.Border();
    D2D1_COLOR_F trackOn  = ctx.accent;
    D2D1_COLOR_F trackColor = LerpColor(trackOff, trackOn, thumbPos);

    // 轨道发光效果（开启时）
    if (thumbPos > 0.01f) {
        auto glowBrush = MakeBrush(rt, D2D1::ColorF(
            ctx.accent.r, ctx.accent.g, ctx.accent.b, 0.2f * thumbPos));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(trackX - 2, trackY - 2,
                                           trackX + trackW + 2, trackY + trackH + 2),
                              radius + 2, radius + 2),
            glowBrush.Get());
    }

    // 轨道背景
    auto trackBrush = MakeBrush(rt, trackColor);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH),
                          radius, radius),
        trackBrush.Get());

    // 滑块圆点（更大，带阴影）
    const float thumbR = 8;
    const float thumbMargin = 4;
    float thumbX = trackX + thumbMargin + thumbR + thumbPos * (trackW - 2 * thumbMargin - 2 * thumbR);
    const float thumbY = trackY + trackH / 2;

    // 滑块阴影
    auto thumbShadow = MakeBrush(rt, D2D1::ColorF(0, 0, 0, 0.3f));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY + 1), thumbR + 1, thumbR + 1),
                    thumbShadow.Get());

    // 滑块主体（开启时白带微紫，关闭时灰白）
    D2D1_COLOR_F thumbColor = LerpColor(
        D2D1::ColorF(0.85f, 0.85f, 0.90f, 1.0f),  // 关闭：灰白
        D2D1::ColorF(1, 1, 1, 1),                    // 开启：纯白
        thumbPos);
    auto thumbBrush = MakeBrush(rt, thumbColor);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR, thumbR),
                    thumbBrush.Get());

    // 悬停时整行微高亮
    if (hovered) {
        auto hoverBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 0.03f));
        rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), hoverBrush.Get());
    }
}

UIElement* Toggle::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    return this;
}

bool Toggle::TickAnimation(float deltaMs) {
    float target = value ? 1.0f : 0.0f;

    if (!thumbAnim_.active && std::abs(thumbPos_ - target) > 0.001f) {
        thumbAnim_.Start(thumbPos_, target, 250.0f, SpringEase);
    }

    if (thumbAnim_.active) {
        thumbAnim_.Tick(deltaMs);
        thumbPos_ = thumbAnim_.Current();
        if (thumbAnim_.IsDone()) {
            thumbPos_ = thumbAnim_.to;
        }
    }

    return thumbAnim_.active || UIElement::TickAnimation(deltaMs);
}

// ═══════════════════════════════════════
// Slider — 滑块（增强版：更大的手柄+更明显的光晕+渐变轨道）
// ═══════════════════════════════════════

float Slider::MeasureWidth(float availWidth) {
    return availWidth;
}

float Slider::MeasureHeight(float availWidth) {
    return 40;  // 增高以容纳更大的手柄
}

void Slider::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void Slider::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 标签
    auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 80, y_ + 22),
                      txtBrush.Get());
    }

    // 值
    char valBuf[64];
    snprintf(valBuf, sizeof(valBuf), "%.0f%s", value, suffix.c_str());
    auto valFmt = MakeTextFormat(L"Segoe UI Variable", 12);
    if (valFmt) {
        valFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        valFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        auto valBrush = MakeBrush(rt, ctx.accentLight);  // 用强调色显示值
        std::wstring wval = Utf8ToWide(valBuf);
        rt->DrawTextW(wval.c_str(), static_cast<UINT32>(wval.size()),
                      valFmt.Get(), D2D1::RectF(x_ + w_ - 80, y_, x_ + w_, y_ + 22),
                      valBrush.Get());
    }

    // 轨道
    const float trackX = x_;
    const float trackY = y_ + 26;
    const float trackW = w_;
    const float trackH = 6;  // 轨道稍高
    float ratio = (maxValue > minValue) ? (value - minValue) / (maxValue - minValue) : 0;
    ratio = std::clamp(ratio, 0.0f, 1.0f);

    // 背景轨道（圆角3px，边框色 — 使用访问器兼容亮色模式）
    auto trackBg = MakeBrush(rt, ctx.Border());
    D2D1_ROUNDED_RECT trackRR = {D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH), 3, 3};
    rt->FillRoundedRectangle(trackRR, trackBg.Get());

    // 已填充部分（紫色→浅紫渐变）
    if (ratio > 0) {
        if (!fillGradientBrush_) {
            D2D1_GRADIENT_STOP stops[] = {
                { 0.0f, ctx.accent },        // #6C5CE7
                { 1.0f, ctx.accentLight },   // #A29BFE
            };
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
            rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2,
                                             D2D1_EXTEND_MODE_CLAMP, &collection);
            if (collection) {
                rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(trackX, 0), D2D1::Point2F(trackX + trackW * ratio, 0)),
                    collection.Get(), &fillGradientBrush_);
            }
        }
        if (fillGradientBrush_) {
            fillGradientBrush_->SetEndPoint(D2D1::Point2F(trackX + trackW * ratio, 0));
            D2D1_ROUNDED_RECT fillRR = {D2D1::RectF(trackX, trackY, trackX + trackW * ratio, trackY + trackH), 3, 3};
            rt->FillRoundedRectangle(fillRR, fillGradientBrush_.Get());
        } else {
            auto trackFg = MakeBrush(rt, ctx.accent);
            D2D1_ROUNDED_RECT fillRR = {D2D1::RectF(trackX, trackY, trackX + trackW * ratio, trackY + trackH), 3, 3};
            rt->FillRoundedRectangle(fillRR, trackFg.Get());
        }
    }

    // 滑块手柄（8px 圆 + 大光晕）
    const float thumbX = trackX + trackW * ratio;
    const float thumbY = trackY + trackH / 2;
    const float thumbR = 8;  // 更大的手柄

    // hover 光晕（更明显）
    float hoverT = hoverT_;
    if (hoverT > 0.01f) {
        // 外层光晕
        auto glowBrush = MakeBrush(rt, D2D1::ColorF(
            ctx.accent.r, ctx.accent.g, ctx.accent.b, 0.25f * hoverT));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR + 8, thumbR + 8),
                        glowBrush.Get());
        // 中层光晕
        auto glowBrush2 = MakeBrush(rt, D2D1::ColorF(
            ctx.accent.r, ctx.accent.g, ctx.accent.b, 0.15f * hoverT));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR + 14, thumbR + 14),
                        glowBrush2.Get());
    }

    // 手柄阴影
    auto thumbShadow = MakeBrush(rt, D2D1::ColorF(0, 0, 0, 0.3f));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY + 1), thumbR + 1, thumbR + 1),
                    thumbShadow.Get());

    // 手柄主体
    auto thumbBrush = MakeBrush(rt, ctx.accent);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR, thumbR),
                    thumbBrush.Get());

    // 手柄内圆（白色高光）
    auto innerBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, dragging ? 0.9f : 0.5f));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), 3.5f, 3.5f),
                    innerBrush.Get());
}

UIElement* Slider::HitTest(float x, float y) {
    if (!visible_) return nullptr;
    // 匹配轨道区域和标签区域
    if (x >= x_ && x <= x_ + w_ && y >= y_ + 16 && y <= y_ + h_)
        return this;
    return nullptr;
}

bool Slider::TickAnimation(float deltaMs) {
    if (hovered || dragging) {
        if (!hoverAnim_.active && hoverT_ < 0.99f) {
            hoverAnim_.Start(hoverT_, 1.0f, 180.0f, EaseOutCubic);
        }
    } else {
        if (!hoverAnim_.active && hoverT_ > 0.01f) {
            hoverAnim_.Start(hoverT_, 0.0f, 250.0f, EaseOutCubic);
        }
    }

    if (hoverAnim_.active) {
        hoverAnim_.Tick(deltaMs);
        hoverT_ = hoverAnim_.Current();
        if (hoverAnim_.IsDone()) {
            hoverT_ = hoverAnim_.to;
        }
    }

    return hoverAnim_.active || UIElement::TickAnimation(deltaMs);
}

// ═══════════════════════════════════════
// ComboBox — 下拉选择（增强版：悬停高亮+阴影+动画）
// ═══════════════════════════════════════

float ComboBox::MeasureWidth(float availWidth) {
    return availWidth;
}

float ComboBox::MeasureHeight(float availWidth) {
    return dropped ? static_cast<float>(40 + items.size() * 32) : 40;
}

void ComboBox::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void ComboBox::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 标签
    auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ / 2, y_ + 40),
                      txtBrush.Get());
    }

    // 下拉框
    const float boxX = x_ + w_ / 2;
    const float boxW = w_ / 2;
    const float boxH = 30;
    const float boxY = y_ + 5;

    // 背景（基于主题 surface 色派生 → hover 更亮；使用访问器兼容亮色模式）
    auto surf = ctx.Surface();
    D2D1_COLOR_F bgColor = hovered
        ? D2D1::ColorF(surf.r * 1.1f, surf.g * 1.1f, surf.b * 1.1f, 1.0f)
        : D2D1::ColorF(surf.r * 0.9f, surf.g * 0.9f, surf.b * 0.9f, 1.0f);
    auto boxBg = MakeBrush(rt, bgColor);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(boxX, boxY, boxX + boxW, boxY + boxH), 6, 6),
                              boxBg.Get());

    // 边框
    D2D1_COLOR_F borderColor = hovered
        ? D2D1::ColorF(ctx.accent.r, ctx.accent.g, ctx.accent.b, 0.5f)  // 悬停时强调色边框
        : ctx.Border();
    auto boxBorder = MakeBrush(rt, borderColor);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(boxX, boxY, boxX + boxW, boxY + boxH), 6, 6),
                              boxBorder.Get(), 1);

    // 当前选中项
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size())) {
        auto itemFmt = MakeTextFormat(L"Segoe UI Variable", 13);
        if (itemFmt) {
            itemFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto valBrush = MakeBrush(rt, ctx.Text());
            std::wstring witem = Utf8ToWide(items[selectedIndex]);
            rt->DrawTextW(witem.c_str(), static_cast<UINT32>(witem.size()),
                          itemFmt.Get(), D2D1::RectF(boxX + 10, boxY, boxX + boxW - 24, boxY + boxH),
                          valBrush.Get());
        }
    }

    // 下拉箭头 ▼（用紫色强调色）
    auto arrowBrush = MakeBrush(rt, hovered ? ctx.accent : ctx.TextSecondary());
    float ax = boxX + boxW - 14, ay = boxY + boxH / 2;
    rt->DrawLine(D2D1::Point2F(ax - 4, ay - 2), D2D1::Point2F(ax, ay + 2), arrowBrush.Get(), 1.5f);
    rt->DrawLine(D2D1::Point2F(ax, ay + 2), D2D1::Point2F(ax + 4, ay - 2), arrowBrush.Get(), 1.5f);

    // 下拉列表（由 DrawComboBoxDropdown 在裁剪区域外绘制）
}

UIElement* ComboBox::HitTest(float x, float y) {
    if (!visible_) return nullptr;
    if (dropped) {
        float dropH = static_cast<float>(items.size() * 32);
        float boxX = x_ + w_ / 2;
        float boxY = y_ + 5;
        if (x >= boxX && x <= x_ + w_ && y >= boxY && y <= boxY + 30 + dropH)
            return this;
        return nullptr;
    }
    float boxX = x_ + w_ / 2;
    float boxY = y_ + 5;
    if (x >= boxX && x <= x_ + w_ && y >= boxY && y <= boxY + 30)
        return this;
    return nullptr;
}

// ═══════════════════════════════════════
// Card — 圆角容器（增强版：阴影+更明显的分隔线+悬停效果）
// ═══════════════════════════════════════

float Card::MeasureWidth(float availWidth) {
    return availWidth;
}

float Card::MeasureHeight(float availWidth) {
    float contentH = 0;
    float innerW = availWidth - padding.left - padding.right;

    if (!title.empty()) {
        contentH += 24 + 8;
    }

    for (size_t i = 0; i < children_.size(); ++i) {
        if (!children_[i]->IsVisible()) continue;
        contentH += children_[i]->MeasureHeight(innerW);
        if (i > 0) contentH += gap;
    }

    return contentH + padding.top + padding.bottom;
}

void Card::Arrange(float x, float y, float w, float h) {
    UIElement::Arrange(x, y, w, h);

    float innerX = x + padding.left;
    float innerW = w - padding.left - padding.right;
    float curY = y + padding.top;

    if (!title.empty()) {
        curY += 24 + 8;
    }

    for (auto& child : children_) {
        if (!child->IsVisible()) continue;
        float childH = child->MeasureHeight(innerW);
        child->Arrange(innerX, curY, innerW, childH);
        curY += childH + gap;
    }
}

void Card::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void Card::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 卡片阴影（微妙的深色投影）
    auto shadowBrush = MakeBrush(rt, D2D1::ColorF(0, 0, 0, 0.25f));
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x_ + 2, y_ + 3, x_ + w_ + 2, y_ + h_ + 4), cornerRadius, cornerRadius),
        shadowBrush.Get());

    // 卡片背景（微蓝紫表面色）
    D2D1_COLOR_F cardBgColor = ctx.Surface();
    auto cardBg = MakeBrush(rt, cardBgColor);
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x_, y_, x_ + w_, y_ + h_), cornerRadius, cornerRadius};
    rt->FillRoundedRectangle(rr, cardBg.Get());

    // 卡片顶部高光（模拟微妙的光泽）
    auto highlightBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 0.03f));
    D2D1_ROUNDED_RECT hlRR = {D2D1::RectF(x_, y_, x_ + w_, y_ + h_ * 0.3f), cornerRadius, cornerRadius};
    rt->FillRoundedRectangle(hlRR, highlightBrush.Get());

    // 卡片边框（使用主题边框色，亮暗模式自适应）
    auto cardBorder = MakeBrush(rt, ctx.Border());
    rt->DrawRoundedRectangle(rr, cardBorder.Get(), 1);

    // 标题（Segoe UI Variable 13px SemiBold）
    if (!title.empty()) {
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (fmt) {
            auto txtBrush = MakeBrush(rt, ctx.Text());
            std::wstring wtitle = Utf8ToWide(title);
            rt->DrawTextW(wtitle.c_str(), static_cast<UINT32>(wtitle.size()),
                          fmt.Get(), D2D1::RectF(x_ + padding.left, y_ + padding.top,
                                                  x_ + w_ - padding.right, y_ + padding.top + 24),
                          txtBrush.Get());
        }
    }

    // 绘制子元素之间的分隔线（更明显）
    float innerX = x_ + padding.left;
    float innerW = w_ - padding.left - padding.right;
    float curY = y_ + padding.top;
    if (!title.empty()) curY += 24 + 8;

    for (size_t i = 0; i < children_.size(); ++i) {
        if (!children_[i]->IsVisible()) continue;
        float childH = children_[i]->MeasureHeight(innerW);
        if (i > 0) {
            float lineY = curY - gap / 2;
            // 分隔线使用主题边框色，亮暗模式自适应
            auto sepBrush = MakeBrush(rt, ctx.Border());
            rt->DrawLine(
                D2D1::Point2F(innerX + 8, lineY),
                D2D1::Point2F(innerX + innerW - 8, lineY),
                sepBrush.Get(), 1);
        }
        curY += childH + gap;
    }

    // 绘制子元素
    for (auto& child : children_) {
        if (child->IsVisible()) child->Draw(rt, ctx);
    }
}

// ═══════════════════════════════════════
// NavItem — 左侧导航项（增强版：圆角选中+悬停动画+图标）
// ═══════════════════════════════════════

float NavItem::MeasureWidth(float availWidth) {
    return availWidth;
}

float NavItem::MeasureHeight(float availWidth) {
    return 38;
}

void NavItem::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void NavItem::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 选中/悬停背景（圆角矩形）
    float insetX = x_ + 8;
    float insetW = w_ - 16;

    if (selected) {
        // 选中背景：使用访问器兼容亮色模式
        auto selBg = MakeBrush(rt, ctx.Surface());
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(insetX, y_ + 2, insetX + insetW, y_ + h_ - 2), 6, 6),
            selBg.Get());

        // 左侧紫色指示器（3px圆角条）
        auto accent = MakeBrush(rt, ctx.accent);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(insetX, y_ + 8, insetX + 3, y_ + h_ - 8), 1.5f, 1.5f),
            accent.Get());
    } else if (hovered) {
        // 悬停背景：使用主题 surface 色派生，亮暗模式自适应
        auto surf = ctx.Surface();
        auto hoverBg = MakeBrush(rt, D2D1::ColorF(surf.r * 1.05f, surf.g * 1.05f, surf.b * 1.05f, 1.0f));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(insetX, y_ + 2, insetX + insetW, y_ + h_ - 2), 6, 6),
            hoverBg.Get());
    }

    // 图标（Segoe MDL2 Assets）
    float textX = x_ + 20;
    if (!icon.empty()) {
        auto iconFmt = MakeTextFormat(L"Segoe MDL2 Assets", 15);
        if (iconFmt) {
            iconFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto iconBrush = MakeBrush(rt, selected ? ctx.accent : ctx.TextSecondary());
            rt->DrawTextW(icon.c_str(), static_cast<UINT32>(icon.size()),
                          iconFmt.Get(), D2D1::RectF(x_ + 16, y_, x_ + 40, y_ + h_),
                          iconBrush.Get());
        }
        textX = x_ + 44;
    }

    // 文本
    auto fmt = MakeTextFormat(L"Segoe UI Variable", 14,
                              selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto txtBrush = MakeBrush(rt, selected ? ctx.accent : ctx.TextSecondary());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(textX, y_, x_ + w_, y_ + h_),
                      txtBrush.Get());
    }
}

UIElement* NavItem::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    return this;
}

// ═══════════════════════════════════════
// ColorRow — 颜色选择行
// ═══════════════════════════════════════

float ColorRow::MeasureWidth(float availWidth) {
    return availWidth;
}

float ColorRow::MeasureHeight(float availWidth) {
    return 40;
}

void ColorRow::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void ColorRow::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 标签
    auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 90, y_ + h_),
                      txtBrush.Get());
    }

    // 28×28 圆角色块（更大更明显）
    const float swatchW = 28, swatchH = 28;
    const float swatchX = x_ + w_ - swatchW - 64;
    const float swatchY = y_ + (h_ - swatchH) / 2;

    // 色块阴影
    auto shadowBrush = MakeBrush(rt, D2D1::ColorF(0, 0, 0, 0.3f));
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(swatchX + 1, swatchY + 2, swatchX + swatchW + 1, swatchY + swatchH + 2), 6, 6),
        shadowBrush.Get());

    // 色块
    auto colorBrush = MakeBrush(rt, colorValue);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(swatchX, swatchY, swatchX + swatchW, swatchY + swatchH), 6, 6),
        colorBrush.Get());

    // 边框（亮暗模式自适应）
    auto borderBrush = MakeBrush(rt, ctx.Border());
    rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(swatchX, swatchY, swatchX + swatchW, swatchY + swatchH), 6, 6),
        borderBrush.Get(), 1);

    // HEX 文本
    auto hexFmt = MakeTextFormat(L"Consolas", 12);
    if (hexFmt) {
        hexFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto hexBrush = MakeBrush(rt, ctx.TextSecondary());
        std::wstring whex = Utf8ToWide(textValue);
        rt->DrawTextW(whex.c_str(), static_cast<UINT32>(whex.size()),
                      hexFmt.Get(), D2D1::RectF(swatchX + swatchW + 8, swatchY, x_ + w_ - 4, swatchY + swatchH),
                      hexBrush.Get());
    }

    // 悬停高亮
    if (hovered) {
        auto hoverBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 0.04f));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), 6, 6),
            hoverBrush.Get());
    }
}

UIElement* ColorRow::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    return this;
}

// ═══════════════════════════════════════
// LabelRow — 只读标签行
// ═══════════════════════════════════════

float LabelRow::MeasureWidth(float availWidth) {
    return availWidth;
}

float LabelRow::MeasureHeight(float availWidth) {
    return 40;
}

void LabelRow::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void LabelRow::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 标签
    auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ / 2, y_ + h_),
                      txtBrush.Get());
    }

    // 值（右对齐）
    auto valFmt = MakeTextFormat(L"Segoe UI Variable", 12);
    if (valFmt) {
        valFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        valFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        auto valBrush = MakeBrush(rt, !readOnly ? ctx.accentLight : ctx.TextSecondary());
        std::wstring wval = Utf8ToWide(textValue);
        rt->DrawTextW(wval.c_str(), static_cast<UINT32>(wval.size()),
                      valFmt.Get(), D2D1::RectF(x_ + w_ / 2, y_, x_ + w_ - 16, y_ + h_),
                      valBrush.Get());
    }

    // 悬停高亮（非只读时）
    if (hovered && !readOnly) {
        auto hoverBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 0.04f));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), 6, 6),
            hoverBrush.Get());
    }
}

UIElement* LabelRow::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    return this;
}

// ═══════════════════════════════════════
// ThemePresets — 预设主题色板
// ═══════════════════════════════════════

float ThemePresets::MeasureWidth(float availWidth) {
    return availWidth;
}

float ThemePresets::MeasureHeight(float availWidth) {
    return 76;
}

void ThemePresets::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void ThemePresets::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_ || presets.empty()) return;

    const int cols = static_cast<int>(presets.size());
    const float gap = 10;
    const float swatchW = 40;
    const float swatchH = 40;
    const float totalW = cols * swatchW + (cols - 1) * gap;
    const float startX = x_ + (w_ - totalW) / 2;

    auto nameFmt = MakeTextFormat(L"Segoe UI Variable", 11);

    for (int i = 0; i < cols; ++i) {
        const float sx = startX + i * (swatchW + gap);
        const float sy = y_ + 4;

        // 色块阴影
        auto shadowBrush = MakeBrush(rt, D2D1::ColorF(0, 0, 0, 0.2f));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(sx + 1, sy + 2, sx + swatchW + 1, sy + swatchH + 2), 8, 8),
            shadowBrush.Get());

        // 上半高亮色
        const float halfH = swatchH / 2;
        auto hlBrush = MakeBrush(rt, presets[i].hlColor);
        // 下半普通色
        auto nlBrush = MakeBrush(rt, presets[i].nlColor);

        // 上半圆角
        D2D1_ROUNDED_RECT topRr = {D2D1::RectF(sx, sy, sx + swatchW, sy + halfH), 8, 8};
        rt->FillRoundedRectangle(topRr, hlBrush.Get());

        // 下半圆角
        D2D1_ROUNDED_RECT botRr = {D2D1::RectF(sx, sy + halfH, sx + swatchW, sy + swatchH), 8, 8};
        rt->FillRoundedRectangle(botRr, nlBrush.Get());

        // 选中边框（紫色强调色，更粗）
        if (i == selectedIndex) {
            auto selBrush = MakeBrush(rt, ctx.accent);
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(sx - 2, sy - 2, sx + swatchW + 2, sy + swatchH + 2), 9, 9),
                selBrush.Get(), 2.5f);
        }

        // 悬停边框
        if (i == hoveredIndex && i != selectedIndex) {
            auto hoverBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 0.2f));
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(sx - 1, sy - 1, sx + swatchW + 1, sy + swatchH + 1), 8, 8),
                hoverBrush.Get(), 1.5f);
        }

        // 名称
        if (nameFmt) {
            auto nameBrush = MakeBrush(rt, ctx.TextSecondary());
            nameFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            std::wstring wname = Utf8ToWide(presets[i].name);
            rt->DrawTextW(wname.c_str(), static_cast<UINT32>(wname.size()),
                          nameFmt.Get(), D2D1::RectF(sx - 8, sy + swatchH + 4, sx + swatchW + 8, sy + swatchH + 22),
                          nameBrush.Get());
        }
    }
}

UIElement* ThemePresets::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    return this;
}

} // namespace moekoe::ui
