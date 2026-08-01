// SPDX-License-Identifier: GPL-3.0
// nav_view.cpp - 左侧导航面板实现（暗色 Fluent Design）
#include "ui/nav_view.h"
#include "ui/color_utils.h"

using moekoe::Utf8ToWide;

namespace moekoe::ui {

// 辅助函数（与 ui_elements.cpp 中的相同）
static Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> MakeBrush(
    ID2D1RenderTarget* rt, const D2D1_COLOR_F& color)
{
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(color, brush.GetAddressOf());
    return brush;
}

static Microsoft::WRL::ComPtr<IDWriteFactory>& GetDWriteFactory() {
    static Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    if (!factory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
    }
    return factory;
}

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

void NavView::BuildItems(const std::vector<std::string>& labels) {
    children_.clear();
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        auto item = std::make_unique<NavItem>();
        item->label = labels[i];
        item->pageIndex = i;
        item->selected = (i == selectedIndex_);
        item->id = "nav_" + std::to_string(i);
        item->SetOnClick([this](UIElement* el) {
            auto* ni = static_cast<NavItem*>(el);
            SetSelectedIndex(ni->pageIndex);
            if (onPageChange_) onPageChange_(ni->pageIndex);
        });
        AddChild(std::move(item));
    }
}

void NavView::SetSelectedIndex(int idx) {
    selectedIndex_ = idx;
    for (auto& child : children_) {
        auto* ni = static_cast<NavItem*>(child.get());
        ni->selected = (ni->pageIndex == idx);
    }
}

float NavView::MeasureWidth(float availWidth) {
    return availWidth;
}

float NavView::MeasureHeight(float availWidth) {
    float h = 50;
    for (auto& child : children_) {
        h += child->MeasureHeight(availWidth);
    }
    return h;
}

void NavView::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void NavView::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 导航背景（比内容区更深的主题色，与内容区形成对比）
    // 通过降低 bg 亮度 40% 实现
    D2D1_COLOR_F navBgColor = D2D1::ColorF(
        ctx.bg.r * 0.6f,
        ctx.bg.g * 0.6f,
        ctx.bg.b * 0.6f, 1.0f);
    auto navBg = MakeBrush(rt, navBgColor);
    rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), navBg.Get());

    // 右侧分隔线（半透明紫调）
    auto sepBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 0.06f));
    rt->DrawLine(D2D1::Point2F(x_ + w_ - 0.5f, y_), D2D1::Point2F(x_ + w_ - 0.5f, y_ + h_),
                 sepBrush.Get(), 1);

    // 标题 "TaskbarLyrics"（紫色强调色）
    auto titleFmt = MakeTextFormat(L"Segoe UI Variable", 16, DWRITE_FONT_WEIGHT_BOLD);
    if (titleFmt) {
        auto titleBrush = MakeBrush(rt, ctx.accent);  // 使用紫色
        rt->DrawTextW(L"TaskbarLyrics", 14,
                      titleFmt.Get(), D2D1::RectF(x_ + 16, y_ + 12, x_ + w_ - 16, y_ + 44),
                      titleBrush.Get());
    }

    // 导航项
    float itemY = y_ + 50;
    for (auto& child : children_) {
        if (child->IsVisible()) {
            child->Arrange(x_, itemY, w_, child->MeasureHeight(w_));
            child->Draw(rt, ctx);
            itemY += child->MeasureHeight(w_);
        }
    }
}

UIElement* NavView::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    for (auto& child : children_) {
        UIElement* hit = child->HitTest(x, y);
        if (hit) return hit;
    }
    return this;
}

} // namespace moekoe::ui
