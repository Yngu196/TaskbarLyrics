// SPDX-License-Identifier: GPL-3.0
// nav_view.cpp - 左侧导航面板实现
#include "ui/nav_view.h"

namespace moekoe::ui {

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
    return availWidth;  // 占满分配的导航宽度
}

float NavView::MeasureHeight(float availWidth) {
    float h = 50;  // 顶部标题区域
    for (auto& child : children_) {
        h += child->MeasureHeight(availWidth);
    }
    return h;
}

void NavView::Draw(ID2D1RenderTarget* rt) {
    if (!visible_) return;

    // 导航背景
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> navBg;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.961f, 0.961f, 0.961f, 1.0f), navBg.GetAddressOf());
    rt->FillRectangle(D2D1::RectF(x_, y_, x_ + w_, y_ + h_), navBg.Get());

    // 标题 "TaskbarLyrics"
    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFmt;
    dwFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16, L"zh-CN", titleFmt.GetAddressOf());
    if (titleFmt) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> titleBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.13f, 0.13f, 1), titleBrush.GetAddressOf());
        rt->DrawTextW(L"TaskbarLyrics", 14,
                      titleFmt.Get(), D2D1::RectF(x_ + 16, y_ + 12, x_ + w_ - 16, y_ + 44),
                      titleBrush.Get());
    }

    // 导航项
    float itemY = y_ + 50;
    for (auto& child : children_) {
        if (child->IsVisible()) {
            child->Arrange(x_, itemY, w_, child->MeasureHeight(w_));
            child->Draw(rt);
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
