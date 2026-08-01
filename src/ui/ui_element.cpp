// SPDX-License-Identifier: GPL-3.0
// ui_element.cpp - UIElement 基类实现
#include "ui/ui_element.h"

namespace moekoe::ui {

void UIElement::AddChild(std::unique_ptr<UIElement> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
}

void UIElement::Arrange(float x, float y, float w, float h) {
    x_ = x; y_ = y; w_ = w; h_ = h;
}

UIElement* UIElement::HitTest(float x, float y) {
    if (!visible_ || !ContainsPoint(x, y)) return nullptr;
    // 逆序遍历子元素（后绘制的在上层，优先命中）
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        UIElement* hit = (*it)->HitTest(x, y);
        if (hit) return hit;
    }
    return this;  // 自身被命中
}

bool UIElement::TickAnimation(float deltaMs) {
    bool anyActive = false;
    for (auto& child : children_) {
        if (child->IsVisible()) {
            anyActive = child->TickAnimation(deltaMs) || anyActive;
        }
    }
    return anyActive;
}

} // namespace moekoe::ui
