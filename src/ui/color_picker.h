// SPDX-License-Identifier: GPL-3.0
// color_picker.h - D2D 颜色选择器弹窗
//
// 独立的颜色选择器 UI 组件，封装色板（Hue×Sat 网格）+ 亮度条 +
// 预览色块 + Hex 显示 + 确认按钮的全部交互逻辑。
// 无状态依赖，通过方法参数接收所需的 D2D 资源和窗口句柄。
//
#pragma once

#include "ui/color_utils.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <string>
#include <vector>

namespace moekoe {

// 颜色主题（与 d2d_settings_window.h 的 ThemeColors 保持一致）
struct D2DThemeColors {
    D2D1_COLOR_F bg;
    D2D1_COLOR_F surface;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentHover;
};

// ── D2D 颜色选择器弹窗 ──
//
// 使用方式:
//   1. 检测到 ColorRow 点击时调用 Activate() 激活
//   2. OnMouseDown / OnMouseMove 委托给对应的 Handle* 方法
//   3. DrawAll() 中调用 Draw() 绘制
//   4. 用户确认后从 HandleMouseDown 获取结果，写回目标 Control
//
class ColorPickerPopup {
public:
    // 鼠标事件处理结果
    enum class ActionResult {
        None,       // 未消费（弹窗未激活或点击在弹窗外）
        Handled,    // 已消费（色板/亮度条拖动）
        Confirmed,  // 点击确定按钮
        Cancelled,  // 点击弹窗外取消
    };

    // ── 状态查询 ──
    bool IsActive() const { return active_; }

    // ── 生命周期 ──
    // 激活弹窗：根据 hwnd 客户区计算布局，将 initialColor 转换为 HSL
    void Activate(HWND hwnd, const D2D1_COLOR_F& initialColor, int titleBarHeight);
    // 关闭弹窗（释放鼠标捕获）
    void Deactivate(HWND hwnd);

    // ── 输入事件 ──
    // 鼠标按下。若确认，newColor/newHex 返回新颜色值。
    ActionResult HandleMouseDown(int x, int y,
                                 D2D1_COLOR_F* newColor, std::string* newHex);
    // 鼠标移动（左键拖动色板/亮度条）。仅当 leftButtonDown 且弹窗激活时有效。
    void HandleMouseMove(int x, int y, bool leftButtonDown);

    // ── 绘制 ──
    // scrollOffset 用于调整弹窗坐标偏移（与主窗口滚动同步）
    void Draw(ID2D1RenderTarget* rt,
              bool isDarkMode,
              const D2DThemeColors& theme,
              IDWriteTextFormat* valueFmt,
              IDWriteTextFormat* hintFmt,
              ID2D1SolidColorBrush* textSecondaryBrush,
              int scrollOffset);

private:
    bool active_{false};
    float hue_{0.0f};          // 色相 0-360
    float sat_{0.0f};          // 饱和度 0-1
    float lum_{0.5f};          // 亮度 0-1（默认 0.5，防止色板点击亮度为 0 的 bug）
    RECT popupRect_{};         // 弹窗区域
    RECT gridRect_{};          // 色板网格区域
    RECT barRect_{};           // 亮度条区域
    RECT previewRect_{};       // 预览色块区域
    RECT confirmRect_{};       // 确定按钮区域
};

} // namespace moekoe
