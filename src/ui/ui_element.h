// SPDX-License-Identifier: GPL-3.0
// ui_element.h - UI Framework 核心基类
//
// 职责:
//   - UIElement 控件树基类（树结构 + Measure/Arrange/Draw/HitTest）
//   - 布局约束结构体（Margin / Padding）
//   - 所有具体控件（TextBlock/Button/Toggle/Slider/ComboBox/Card/NavItem）
//     继承此基类
//
#pragma once

#include <d2d1.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace moekoe::ui {

// ── 布局约束 ──

struct Margin {
    float left{0}, top{0}, right{0}, bottom{0};
};

struct Padding {
    float left{0}, top{0}, right{0}, bottom{0};
};

// ── UIElement 基类 ──

class UIElement {
public:
    virtual ~UIElement() = default;

    // ── 树结构 ──
    UIElement* Parent() const { return parent_; }
    void AddChild(std::unique_ptr<UIElement> child);
    const std::vector<std::unique_ptr<UIElement>>& Children() const { return children_; }

    // ── 布局 ──
    // 测量期望宽度（给定可用宽度，返回自身需要的宽度）
    virtual float MeasureWidth(float availWidth) = 0;
    // 测量期望高度（给定可用宽度，返回自身需要的高度）
    virtual float MeasureHeight(float availWidth) = 0;
    // 确定最终位置和尺寸
    virtual void Arrange(float x, float y, float w, float h);

    float X() const { return x_; }
    float Y() const { return y_; }
    float Width() const { return w_; }
    float Height() const { return h_; }

    // ── 渲染 ──
    virtual void Draw(ID2D1RenderTarget* rt) = 0;

    // ── 命中测试 ──
    // 返回最深层被命中的元素（可以是自身或子元素），nullptr 表示未命中
    virtual UIElement* HitTest(float x, float y);

    // ── 可见性 ──
    void SetVisible(bool v) { visible_ = v; }
    bool IsVisible() const { return visible_; }

    // ── 交互回调 ──
    using ClickHandler = std::function<void(UIElement*)>;
    void SetOnClick(ClickHandler h) { onClick_ = std::move(h); }
    ClickHandler& OnClick() { return onClick_; }

    // ── 标识 ──
    std::string id;

    // ── 外边距 ──
    Margin margin;

protected:
    UIElement* parent_{nullptr};
    std::vector<std::unique_ptr<UIElement>> children_;
    float x_{0}, y_{0}, w_{0}, h_{0};
    bool visible_{true};
    ClickHandler onClick_;

    // 辅助：判断点是否在自身区域内
    bool ContainsPoint(float px, float py) const {
        return px >= x_ && px < x_ + w_ && py >= y_ && py < y_ + h_;
    }
};

} // namespace moekoe::ui
