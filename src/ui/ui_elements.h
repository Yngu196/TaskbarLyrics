// SPDX-License-Identifier: Apache-2.0
// ui_elements.h - 具体控件声明
//
// 7 个设置页需要的控件:
//   TextBlock  - 静态文本/标题
//   Button     - 可点击按钮
//   Toggle     - 开关
//   Slider     - 滑块
//   ComboBox   - 下拉选择
//   Card       - 圆角容器（自动垂直布局子元素）
//   NavItem    - 左侧导航项
//
#pragma once

#include "ui/ui_element.h"
#include "ui/ui_animation.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace moekoe::ui {

// 共享 DWrite 工厂（惰性创建，空置后可重建；设置窗口关闭时释放以回收内存）
Microsoft::WRL::ComPtr<IDWriteFactory>& GetSharedDWriteFactory();
void ReleaseSharedUIDWriteFactory();

// ═══════════════════════════════════════
// TextBlock — 静态文本
// ═══════════════════════════════════════
class TextBlock : public UIElement {
public:
    enum class Style { Title, Body, Caption, SectionHeader };

    std::string text;
    Style style{Style::Body};

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
};

// ═══════════════════════════════════════
// Button — 可点击按钮
// ═══════════════════════════════════════
class Button : public UIElement {
public:
    std::string text;
    bool isPrimary{false};
    bool isDanger{false};

    // 运行时状态
    bool hovered{false};
    bool pressed{false};

    // 动画
    Animation<float> hoverAnim_;  // 0→1 悬停动画
    float hoverT_{0.0f};          // 当前悬停动画进度（由 TickAnimation 更新）

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;
    bool TickAnimation(float deltaMs) override;
};

// ═══════════════════════════════════════
// Toggle — 开关
// ═══════════════════════════════════════
class Toggle : public UIElement {
public:
    std::string label;
    bool value{false};
    bool compact{false};  // 紧凑模式：仅绘制开关本体（标签由 SettingRow 行标题承担）

    // 运行时状态
    bool hovered{false};

    // 动画
    Animation<float> thumbAnim_;  // 0（关闭）→1（开启）滑块位置
    float thumbPos_{0.0f};        // 当前显示的滑块位置（由 TickAnimation 更新）

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;
    bool TickAnimation(float deltaMs) override;
};

// ═══════════════════════════════════════
// Slider — 滑块
// ═══════════════════════════════════════
class Slider : public UIElement {
public:
    std::string label;
    float minValue{0}, maxValue{100}, value{50};
    std::string suffix;
    bool compact{false};  // 紧凑模式：仅绘制数值+轨道（标签由 SettingRow 行标题承担）

    // 运行时状态
    bool hovered{false};
    bool dragging{false};

    // 动画
    Animation<float> hoverAnim_;  // 手柄 hover 动画
    float hoverT_{0.0f};          // 当前 hover 动画进度（由 TickAnimation 更新）

    // 缓存渐变画刷（跨帧复用，避免每帧创建）
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> fillGradientBrush_;

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;
    bool TickAnimation(float deltaMs) override;
};

// ═══════════════════════════════════════
// ComboBox — 下拉选择
// ═══════════════════════════════════════
class ComboBox : public UIElement {
public:
    std::string label;
    std::vector<std::string> items;
    int selectedIndex{0};
    bool compact{false};  // 紧凑模式：下拉框占满整个元素宽度（标签由 SettingRow 行标题承担）

    // 下拉框在元素内的矩形（compact 时占满，否则占右半），供弹窗绘制/命中使用
    void GetBoxRect(float* boxX, float* boxW) const;

    // 运行时状态
    bool hovered{false};
    bool dropped{false};
    int hoveredDropIndex_{-1};  // 下拉列表中悬停的项索引

    // 悬停过渡动画（Win11 柔和反馈）
    Animation<float> hoverAnim_;
    float hoverT_{0.0f};

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;
    bool TickAnimation(float deltaMs) override;
};

// ═══════════════════════════════════════
// Card — 圆角容器（自动垂直布局子元素）
// ═══════════════════════════════════════
class Card : public UIElement {
public:
    std::string title;      // 卡片标题（可选，空串则不显示）
    Padding padding{20, 20, 20, 20};  // 内边距
    float cornerRadius{12};
    float gap{12};          // 子元素间距

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Arrange(float x, float y, float w, float h) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
};

// ═══════════════════════════════════════
// SettingRow — 独立设置行卡片（参考 Windows 11 设置风格）
//
// 每行是一张独立圆角卡片：左侧「图标 + 标题 + 灰色说明」，
// 右侧托管一个控件（Toggle/ComboBox/Slider/ColorRow/LabelRow/Button）。
// 控件以 compact 模式绘制（只画控件本体，标签由行标题承担）。
// 命中测试会优先命中子控件；点击行空白区可由窗口层转发给行内控件。
// ═══════════════════════════════════════
class SettingRow : public UIElement {
public:
    std::string title;        // 行标题（左上）
    std::string description;  // 灰色说明（标题下方，可选）
    std::wstring icon;        // Segoe MDL2 Assets 图标码点（空=无图标）

    // 行内托管的控件（第一个子元素，可能为空）
    UIElement* Control();

    // 分组模式：嵌入分组卡片（Card padding=0 gap=0）中时，
    // 行自身不画卡片底/边框（分组卡片负责），仅保留悬停高亮
    bool grouped{false};

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Arrange(float x, float y, float w, float h) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;

    // 运行时状态
    bool hovered{false};
};

// ═══════════════════════════════════════
// NavItem — 左侧导航项
// ═══════════════════════════════════════
class NavItem : public UIElement {
public:
    std::string label;
    std::wstring icon;      // Segoe MDL2 Assets 图标码点（空=无图标）
    int pageIndex{0};       // 对应的页面索引
    bool selected{false};

    // 运行时状态
    bool hovered{false};

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;
};

// ═══════════════════════════════════════
// ColorRow — 颜色选择行
// ═══════════════════════════════════════
class ColorRow : public UIElement {
public:
    std::string label;
    D2D1_COLOR_F colorValue{0, 0, 0, 1};
    std::string textValue;  // Hex 格式，如 "#4CC2FF"
    bool compact{false};    // 紧凑模式：仅绘制色块+Hex（标签由 SettingRow 行标题承担）

    // 运行时状态
    bool hovered{false};

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;
};

// ═══════════════════════════════════════
// LabelRow — 只读标签行（可点击触发操作）
// ═══════════════════════════════════════
class LabelRow : public UIElement {
public:
    std::string label;
    std::string textValue;
    bool readOnly{false};
    bool compact{false};  // 紧凑模式：仅绘制值+箭头（标签由 SettingRow 行标题承担）

    // 运行时状态
    bool hovered{false};

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;
};

// ═══════════════════════════════════════
// ThemePresets — 预设主题色板
// ═══════════════════════════════════════
struct ThemePreset {
    D2D1_COLOR_F hlColor;  // 高亮色
    D2D1_COLOR_F nlColor;  // 普通色
    std::string  name;     // 预设名称
};

class ThemePresets : public UIElement {
public:
    std::vector<ThemePreset> presets;
    int selectedIndex{-1};

    // 运行时状态
    int hoveredIndex{-1};

    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;
};

} // namespace moekoe::ui
