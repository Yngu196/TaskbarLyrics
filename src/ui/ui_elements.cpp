// SPDX-License-Identifier: Apache-2.0
// ui_elements.cpp - 具体控件实现（Fluent Design 暗色主题 + Linear 紫色风格）
#include "ui/ui_elements.h"
#include "ui/color_utils.h"
#include "core/constants.h"

#include <cmath>

// Utf8ToWide 在 color_utils.h 中声明（moekoe 命名空间）
using moekoe::Utf8ToWide;

namespace moekoe::ui {

// ── 辅助：获取 DWrite 工厂（共享实例，避免重复创建） ──
Microsoft::WRL::ComPtr<IDWriteFactory>& GetSharedDWriteFactory() {
    static Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    if (!factory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    }
    return factory;
}

void ReleaseSharedUIDWriteFactory() {
    GetSharedDWriteFactory().Reset();
}

static Microsoft::WRL::ComPtr<IDWriteFactory>& GetDWriteFactory() {
    return GetSharedDWriteFactory();
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
    case Style::SectionHeader:  return 30;
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
        fontSize = 14;
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

    // 默认按钮保持接近卡片底色，以边框与强调色反馈状态；避免亮色主题下的大面积灰块。
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
        // 默认按钮：仅作轻微强调色混合，亮暗主题都能维持干净层级。
        auto s = ctx.Surface();
        bgDefault = s;
        bgHover   = LerpColor(s, ctx.accent, 0.08f);
        bgPress   = LerpColor(s, ctx.accent, 0.16f);
        textColor = ctx.accent;
    }

    D2D1_COLOR_F bgColor = bgDefault;
    if (pressed) {
        bgColor = bgPress;
    } else if (hoverT > 0) {
        bgColor = LerpColor(bgDefault, bgHover, hoverT);
    }

    // 仅保留很轻的层次投影，避免表单控件看起来过重。
    float shadowAlpha = isPrimary ? 0.16f : (0.03f + 0.05f * hoverT);
    if (shadowAlpha > 0.01f) {
        auto shadowBrush = MakeBrush(rt, D2D1::ColorF(0, 0, 0, shadowAlpha));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x_ + 1, y_ + 1, x_ + w_ + 1, y_ + h_ + 2), 7, 7),
            shadowBrush.Get());
    }

    // 7px 圆角与卡片保持一致，但不增加额外绘制层。
    auto bgBrush = MakeBrush(rt, bgColor);
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x_, y_, x_ + w_, y_ + h_), 7, 7};
    rt->FillRoundedRectangle(rr, bgBrush.Get());

    // 边框提供清晰但克制的可点击感。
    D2D1_COLOR_F borderColor = isPrimary
        ? LerpColor(ctx.accent, ctx.Border(), hoverT)
        : (hoverT > 0.01f ? D2D1::ColorF(ctx.accent.r, ctx.accent.g, ctx.accent.b, 0.50f) : ctx.Border());
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

    // 标签（紧凑模式下由 SettingRow 行标题承担）
    if (!compact) {
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
        if (fmt) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
            std::wstring wlabel = Utf8ToWide(label);
            rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                          fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 56, y_ + h_),
                          txtBrush.Get());
        }
    }

    // 开关轨道（更大的胶囊形）
    const float trackW = 44, trackH = 24;
    const float trackX = compact ? x_ : x_ + w_ - trackW - 6;
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

    // 标签（紧凑模式下由 SettingRow 行标题承担）
    if (!compact) {
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
        if (fmt) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
            std::wstring wlabel = Utf8ToWide(label);
            rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                          fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 80, y_ + 22),
                          txtBrush.Get());
        }
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

void ComboBox::GetBoxRect(float* boxX, float* boxW) const {
    if (compact) {
        *boxX = x_;
        *boxW = w_;
    } else {
        *boxX = x_ + w_ / 2;
        *boxW = w_ / 2;
    }
}

void ComboBox::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void ComboBox::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 标签（紧凑模式下由 SettingRow 行标题承担）
    if (!compact) {
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
        if (fmt) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
            std::wstring wlabel = Utf8ToWide(label);
            rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                          fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ / 2, y_ + 40),
                          txtBrush.Get());
        }
    }

    // 下拉框
    const float boxH = 30;
    const float boxY = y_ + 5;
    float boxX, boxW;
    GetBoxRect(&boxX, &boxW);

    // Win11 扁平风格：无边框、圆角、悬停柔和过渡
    // 填充色为表面色的轻微中性加深（hover 时随动画进度平滑加深）
    const auto surf = ctx.Surface();
    const float boxR = 6;
    const float tint = 0.042f + 0.030f * hoverT_;
    auto boxBg = MakeBrush(rt, LerpColor(surf, ctx.Text(), tint));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(boxX, boxY, boxX + boxW, boxY + boxH), boxR, boxR),
                              boxBg.Get());

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
    float boxX, boxW;
    GetBoxRect(&boxX, &boxW);
    if (dropped) {
        float dropH = static_cast<float>(items.size() * 32);
        float boxY = y_ + 5;
        if (x >= boxX && x <= boxX + boxW && y >= boxY && y <= boxY + 30 + dropH)
            return this;
        return nullptr;
    }
    float boxY = y_ + 5;
    if (x >= boxX && x <= boxX + boxW && y >= boxY && y <= boxY + 30)
        return this;
    return nullptr;
}

bool ComboBox::TickAnimation(float deltaMs) {
    // 悬停动画：启动/更新（与 Button 一致的柔和过渡）
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

    // 双层低透明度弥散投影：贴近 Win11 卡片的"轻微悬浮"感
    const float shadowAlpha1 = ctx.isDarkMode ? 0.10f : 0.030f;
    const float shadowAlpha2 = ctx.isDarkMode ? 0.06f : 0.018f;
    auto shadowBrush1 = MakeBrush(rt, D2D1::ColorF(0, 0, 0, shadowAlpha1));
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x_, y_ + 1.5f, x_ + w_, y_ + h_ + 1.5f),
                          cornerRadius + 1, cornerRadius + 1),
        shadowBrush1.Get());
    auto shadowBrush2 = MakeBrush(rt, D2D1::ColorF(0, 0, 0, shadowAlpha2));
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x_, y_ + 3, x_ + w_, y_ + h_ + 3),
                          cornerRadius + 2, cornerRadius + 2),
        shadowBrush2.Get());

    // 卡片背景（纯表面色）
    auto cardBg = MakeBrush(rt, ctx.Surface());
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x_, y_, x_ + w_, y_ + h_), cornerRadius, cornerRadius};
    rt->FillRoundedRectangle(rr, cardBg.Get());

    // 边框极大淡化（≈ rgba(0,0,0,0.05)，暗色下用白色低透明度）
    auto cardBorder = MakeBrush(rt, ctx.isDarkMode
        ? D2D1::ColorF(1, 1, 1, 0.055f)
        : D2D1::ColorF(0, 0, 0, 0.05f));
    rt->DrawRoundedRectangle(rr, cardBorder.Get(), 1);

    // 标题使用短强调色标记，帮助长表单快速分组。
    if (!title.empty()) {
        auto accentBrush = MakeBrush(rt, ctx.accent);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x_ + padding.left, y_ + padding.top + 5,
                                          x_ + padding.left + 3, y_ + padding.top + 19), 1.5f, 1.5f),
            accentBrush.Get());
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (fmt) {
            auto txtBrush = MakeBrush(rt, ctx.Text());
            std::wstring wtitle = Utf8ToWide(title);
            rt->DrawTextW(wtitle.c_str(), static_cast<UINT32>(wtitle.size()),
                          fmt.Get(), D2D1::RectF(x_ + padding.left + 10, y_ + padding.top,
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
            // 分隔线极淡（与 Win11 设置一致），从文本起始处对齐
            auto sepBrush = MakeBrush(rt, ctx.isDarkMode
                ? D2D1::ColorF(1, 1, 1, 0.07f)
                : D2D1::ColorF(0, 0, 0, 0.07f));
            rt->DrawLine(
                D2D1::Point2F(innerX + 16, lineY),
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
// SettingRow — 独立设置行卡片（参考 Windows 11 设置风格）
// ═══════════════════════════════════════

UIElement* SettingRow::Control() {
    return children_.empty() ? nullptr : children_[0].get();
}

float SettingRow::MeasureWidth(float availWidth) {
    return availWidth;
}

float SettingRow::MeasureHeight(float availWidth) {
    // 行内控件高度统一按 40 处理（保持下拉展开时行高稳定，列表由弹窗绘制）
    const float padV = 12;
    float textH = 0;
    if (!title.empty()) textH += 20;
    if (!description.empty()) {
        // 估算说明换行行数（ASCII≈6.5px/字符，CJK≈12px/字符），行高随行数增长
        float textW = 0;
        std::wstring wdesc = Utf8ToWide(description);
        for (wchar_t c : wdesc) textW += (c < 0x2000) ? 6.5f : 12.0f;
        // 与 Draw 的 textRight 一致：右侧按控件类型预留宽度
        float ctrlW = 200;
        UIElement* ctrl = Control();
        if (ctrl && ctrl->IsVisible()) {
            if (dynamic_cast<Toggle*>(ctrl)) ctrlW = 56;
            else if (dynamic_cast<ComboBox*>(ctrl)) ctrlW = 200;
            else if (dynamic_cast<ColorRow*>(ctrl)) ctrlW = 110;
            else if (dynamic_cast<LabelRow*>(ctrl)) ctrlW = 200;
            else if (dynamic_cast<Slider*>(ctrl)) ctrlW = 240;
            else if (dynamic_cast<Button*>(ctrl)) ctrlW = 100;
        }
        float iconW = icon.empty() ? 0.0f : 28.0f;
        float textX = 16 + iconW;
        float textRight = std::min(availWidth - 150, availWidth - 16 - ctrlW - 12);
        float avail = std::max(80.0f, textRight - textX);
        float lines = std::max(1.0f, std::ceil(textW / avail));
        textH += (title.empty() ? 0 : 2) + 18 * lines;
    }
    return std::max(textH, 40.0f) + padV * 2;
}

void SettingRow::Arrange(float x, float y, float w, float h) {
    UIElement::Arrange(x, y, w, h);

    UIElement* ctrl = Control();
    if (!ctrl || !ctrl->IsVisible()) return;

    const float padH = 16;
    float ctrlW = 0;
    float ctrlH = 40;

    if (dynamic_cast<Toggle*>(ctrl)) {
        ctrlW = 56;
    } else if (dynamic_cast<ComboBox*>(ctrl)) {
        ctrlW = 200;
    } else if (dynamic_cast<ColorRow*>(ctrl)) {
        ctrlW = 110;
    } else if (dynamic_cast<LabelRow*>(ctrl)) {
        ctrlW = 200;
    } else if (dynamic_cast<Button*>(ctrl)) {
        ctrlW = std::max(90.0f, ctrl->MeasureWidth(0));
        ctrlH = ctrl->MeasureHeight(0);
    } else if (dynamic_cast<Slider*>(ctrl)) {
        // 滑块占图标+标题区右侧的剩余宽度（上限 240，避免挤压左侧说明文字）
        float reserved = padH + (icon.empty() ? 0 : 28) + 220 + padH;
        ctrlW = std::clamp(w - reserved, 160.0f, 240.0f);
    } else {
        ctrlW = ctrl->MeasureWidth(0);
    }

    // 控件以紧凑模式绘制（标签由行标题承担）
    if (auto* t = dynamic_cast<Toggle*>(ctrl)) t->compact = true;
    else if (auto* c = dynamic_cast<ComboBox*>(ctrl)) c->compact = true;
    else if (auto* s = dynamic_cast<Slider*>(ctrl)) s->compact = true;
    else if (auto* cr = dynamic_cast<ColorRow*>(ctrl)) cr->compact = true;
    else if (auto* lr = dynamic_cast<LabelRow*>(ctrl)) lr->compact = true;

    float ctrlX = x + w - padH - ctrlW;
    float ctrlY = y + (h - ctrlH) / 2;
    ctrl->Arrange(ctrlX, ctrlY, ctrlW, ctrlH);
}

void SettingRow::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void SettingRow::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    const float radius = 8;
    const float padH = 16;

    if (grouped) {
        // 分组模式：仅悬停时高亮整行（卡片底/边框/分隔线由分组卡片负责）
        if (hovered) {
            auto hoverBrush = MakeBrush(rt, LerpColor(ctx.Surface(), ctx.Text(),
                                                      ctx.isDarkMode ? 0.05f : 0.035f));
            rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), hoverBrush.Get());
        }
    } else {
        // 卡片背景（悬停微亮，Fluent 中性混色）
        D2D1_COLOR_F bg = ctx.Surface();
        if (hovered) bg = LerpColor(bg, ctx.Text(), ctx.isDarkMode ? 0.05f : 0.035f);
        auto bgBrush = MakeBrush(rt, bg);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), radius, radius),
            bgBrush.Get());

        // 卡片边框（极大淡化）
        auto borderBrush = MakeBrush(rt, ctx.isDarkMode
            ? D2D1::ColorF(1, 1, 1, 0.055f)
            : D2D1::ColorF(0, 0, 0, 0.05f));
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), radius, radius),
            borderBrush.Get(), 1);
    }

    // 文本区右边界（避免压到右侧控件）
    float textRight = x_ + w_ - 150;
    UIElement* ctrl = Control();
    if (ctrl && ctrl->IsVisible()) textRight = std::min(textRight, ctrl->X() - 12);

    // 图标（Segoe MDL2 Assets，整行垂直居中）
    float textX = x_ + padH;
    if (!icon.empty()) {
        if (auto iconFmt = MakeTextFormat(L"Segoe MDL2 Assets", 15)) {
            iconFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto iconBrush = MakeBrush(rt, ctx.TextSecondary());
            rt->DrawTextW(icon.c_str(), static_cast<UINT32>(icon.size()),
                          iconFmt.Get(), D2D1::RectF(x_ + padH, y_, x_ + padH + 26, y_ + h_),
                          iconBrush.Get());
        }
        textX += 28;
    }

    // 标题 + 说明（顶部对齐的文本块整体垂直居中）
    float textH = (!title.empty() ? 20 : 0) +
                  (!description.empty() ? (title.empty() ? 0 : 2) + 18 : 0);
    float textY = y_ + (h_ - textH) / 2;

    if (!title.empty()) {
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 14);
        if (fmt) {
            auto txtBrush = MakeBrush(rt, ctx.Text());
            std::wstring wtitle = Utf8ToWide(title);
            rt->DrawTextW(wtitle.c_str(), static_cast<UINT32>(wtitle.size()),
                          fmt.Get(), D2D1::RectF(textX, textY, textRight, textY + 20),
                          txtBrush.Get());
        }
    }
    if (!description.empty()) {
        float descY = textY + (title.empty() ? 0 : 22);
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
        if (fmt) {
            auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
            std::wstring wdesc = Utf8ToWide(description);
            rt->DrawTextW(wdesc.c_str(), static_cast<UINT32>(wdesc.size()),
                          fmt.Get(), D2D1::RectF(textX, descY, textRight, descY + 18),
                          txtBrush.Get());
        }
    }

    // 子控件
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

    // 选中/悬停背景（圆角矩形）——左侧留出明显空白，不贴导航边缘
    float insetX = x_ + 14;
    float insetW = w_ - 28;

    // Fluent 规范：选中项用中性色高亮（accent 仅保留指示条与图标）
    const float selMix = ctx.isDarkMode ? 0.09f : 0.06f;
    const float hoverMix = ctx.isDarkMode ? 0.06f : 0.04f;

    if (selected) {
        auto selBg = MakeBrush(rt, LerpColor(ctx.Surface(), ctx.Text(), selMix));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(insetX, y_ + 2, insetX + insetW, y_ + h_ - 2), 6, 6),
            selBg.Get());

        // 左侧强调色指示器（3px圆角条，内缩在药丸内，左侧露一点背景）
        auto accent = MakeBrush(rt, ctx.accent);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(insetX + 5, y_ + 8, insetX + 8, y_ + h_ - 8), 1.5f, 1.5f),
            accent.Get());
    } else if (hovered) {
        // 悬停反馈比选中态更弱，避免侧栏抢占内容区注意力。
        auto hoverBg = MakeBrush(rt, LerpColor(ctx.Surface(), ctx.Text(), hoverMix));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(insetX, y_ + 2, insetX + insetW, y_ + h_ - 2), 6, 6),
            hoverBg.Get());
    }

    // 图标（Segoe MDL2 Assets）
    float textX = x_ + 36;
    if (!icon.empty()) {
        auto iconFmt = MakeTextFormat(L"Segoe MDL2 Assets", 15);
        if (iconFmt) {
            iconFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto iconBrush = MakeBrush(rt, selected ? ctx.accent : ctx.TextSecondary());
            rt->DrawTextW(icon.c_str(), static_cast<UINT32>(icon.size()),
                          iconFmt.Get(), D2D1::RectF(x_ + 32, y_, x_ + 56, y_ + h_),
                          iconBrush.Get());
        }
        textX = x_ + 60;
    }

    // 文本
    auto fmt = MakeTextFormat(L"Segoe UI Variable", 14,
                              selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto txtBrush = MakeBrush(rt, selected ? ctx.Text() : ctx.TextSecondary());
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

    // 标签（紧凑模式下由 SettingRow 行标题承担）
    if (!compact) {
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
        if (fmt) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
            std::wstring wlabel = Utf8ToWide(label);
            rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                          fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 90, y_ + h_),
                          txtBrush.Get());
        }
    }

    // 28×28 圆角色块（更大更明显）
    const float swatchW = 28, swatchH = 28;
    const float swatchX = compact ? x_ : x_ + w_ - swatchW - 64;
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

    // 标签（紧凑模式下由 SettingRow 行标题承担）
    if (!compact) {
        auto fmt = MakeTextFormat(L"Segoe UI Variable", 12);
        if (fmt) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto txtBrush = MakeBrush(rt, ctx.TextSecondary());
            std::wstring wlabel = Utf8ToWide(label);
            rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                          fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ / 2, y_ + h_),
                          txtBrush.Get());
        }
    }

    // 值（右对齐）
    auto valFmt = MakeTextFormat(L"Segoe UI Variable", 12);
    if (valFmt) {
        valFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        valFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        auto valBrush = MakeBrush(rt, !readOnly ? ctx.accentLight : ctx.TextSecondary());
        std::wstring wval = Utf8ToWide(textValue);
        rt->DrawTextW(wval.c_str(), static_cast<UINT32>(wval.size()),
                      valFmt.Get(), D2D1::RectF(compact ? x_ : x_ + w_ / 2, y_, x_ + w_ - 16, y_ + h_),
                      valBrush.Get());
    }

    // 可点击暗示：值右侧的「›」小箭头（只读行不显示），提示点击可修改
    if (!readOnly) {
        auto chevronBrush = MakeBrush(rt, ctx.TextSecondary());
        const float cx = x_ + w_ - 9, cy = y_ + h_ / 2;
        rt->DrawLine(D2D1::Point2F(cx - 2.5f, cy - 4), D2D1::Point2F(cx + 0.5f, cy),
                     chevronBrush.Get(), 1.5f);
        rt->DrawLine(D2D1::Point2F(cx + 0.5f, cy), D2D1::Point2F(cx - 2.5f, cy + 4),
                     chevronBrush.Get(), 1.5f);
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
