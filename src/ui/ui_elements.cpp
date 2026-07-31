// SPDX-License-Identifier: GPL-3.0
// ui_elements.cpp - 具体控件实现
#include "ui/ui_elements.h"
#include "ui/color_utils.h"
#include "core/constants.h"

#include <cmath>

// Utf8ToWide 在 color_utils.h 中声明（moekoe 命名空间）
using moekoe::Utf8ToWide;

namespace moekoe::ui {

// ── 辅助：创建临时 TextLayout 测量文本 ──
// 注意：需要 IDWriteFactory，由 Draw/Measure 时的渲染上下文提供。
// 这里用静态缓存方式简化 Phase 1 实现。

// ═══════════════════════════════════════
// TextBlock
// ═══════════════════════════════════════

float TextBlock::MeasureWidth(float availWidth) {
    return availWidth;  // 文本占满可用宽度
}

float TextBlock::MeasureHeight(float availWidth) {
    switch (style) {
    case Style::Title:          return 28;
    case Style::SectionHeader:  return 28;
    case Style::Caption:        return 18;
    default:                    return 22;
    }
}

void TextBlock::Draw(ID2D1RenderTarget* rt) {
    if (!visible_ || text.empty()) return;

    // 获取或创建 TextFormat（简化实现：每次 Draw 时创建）
    // Phase 2 优化：缓存 TextFormat
    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }

    float fontSize = 14.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    switch (style) {
    case Style::Title:          fontSize = 20; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
    case Style::SectionHeader:  fontSize = 14; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
    case Style::Caption:        fontSize = 12; break;
    default:                    fontSize = 14; break;
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        fontSize, L"zh-CN", fmt.GetAddressOf());

    if (!fmt) return;

    // 颜色
    D2D1_COLOR_F color = {0.40f, 0.40f, 0.40f, 1.0f};  // 灰色说明文字
    switch (style) {
    case Style::Title:          color = {0.13f, 0.13f, 0.13f, 1.0f}; break;
    case Style::SectionHeader:  color = {0.20f, 0.20f, 0.20f, 1.0f}; break;
    default:                    color = {0.33f, 0.33f, 0.33f, 1.0f}; break;
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(color, brush.GetAddressOf());

    std::wstring wtext = Utf8ToWide(text);
    rt->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size()),
                  fmt.Get(), D2D1::RectF(x_, y_, x_ + w_, y_ + h_),
                  brush.Get());
}

// ═══════════════════════════════════════
// Button
// ═══════════════════════════════════════

float Button::MeasureWidth(float availWidth) {
    return 100;  // 固定宽度
}

float Button::MeasureHeight(float availWidth) {
    return 32;
}

void Button::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    D2D1_COLOR_F bgColor = isPrimary ? D2D1::ColorF(0x0078D4) :
                           isDanger  ? D2D1::ColorF(0xD13438) :
                                       D2D1::ColorF(0xE0E0E0);
    if (hovered && !pressed) {
        bgColor = isPrimary ? D2D1::ColorF(0x1A86D9) :
                   isDanger  ? D2D1::ColorF(0xE04B4F) :
                               D2D1::ColorF(0xD0D0D0);
    }
    if (pressed) {
        bgColor = isPrimary ? D2D1::ColorF(0x005A9E) :
                   isDanger  ? D2D1::ColorF(0xA4262C) :
                               D2D1::ColorF(0xC0C0C0);
    }

    D2D1_COLOR_F textColor = (isPrimary || isDanger)
                              ? D2D1::ColorF(1, 1, 1, 1)
                              : D2D1::ColorF(0.2f, 0.2f, 0.2f, 1);

    // 圆角矩形背景
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush, txtBrush;
    rt->CreateSolidColorBrush(bgColor, bgBrush.GetAddressOf());
    rt->CreateSolidColorBrush(textColor, txtBrush.GetAddressOf());

    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x_, y_, x_ + w_, y_ + h_), 6, 6};
    rt->FillRoundedRectangle(rr, bgBrush.Get());

    // 文本
    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13, L"zh-CN", fmt.GetAddressOf());
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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

// ═══════════════════════════════════════
// Toggle
// ═══════════════════════════════════════

float Toggle::MeasureWidth(float availWidth) {
    return availWidth;  // 占满可用宽度
}

float Toggle::MeasureHeight(float availWidth) {
    return 36;
}

void Toggle::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    // 标签
    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14, L"zh-CN", fmt.GetAddressOf());
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> txtBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.13f, 0.13f, 1), txtBrush.GetAddressOf());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 52, y_ + h_),
                      txtBrush.Get());
    }

    // 开关轨道
    const float trackW = 40, trackH = 20;
    const float trackX = x_ + w_ - trackW - 4;
    const float trackY = y_ + (h_ - trackH) / 2;
    const float radius = trackH / 2;

    D2D1_COLOR_F trackColor = value ? D2D1::ColorF(0x0078D4) : D2D1::ColorF(0.741f, 0.741f, 0.741f, 1.0f);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> trackBrush;
    rt->CreateSolidColorBrush(trackColor, trackBrush.GetAddressOf());
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH),
                          radius, radius),
        trackBrush.Get());

    // 滑块圆点
    const float thumbR = 8;
    const float thumbX = value ? (trackX + trackW - thumbR - 2) : (trackX + thumbR + 2);
    const float thumbY = trackY + trackH / 2;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> thumbBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), thumbBrush.GetAddressOf());
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR, thumbR),
                    thumbBrush.Get());

    // 悬停高亮
    if (hovered) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.04f), hoverBrush.GetAddressOf());
        rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), hoverBrush.Get());
    }
}

UIElement* Toggle::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    return this;
}

// ═══════════════════════════════════════
// Slider
// ═══════════════════════════════════════

float Slider::MeasureWidth(float availWidth) {
    return availWidth;
}

float Slider::MeasureHeight(float availWidth) {
    return 36;
}

void Slider::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }

    // 标签
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14, L"zh-CN", fmt.GetAddressOf());
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> txtBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.13f, 0.13f, 1), txtBrush.GetAddressOf());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 80, y_ + 22),
                      txtBrush.Get());
    }

    // 值
    char valBuf[64];
    snprintf(valBuf, sizeof(valBuf), "%.0f%s", value, suffix.c_str());
    Microsoft::WRL::ComPtr<IDWriteTextFormat> valFmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12, L"zh-CN", valFmt.GetAddressOf());
    if (valFmt) {
        valFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        valFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> valBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1), valBrush.GetAddressOf());
        std::wstring wval = Utf8ToWide(valBuf);
        rt->DrawTextW(wval.c_str(), static_cast<UINT32>(wval.size()),
                      valFmt.Get(), D2D1::RectF(x_ + w_ - 80, y_, x_ + w_, y_ + 22),
                      valBrush.Get());
    }

    // 轨道
    const float trackX = x_;
    const float trackY = y_ + 24;
    const float trackW = w_;
    const float trackH = 4;
    float ratio = (maxValue > minValue) ? (value - minValue) / (maxValue - minValue) : 0;
    ratio = std::clamp(ratio, 0.0f, 1.0f);

    // 背景轨道
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> trackBg, trackFg;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.878f, 0.878f, 0.878f, 1.0f), trackBg.GetAddressOf());
    rt->CreateSolidColorBrush(D2D1::ColorF(0x0078D4), trackFg.GetAddressOf());
    rt->FillRectangle(D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH), trackBg.Get());
    rt->FillRectangle(D2D1::RectF(trackX, trackY, trackX + trackW * ratio, trackY + trackH), trackFg.Get());

    // 滑块圆点
    const float thumbX = trackX + trackW * ratio;
    const float thumbY = trackY + trackH / 2;
    const float thumbR = 6;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> thumbBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0x0078D4), thumbBrush.GetAddressOf());
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR, thumbR), thumbBrush.Get());
}

UIElement* Slider::HitTest(float x, float y) {
    if (!visible_) return nullptr;
    // 只匹配轨道区域（y_ + 16 到底部），标签文本不响应点击
    if (x >= x_ && x <= x_ + w_ && y >= y_ + 16 && y <= y_ + h_)
        return this;
    return nullptr;
}

// ═══════════════════════════════════════
// ComboBox
// ═══════════════════════════════════════

float ComboBox::MeasureWidth(float availWidth) {
    return availWidth;
}

float ComboBox::MeasureHeight(float availWidth) {
    return dropped ? static_cast<float>(36 + items.size() * 30) : 36;
}

void ComboBox::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }

    // 标签
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14, L"zh-CN", fmt.GetAddressOf());
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> txtBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.13f, 0.13f, 1), txtBrush.GetAddressOf());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ / 2, y_ + 36),
                      txtBrush.Get());
    }

    // 下拉框
    const float boxX = x_ + w_ / 2;
    const float boxW = w_ / 2;
    const float boxH = 28;
    const float boxY = y_ + 4;

    // 背景
    D2D1_COLOR_F bgColor = hovered ? D2D1::ColorF(0.941f, 0.941f, 0.941f, 1.0f) : D2D1::ColorF(0.910f, 0.910f, 0.910f, 1.0f);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> boxBg;
    rt->CreateSolidColorBrush(bgColor, boxBg.GetAddressOf());
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(boxX, boxY, boxX + boxW, boxY + boxH), 4, 4),
                              boxBg.Get());

    // 当前选中项
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size())) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> itemFmt;
        dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13, L"zh-CN", itemFmt.GetAddressOf());
        if (itemFmt) {
            itemFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> valBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f, 1), valBrush.GetAddressOf());
            std::wstring witem = Utf8ToWide(items[selectedIndex]);
            rt->DrawTextW(witem.c_str(), static_cast<UINT32>(witem.size()),
                          itemFmt.Get(), D2D1::RectF(boxX + 8, boxY, boxX + boxW - 20, boxY + boxH),
                          valBrush.Get());
        }
    }

    // 下拉箭头 ▼
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> arrowBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1), arrowBrush.GetAddressOf());
    float ax = boxX + boxW - 14, ay = boxY + boxH / 2;
    rt->DrawLine(D2D1::Point2F(ax - 4, ay - 2), D2D1::Point2F(ax, ay + 2), arrowBrush.Get(), 1.5f);
    rt->DrawLine(D2D1::Point2F(ax, ay + 2), D2D1::Point2F(ax + 4, ay - 2), arrowBrush.Get(), 1.5f);

    // 下拉列表
    if (dropped) {
        float listY = boxY + boxH;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> listBg, itemBg;
        rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), listBg.GetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(0.910f, 0.910f, 0.910f, 1.0f), itemBg.GetAddressOf());
        rt->FillRectangle(D2D1::RectF(boxX, listY, boxX + boxW, listY + items.size() * 30), listBg.Get());

        Microsoft::WRL::ComPtr<IDWriteTextFormat> itemFmt;
        dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13, L"zh-CN", itemFmt.GetAddressOf());
        if (itemFmt) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> itemBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f, 1), itemBrush.GetAddressOf());
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                float iy = listY + i * 30;
                if (i == selectedIndex) {
                    rt->FillRectangle(D2D1::RectF(boxX, iy, boxX + boxW, iy + 30), itemBg.Get());
                }
                std::wstring witem = Utf8ToWide(items[i]);
                rt->DrawTextW(witem.c_str(), static_cast<UINT32>(witem.size()),
                              itemFmt.Get(), D2D1::RectF(boxX + 8, iy, boxX + boxW - 8, iy + 30),
                              itemBrush.Get());
            }
        }
    }
}

UIElement* ComboBox::HitTest(float x, float y) {
    if (!visible_) return nullptr;
    // 展开时，命中区域扩展到包含下拉选项
    if (dropped) {
        float dropH = static_cast<float>(items.size() * 30);
        float boxX = x_ + w_ / 2;
        float boxY = y_ + 4;
        if (x >= boxX && x <= x_ + w_ && y >= boxY && y <= boxY + 28 + dropH)
            return this;
        return nullptr;
    }
    // 未展开时，只匹配右侧下拉框区域（标签区域不响应点击）
    float boxX = x_ + w_ / 2;
    float boxY = y_ + 4;
    if (x >= boxX && x <= x_ + w_ && y >= boxY && y <= boxY + 28)
        return this;
    return nullptr;
}

// ═══════════════════════════════════════
// Card — 圆角容器
// ═══════════════════════════════════════

float Card::MeasureWidth(float availWidth) {
    return availWidth;  // 卡片占满可用宽度
}

float Card::MeasureHeight(float availWidth) {
    float contentH = 0;
    float innerW = availWidth - padding.left - padding.right;

    // 标题高度
    if (!title.empty()) {
        contentH += 24 + 8;  // 标题 + 标题与内容间距
    }

    // 子元素高度
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

    // 标题区域（由 Draw 绘制，不计入子元素）
    if (!title.empty()) {
        curY += 24 + 8;
    }

    // 排列子元素
    for (auto& child : children_) {
        if (!child->IsVisible()) continue;
        float childH = child->MeasureHeight(innerW);
        child->Arrange(innerX, curY, innerW, childH);
        curY += childH + gap;
    }
}

void Card::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    // 卡片背景
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardBg;
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), cardBg.GetAddressOf());
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x_, y_, x_ + w_, y_ + h_), cornerRadius, cornerRadius};
    rt->FillRoundedRectangle(rr, cardBg.Get());

    // 卡片边框
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardBorder;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.910f, 0.910f, 0.910f, 1.0f), cardBorder.GetAddressOf());
    rt->DrawRoundedRectangle(rr, cardBorder.Get(), 1);

    // 标题
    if (!title.empty()) {
        static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
        if (!dwFactory) {
            ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
        }
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14, L"zh-CN", fmt.GetAddressOf());
        if (fmt) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> txtBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f, 1), txtBrush.GetAddressOf());
            std::wstring wtitle = Utf8ToWide(title);
            rt->DrawTextW(wtitle.c_str(), static_cast<UINT32>(wtitle.size()),
                          fmt.Get(), D2D1::RectF(x_ + padding.left, y_ + padding.top,
                                                  x_ + w_ - padding.right, y_ + padding.top + 24),
                          txtBrush.Get());
        }
    }

    // 绘制子元素
    for (auto& child : children_) {
        if (child->IsVisible()) child->Draw(rt);
    }
}

// ═══════════════════════════════════════
// NavItem — 左侧导航项
// ═══════════════════════════════════════

float NavItem::MeasureWidth(float availWidth) {
    return availWidth;
}

float NavItem::MeasureHeight(float availWidth) {
    return 36;
}

void NavItem::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    // 选中背景
    if (selected) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selBg;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.910f, 0.910f, 0.910f, 1.0f), selBg.GetAddressOf());
        rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), selBg.Get());

        // 左侧色条
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent;
        rt->CreateSolidColorBrush(D2D1::ColorF(0x0078D4), accent.GetAddressOf());
        rt->FillRectangle(D2D1::RectF(x_, y_ + 4, x_ + 3, y_ + h_ - 4), accent.Get());
    } else if (hovered) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBg;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.941f, 0.941f, 0.941f, 1.0f), hoverBg.GetAddressOf());
        rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), hoverBg.Get());
    }

    // 文本
    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14, L"zh-CN", fmt.GetAddressOf());
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> txtBrush;
        rt->CreateSolidColorBrush(selected ? D2D1::ColorF(0x0078D4) : D2D1::ColorF(0.33f, 0.33f, 0.33f, 1),
                                  txtBrush.GetAddressOf());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_ + 16, y_, x_ + w_, y_ + h_),
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
    return 36;
}

void ColorRow::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    // 标签
    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14, L"zh-CN", fmt.GetAddressOf());
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> txtBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.13f, 0.13f, 1), txtBrush.GetAddressOf());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ - 52, y_ + h_),
                      txtBrush.Get());
    }

    // 色块
    const float swatchW = 36, swatchH = 24;
    const float swatchX = x_ + w_ - swatchW - 8;
    const float swatchY = y_ + (h_ - swatchH) / 2;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> colorBrush;
    rt->CreateSolidColorBrush(colorValue, colorBrush.GetAddressOf());
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(swatchX, swatchY, swatchX + swatchW, swatchY + swatchH), 4, 4),
        colorBrush.Get());

    // 边框
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.78f, 0.78f, 1), borderBrush.GetAddressOf());
    rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(swatchX, swatchY, swatchX + swatchW, swatchY + swatchH), 4, 4),
        borderBrush.Get(), 1);

    // 悬停高亮
    if (hovered) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.04f), hoverBrush.GetAddressOf());
        rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), hoverBrush.Get());
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
    return 36;
}

void LabelRow::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }

    // 标签
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14, L"zh-CN", fmt.GetAddressOf());
    if (fmt) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> txtBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.13f, 0.13f, 1), txtBrush.GetAddressOf());
        std::wstring wlabel = Utf8ToWide(label);
        rt->DrawTextW(wlabel.c_str(), static_cast<UINT32>(wlabel.size()),
                      fmt.Get(), D2D1::RectF(x_, y_, x_ + w_ / 2, y_ + h_),
                      txtBrush.Get());
    }

    // 值
    Microsoft::WRL::ComPtr<IDWriteTextFormat> valFmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13, L"zh-CN", valFmt.GetAddressOf());
    if (valFmt) {
        valFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        valFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> valBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1), valBrush.GetAddressOf());
        std::wstring wval = Utf8ToWide(textValue);
        rt->DrawTextW(wval.c_str(), static_cast<UINT32>(wval.size()),
                      valFmt.Get(), D2D1::RectF(x_ + w_ / 2, y_, x_ + w_ - 16, y_ + h_),
                      valBrush.Get());
    }

    // 悬停高亮
    if (hovered && !readOnly) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.04f), hoverBrush.GetAddressOf());
        rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), hoverBrush.Get());
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
    return 70;  // 两行：色块 + 名称
}

void ThemePresets::Draw(ID2D1RenderTarget* rt) {
    if (!visible_ || presets.empty()) return;

    const int cols = static_cast<int>(presets.size());
    const float gap = 8;
    const float swatchW = 36;
    const float swatchH = 36;
    const float totalW = cols * swatchW + (cols - 1) * gap;
    const float startX = x_ + (w_ - totalW) / 2;

    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> nameFmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        11, L"zh-CN", nameFmt.GetAddressOf());

    for (int i = 0; i < cols; ++i) {
        const float sx = startX + i * (swatchW + gap);
        const float sy = y_ + 4;

        // 色块：上半高亮色，下半普通色
        const float halfH = swatchH / 2;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hlBrush, nlBrush;
        rt->CreateSolidColorBrush(presets[i].hlColor, hlBrush.GetAddressOf());
        rt->CreateSolidColorBrush(presets[i].nlColor, nlBrush.GetAddressOf());

        // 上半圆角
        D2D1_ROUNDED_RECT topRr = {D2D1::RectF(sx, sy, sx + swatchW, sy + halfH), 6, 6};
        rt->FillRoundedRectangle(topRr, hlBrush.Get());

        // 下半圆角
        D2D1_ROUNDED_RECT botRr = {D2D1::RectF(sx, sy + halfH, sx + swatchW, sy + swatchH), 6, 6};
        rt->FillRoundedRectangle(botRr, nlBrush.Get());

        // 选中边框
        if (i == selectedIndex) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0x0078D4), selBrush.GetAddressOf());
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(sx - 1, sy - 1, sx + swatchW + 1, sy + swatchH + 1), 6, 6),
                selBrush.Get(), 2);
        }

        // 悬停边框
        if (i == hoveredIndex && i != selectedIndex) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.6f, 0.6f, 0.6f, 1), hoverBrush.GetAddressOf());
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(sx - 1, sy - 1, sx + swatchW + 1, sy + swatchH + 1), 6, 6),
                hoverBrush.Get(), 1);
        }

        // 名称
        if (nameFmt) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> nameBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1), nameBrush.GetAddressOf());
            nameFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            std::wstring wname = Utf8ToWide(presets[i].name);
            rt->DrawTextW(wname.c_str(), static_cast<UINT32>(wname.size()),
                          nameFmt.Get(), D2D1::RectF(sx - 8, sy + swatchH + 2, sx + swatchW + 8, sy + swatchH + 20),
                          nameBrush.Get());
        }
    }
}

UIElement* ThemePresets::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    return this;
}

} // namespace moekoe::ui
