// SPDX-License-Identifier: GPL-3.0
// nav_view.h - 左侧导航面板
//
// 职责:
//   - 管理导航项列表
//   - 处理页面切换逻辑
//   - 高亮当前选中项
//
#pragma once

#include "ui/ui_elements.h"

#include <functional>
#include <vector>

namespace moekoe::ui {

class NavView : public UIElement {
public:
    using PageChangeHandler = std::function<void(int pageIndex)>;

    // 构建导航项（icons 可选，为 Segoe MDL2 Assets 码点）
    void BuildItems(const std::vector<std::string>& labels,
                    const std::vector<std::wstring>& icons = {});

    // 当前选中页
    int SelectedIndex() const { return selectedIndex_; }
    void SetSelectedIndex(int idx);

    // 页面切换回调
    void SetOnPageChange(PageChangeHandler h) { onPageChange_ = std::move(h); }

    // UIElement 接口
    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
    UIElement* HitTest(float x, float y) override;

private:
    int selectedIndex_{0};
    PageChangeHandler onPageChange_;
};

} // namespace moekoe::ui
