// SPDX-License-Identifier: GPL-3.0
// d2d_settings_window.h - Direct2D 原生自绘设置界面（Settings 2.0）
//
// 职责:
//   - 使用 Direct2D + DirectWrite 绘制现代化设置界面（左导航 + 右内容）
//   - 设置变更实时应用（无需手动保存）
//   - 颜色选择器弹窗已拆分至 color_picker.h/.cpp
//
#pragma once

#include "ui/color_picker.h"
#include "ui/nav_view.h"
#include "ui/settings_page.h"
#include "config/config.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace moekoe {

// ThemeColors 别名：与 color_picker.h 中的 D2DThemeColors 类型保持一致
using ThemeColors = D2DThemeColors;

class D2DSettingsWindow {
public:
    using ConfigChangedCallback = std::function<void(const Config&)>;
    using ExportActionCallback = std::function<void(const std::string& action)>;

    D2DSettingsWindow();
    ~D2DSettingsWindow();

    D2DSettingsWindow(const D2DSettingsWindow&) = delete;
    D2DSettingsWindow& operator=(const D2DSettingsWindow&) = delete;

    // 显示设置窗口（非模态）
    bool Show(HINSTANCE hInstance, HWND parent, const Config& currentConfig);

    // 注册回调
    void OnConfigChanged(ConfigChangedCallback cb) { onConfigChanged_ = std::move(cb); }
    void OnExportAction(ExportActionCallback cb) { onExportAction_ = std::move(cb); }

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
    // 标题栏绘制
    // ═══════════════════════════════
    void DrawTitleBar(ID2D1RenderTarget* rt);

    // 绘制 ComboBox 下拉列表（在裁剪区域外）
    void DrawComboBoxDropdown(ID2D1RenderTarget* rt, ui::ComboBox* combo);

    // ═══════════════════════════════
    // 鼠标滚轮
    // ═══════════════════════════════
    void OnMouseWheel(int delta);

    // 实时应用：从页面控件收集配置并回调通知主程序
    void ApplyChanges();

    // 从所有页面收集配置变更到指定 Config（不触发回调）
    void CollectAllChanges(Config& cfg);

    // 颜色选择器确认后，将颜色写回当前激活的 ColorRow 控件
    void UpdateActiveColorRow(const D2D1_COLOR_F& color, const std::string& hex);

    // 导出日志文件（IFileSaveDialog）
    void ExportLogFile();

    // 导出诊断信息（IFileSaveDialog）
    void ExportDiagnosticInfo();

    // ═══════════════════════════════
    // Settings 2.0 页面系统
    // ═══════════════════════════════

    void BuildPages(const Config& cfg);
    void ArrangeUI();
    void DrawV2();
    ui::UIElement* HitTestV2(int x, int y);
    void OnMouseDownV2(int x, int y);
    void OnMouseMoveV2(int x, int y);
    void OnMouseUpV2(int x, int y);

    // ═══════════════════════════════
    // 窗口状态
    // ═══════════════════════════════

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

    // 预设画刷
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surfaceBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textSecondaryBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accentBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accentHoverBrush_;

    Config currentConfig_;
    Config editedConfig_;
    ConfigChangedCallback onConfigChanged_;
    ExportActionCallback onExportAction_;

    // 标题栏按钮区域
    RECT titleBarRect_{};
    RECT closeBtnRect_{};
    RECT maxBtnRect_{};
    RECT minBtnRect_{};
    bool hoverClose_{false};
    bool hoverMax_{false};
    bool hoverMin_{false};
    bool isMaximized_{false};
    RECT restoreRect_{};

    // V2 滚动
    int v2ScrollOffset_{0};
    int v2ContentHeight_{0};

    // 窗口尺寸基准值
    static constexpr int kWinWidthBase  = 720;
    static constexpr int kWinHeightBase = 580;
    static constexpr int kTitleBarHeight = 36;

    // 颜色选择器弹窗
    ColorPickerPopup colorPicker_;

    // DPI 缩放
    UINT  dpi_{96};
    float dpiScale_{1.0f};

    // 暗色模式检测
    bool isDarkMode_{false};

    // 颜色主题
    ThemeColors theme_;

    void DetectDarkMode();
    void UpdateThemeColors();

    static constexpr const wchar_t* kWindowClass = L"MoeKoeTaskbarLyricsD2DSettingsClass";
    static bool classRegistered_;

    // 延迟关闭消息
    static constexpr UINT kMsgClose = WM_APP + 2;

    // V2 页面数据
    std::unique_ptr<ui::NavView> navView_;
    std::vector<std::unique_ptr<ui::SettingsPage>> pages_;
    int currentPage_{0};
    ui::UIElement* capturedElement_{nullptr};
    ui::UIElement* hoveredElement_{nullptr};
    ui::ColorRow* activeColorRow_{nullptr};  // 颜色选择器激活时的目标控件
};

} // namespace moekoe
