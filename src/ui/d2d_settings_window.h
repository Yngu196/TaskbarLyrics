// SPDX-License-Identifier: GPL-3.0
// d2d_settings_window.h - Direct2D 原生自绘设置界面
//
// 职责:
//   - 使用 Direct2D + DirectWrite 绘制现代化设置界面（圆角、阴影、毛玻璃背景）
//   - 提供与 settings.html 功能完全一致的设置项
//   - 仅保留原生 Direct2D 设置界面
//   - 颜色选择器弹窗已拆分至 color_picker.h/.cpp
//
#pragma once

#include "ui/color_picker.h"
#include "config/config.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace moekoe {

// ThemeColors 别名：与 color_picker.h 中的 D2DThemeColors 类型保持一致
using ThemeColors = D2DThemeColors;

class D2DSettingsWindow {
public:
    using ConfigChangedCallback = std::function<void(const Config&)>;

    D2DSettingsWindow();
    ~D2DSettingsWindow();

    D2DSettingsWindow(const D2DSettingsWindow&) = delete;
    D2DSettingsWindow& operator=(const D2DSettingsWindow&) = delete;

    // 显示设置窗口（非模态）
    bool Show(HINSTANCE hInstance, HWND parent, const Config& currentConfig);

    // 注册回调
    void OnConfigChanged(ConfigChangedCallback cb) { onConfigChanged_ = std::move(cb); }

    // 是否正在显示
    bool IsVisible() const;

    // 关闭窗口
    void Close();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ═══════════════════════════════
    // D2D 初始化 / 清理
    // ═══════════════════════════════
    bool InitD2D();
    void ShutdownD2D();

    // ═══════════════════════════════
    // 控件系统
    // ═══════════════════════════════

    enum class CtrlType {
        SectionHeader,
        LabelRow,       // label + text input (或只读)
        ToggleRow,      // label + toggle switch
        SliderRow,      // label + slider
        ColorRow,       // label + color button + hex input
        DropdownRow,    // label + dropdown
        ButtonRow,      // label + button
        ThemePresets,   // 预设主题按钮组
        HintText,       // 灰色提示文字
        Spacer,         // 分隔间距
    };

    struct Control {
        CtrlType   type;
        std::string id;          // 唯一标识符
        std::string label;       // 左侧标签文字
        RECT       rect{};       // 控件区域（相对于内容区）
        bool       visible{true};

        // 文本输入
        std::string textValue;
        int         textMaxLen{32};
        bool        readOnly{false};

        // 开关
        bool toggleValue{false};

        // 滑块
        float sliderMin{0}, sliderMax{100}, sliderValue{50};
        std::string sliderSuffix;

        // 颜色
        D2D1_COLOR_F colorValue{0,0,0,1};

        // 下拉框
        std::vector<std::string> dropdownItems;
        int dropdownSelected{0};

        // 按钮
        std::string buttonText;
        bool isPrimary{false};
        bool isDanger{false};

        // 主题预设
        struct ThemePreset {
            D2D1_COLOR_F hlColor;
            D2D1_COLOR_F nlColor;
            std::string name;
        };
        std::vector<ThemePreset> themePresets;
        int themeSelected{-1}; // -1 = 无选中

        // 内部状态
        bool hovered{false};
        bool pressed{false};
        bool editing{false};     // 文本输入焦点
        int  caretPos{0};        // 光标位置
        double caretBlinkTime{0};
        bool showCaret{true};
    };

    void BuildControls(const Config& cfg);
    void UpdateControlVisibility();  // 根据 displayMode 动态显示/隐藏控件
    void LayoutControls(int contentWidth);
    Control* HitTest(int x, int y);
    void DrawAll();

    // 绘制单个控件
    void DrawSectionHeader(ID2D1RenderTarget* rt, const Control& c);
    void DrawLabelRow(ID2D1RenderTarget* rt, const Control& c);
    void DrawToggleRow(ID2D1RenderTarget* rt, const Control& c);
    void DrawSliderRow(ID2D1RenderTarget* rt, const Control& c);
    void DrawColorRow(ID2D1RenderTarget* rt, const Control& c);
    void DrawDropdownRow(ID2D1RenderTarget* rt, const Control& c);
    void DrawButtonRow(ID2D1RenderTarget* rt, const Control& c);
    void DrawThemePresets(ID2D1RenderTarget* rt, const Control& c);
    void DrawHintText(ID2D1RenderTarget* rt, const Control& c);
    void DrawTitleBar(ID2D1RenderTarget* rt);  // 自绘标题栏（关闭/最小化按钮）

    // 鼠标/键盘事件处理
    void OnMouseDown(int x, int y);
    void OnMouseUp(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseWheel(int delta);
    void OnChar(wchar_t ch);
    void OnKeyDown(UINT key);
    void OnLoseFocus();

    // 从控件收集配置并回调
    void ApplyAndSave();
    // 取消（关闭窗口）
    void Cancel();

    // 颜色工具（已移至 color_utils.h，此处保留引用以兼容旧调用）

    HWND hwnd_{nullptr};
    HINSTANCE hInstance_{nullptr};
    HWND parentWnd_{nullptr};

    // D2D 资源
    Microsoft::WRL::ComPtr<ID2D1Factory>           d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget>   renderTarget_;
    Microsoft::WRL::ComPtr<IDWriteFactory>          dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       titleFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       labelFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       valueFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       sectionFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       hintFmt_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       btnFmt_;

    // 预设画刷（按需创建颜色画刷）
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surfaceBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textSecondaryBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accentBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accentHoverBrush_;

    Config currentConfig_;
    Config editedConfig_;  // 编辑中的配置副本
    ConfigChangedCallback onConfigChanged_;

    std::vector<Control> controls_;
    Control* hoverCtrl_{nullptr};
    Control* captureCtrl_{nullptr}; // 鼠标捕获的控件（拖动滑块等）

    // 标题栏
    RECT titleBarRect_{};      // 标题栏区域（用于拖动）
    RECT closeBtnRect_{};      // 关闭按钮区域
    RECT minBtnRect_{};        // 最小化按钮区域
    bool hoverClose_{false};   // 关闭按钮悬停
    bool hoverMin_{false};     // 最小化按钮悬停

    // 滚动
    int scrollOffset_{0};
    int totalContentHeight_{0};
    int contentWidth_{420};
    int rowHeight_{38};
    int sectionPadding_{10};
    int sliderWidth_{160};  // 滑块轨道宽度（像素）

    // 窗口尺寸
    static constexpr int kWinWidth  = 460;
    static constexpr int kWinHeight = 580;
    static constexpr int kTitleBarHeight = 36;  // 自绘标题栏高度

    // 颜色选择器弹窗（已拆分至 color_picker.h/.cpp）
    ColorPickerPopup colorPicker_;
    Control* activeColorCtrl_{nullptr};  // 当前正在编辑颜色的控件

    // 暗色模式检测
    bool isDarkMode_{false};

    // 颜色主题（根据暗/亮模式切换）
    ThemeColors theme_;

    void DetectDarkMode();
    void UpdateThemeColors();

    static constexpr const wchar_t* kWindowClass = L"MoeKoeTaskbarLyricsD2DSettingsClass";
    static bool classRegistered_;

    // 延迟关闭：避免在消息处理中 delete this 导致崩溃
    static constexpr UINT kMsgApplySave  = WM_APP + 2;
    static constexpr UINT kMsgCancel     = WM_APP + 3;
};

} // namespace moekoe
