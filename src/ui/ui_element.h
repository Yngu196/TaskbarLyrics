// SPDX-License-Identifier: GPL-3.0
// ui_element.h - UI Framework 核心基类
//
// 职责:
//   - UIElement 控件树基类（树结构 + Measure/Arrange/Draw/HitTest）
//   - 布局约束结构体（Margin / Padding）
//   - 绘制上下文传递暗色主题状态
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

// ── 绘制上下文 ──
// 每帧绘制时传递给控件的共享状态（暗色模式、主题色等）

struct DrawContext {
    bool isDarkMode{false};
    // 暗色主题颜色（Linear 紫色风格）
    D2D1_COLOR_F bg{0.102f, 0.102f, 0.180f, 1.0f};           // #1A1A2E
    D2D1_COLOR_F surface{0.145f, 0.145f, 0.251f, 1.0f};       // #252540
    D2D1_COLOR_F surfaceHover{0.180f, 0.180f, 0.314f, 1.0f};  // #2E2E50
    D2D1_COLOR_F border{0.227f, 0.227f, 0.361f, 1.0f};        // #3A3A5C
    D2D1_COLOR_F text{0.933f, 0.933f, 0.941f, 1.0f};          // #EEEEF0
    D2D1_COLOR_F textSecondary{0.627f, 0.627f, 0.722f, 1.0f}; // #A0A0B8
    D2D1_COLOR_F accent{0.424f, 0.361f, 0.906f, 1.0f};        // #6C5CE7
    D2D1_COLOR_F accentHover{0.498f, 0.439f, 0.941f, 1.0f};   // #7F70F0
    D2D1_COLOR_F accentLight{0.635f, 0.608f, 0.996f, 1.0f};   // #A29BFE
    D2D1_COLOR_F danger{1.0f, 0.420f, 0.420f, 1.0f};          // #FF6B6B
    // 亮色主题颜色
    D2D1_COLOR_F lightBg{1.0f, 1.0f, 1.0f, 1.0f};
    D2D1_COLOR_F lightSurface{0.961f, 0.969f, 0.976f, 1.0f};  // #F5F7F9
    D2D1_COLOR_F lightBorder{0.878f, 0.894f, 0.910f, 1.0f};   // #E0E4E8
    D2D1_COLOR_F lightText{0.102f, 0.102f, 0.180f, 1.0f};     // #1A1A2E
    D2D1_COLOR_F lightTextSecondary{0.420f, 0.447f, 0.502f, 1.0f}; // #6B7280

    // 便捷访问器：根据当前模式返回颜色
    D2D1_COLOR_F Bg() const { return isDarkMode ? bg : lightBg; }
    D2D1_COLOR_F Surface() const { return isDarkMode ? surface : lightSurface; }
    D2D1_COLOR_F SurfaceHover() const { return surfaceHover; }  // 暗色模式专用
    D2D1_COLOR_F Border() const { return isDarkMode ? border : lightBorder; }
    D2D1_COLOR_F Text() const { return isDarkMode ? text : lightText; }
    D2D1_COLOR_F TextSecondary() const { return isDarkMode ? textSecondary : lightTextSecondary; }
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

    // 带绘制上下文的渲染（新接口，子类可覆写以获取暗色模式等状态）
    virtual void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) { Draw(rt); }

    // ── 命中测试 ──
    // 返回最深层被命中的元素（可以是自身或子元素），nullptr 表示未命中
    virtual UIElement* HitTest(float x, float y);

    // ── 可见性 ──
    void SetVisible(bool v) { visible_ = v; }
    bool IsVisible() const { return visible_; }

    // ── 动画 Tick ──
    // 每帧调用，推进控件内部动画。返回 true 表示有动画仍在播放需要重绘。
    // 默认实现：递归遍历子元素的 TickAnimation
    virtual bool TickAnimation(float deltaMs);

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
