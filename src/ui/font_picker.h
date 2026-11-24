// SPDX-License-Identifier: Apache-2.0
// font_picker.h - 设置界面内置字体选择器（D2D 自绘模态浮层）
//
// 替代 Win32 ChooseFontW 对话框：系统对话框只有"字体族"生效，
// 且风格与自绘设置界面不一致。本组件列出系统已安装字体族，
// 每行以该字体自渲染预览，风格与 ColorPickerPopup 一致。
// 支持搜索过滤、键盘选择（↑/↓/回车/Esc）。
#pragma once

#include "ui/color_picker.h"  // D2DThemeColors

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <windows.h>

namespace moekoe {

// 释放字体选择器的共享 DWrite 工厂与字体族格式缓存（关闭设置窗口时调用）
void ReleaseFontPickerCaches();

class FontPickerPopup {
public:
    enum class ActionResult { None, Confirmed, Cancelled };
    enum class KeyResult { None, Redraw, Confirm, Cancel };

    bool IsActive() const { return active_; }

    // 打开弹窗。currentFamily: 当前字体族（用于初始选中并滚动到位）
    void Activate(HWND hwnd, const std::wstring& currentFamily, int titleBarHeight, float dpiScale);

    void Deactivate(HWND hwnd);

    // 返回 Confirmed 时 outFamily 为选中的字体族（调用方负责写回并 Deactivate）
    ActionResult HandleMouseDown(int x, int y, std::wstring* outFamily);
    // 返回 true 表示悬停状态发生变化（需要重绘）
    bool HandleMouseMove(int x, int y);
    void HandleWheel(int delta);

    // 键盘输入（WM_KEYDOWN / WM_CHAR），返回处理结果
    KeyResult HandleKey(UINT msg, WPARAM wParam);
    // 当前选中字体（无有效选择返回 false）
    bool GetSelected(std::wstring* outFamily) const;

    void Draw(ID2D1RenderTarget* rt, const D2DThemeColors& theme,
              ID2D1SolidColorBrush* textSecondaryBrush, int titleBarHeight);

private:
    // 枚举系统已安装字体族（DirectWrite 系统字体集合，按用户本地化名称）
    static const std::vector<std::wstring>& SystemFamilies();

    // 按搜索词过滤（区分大小写无关子串匹配），重建 filtered_ 并 clamp 滚动
    void UpdateFilter();
    // 按当前客户区计算面板/列表/按钮矩形（DIP）
    void ComputeRects(int clientW, int clientH);
    // 查询客户区尺寸（DIP）
    void ClientSize(int* clientW, int* clientH) const;
    // 列表滚动上限
    int MaxScroll() const;

    bool active_{false};
    HWND hwnd_{nullptr};
    float dpiScale_{1.0f};
    int titleBarHeight_{0};  // 内容区起始偏移（Activate 时记录，命中测试与绘制共用）

    // 搜索
    std::wstring searchText_;
    bool searchFocused_{false};
    std::vector<int> filtered_;  // 匹配的字体族在 SystemFamilies() 中的索引

    int selected_{-1};   // 当前选中（SystemFamilies 中的原始索引）
    int hover_{-1};      // 悬停行（原始索引）
    int scroll_{0};      // 列表滚动偏移（px，DIP）
    int contentHeight_{0};

    RECT panelRect_{};    // 面板（DIP 整数近似）
    RECT searchRect_{};   // 搜索框
    RECT listRect_{};     // 列表区域
    RECT okRect_{};       // 确定按钮
    RECT cancelRect_{};   // 取消按钮
    RECT closeRect_{};    // 右上角 ×
    bool hoverOk_{false};
    bool hoverCancel_{false};
    bool hoverClose_{false};
    bool hoverSearch_{false};
};

} // namespace moekoe
