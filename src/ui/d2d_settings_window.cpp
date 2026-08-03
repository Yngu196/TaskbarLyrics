// SPDX-License-Identifier: GPL-3.0
// d2d_settings_window.cpp - Direct2D 原生自绘设置界面实现（Settings 2.0）
#include "ui/d2d_settings_window.h"
#include "ui/color_utils.h"
#include "ui/settings_draw_utils.h"
#include "util/logger.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <dwmapi.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

namespace moekoe {

using namespace Microsoft::WRL;

// Utf8ToWide 已移至 color_utils.h（inline moekoe::Utf8ToWide）
// 宽字符 → UTF-8（仅此文件使用）
static std::string WideToLocalUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// 简单输入对话框（模态，用于修改端口等数值）
// 使用静态 DLGPROC + thread_local 数据传递
namespace {
struct InputDialogData {
    const wchar_t* prompt;
    wchar_t* buf;
    int bufSize;
    bool confirmed;
};
thread_local InputDialogData* t_inputData = nullptr;
}

static INT_PTR CALLBACK InputDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        // 设置字体为系统默认
        HFONT hFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(hDlg, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

        SetWindowTextW(hDlg, L"输入");
        // 提示文本
        HWND hStatic = CreateWindowExW(0, L"STATIC", t_inputData->prompt,
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       10, 10, 280, 20, hDlg, nullptr, nullptr, nullptr);
        SendMessageW(hStatic, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        // 编辑框（仅数字）
        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", t_inputData->buf,
                        WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
                        10, 36, 280, 24, hDlg, reinterpret_cast<HMENU>(101), nullptr, nullptr);
        SendMessageW(hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        SendMessageW(hEdit, EM_SETLIMITTEXT, t_inputData->bufSize - 1, 0);
        // 确定按钮
        HWND hOk = CreateWindowExW(0, L"BUTTON", L"确定",
                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON | WS_TABSTOP,
                   120, 68, 80, 28, hDlg, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        SendMessageW(hOk, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        // 取消按钮
        HWND hCancel = CreateWindowExW(0, L"BUTTON", L"取消",
                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                       210, 68, 80, 28, hDlg, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        SendMessageW(hCancel, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        SetFocus(hEdit);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            HWND hEdit = GetDlgItem(hDlg, 101);
            GetWindowTextW(hEdit, t_inputData->buf, t_inputData->bufSize);
            t_inputData->confirmed = true;
            EndDialog(hDlg, IDOK);
        } else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
        }
        return TRUE;
    }
    return FALSE;
}

static bool SimpleInputDialog(HWND parent, const wchar_t* title,
                               const wchar_t* prompt, wchar_t* buf, int bufSize) {
    InputDialogData data{prompt, buf, bufSize, false};
    t_inputData = &data;

    // 对话框模板（必须 DWORD 对齐）
    alignas(DWORD) struct {
        DLGTEMPLATE dlg;
        WORD menu = 0;
        WORD cls = 0;
        WORD title = 0;
    } tmpl = {};

    tmpl.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
    tmpl.dlg.cx = 220;
    tmpl.dlg.cy = 80;
    tmpl.dlg.cdit = 0;  // 控件在 WM_INITDIALOG 中动态创建

    INT_PTR result = DialogBoxIndirectParamW(
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
        &tmpl.dlg, parent, InputDlgProc, 0);

    t_inputData = nullptr;
    return data.confirmed;
}

bool D2DSettingsWindow::classRegistered_ = false;

// ═══════════════════════════════
// 构造 / 析构
// ═══════════════════════════════

D2DSettingsWindow::D2DSettingsWindow() = default;
D2DSettingsWindow::~D2DSettingsWindow() { Close(); }

// 颜色工具已移至 color_utils.h（namespace moekoe 自由函数）
// 在 cpp 中直接调用 HexToColorF / ColorFToHex / Lerp / HSLToRGB / RGBToHSL 即可

// ═══════════════════════════════
// 暗色模式检测
// ═══════════════════════════════

void D2DSettingsWindow::DetectDarkMode() {
    // 检查 Windows 应用模式设置
    BOOL dark = FALSE;
    HRESULT hr = ::DwmGetWindowAttribute(
        ::GetDesktopWindow(), DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // Fluent Design 暗色主题：始终启用暗色模式
    isDarkMode_ = true;
}

void D2DSettingsWindow::UpdateThemeColors() {
    // 读取当前 settingsTheme（从 editedConfig_ 获取最新值）
    std::string themeName = editedConfig_.Appearance().settingsTheme;

    // 根据主题名决定暗色/亮色模式："蓝"和"深色"用暗色，"浅色"和"白色"用亮色
    isDarkMode_ = (themeName == "blue" || themeName == "dark");

    if (isDarkMode_) {
        // 每个主题定义完整配色：背景、表面、边框、文本、强调色
        if (themeName == "dark") {
            // Visual Studio Dark 风格
            theme_.bg            = HexToColorF("#1E1E1E");
            theme_.surface       = HexToColorF("#2D2D2D");
            theme_.border        = HexToColorF("#3E3E3E");
            theme_.text          = HexToColorF("#F0F0F0");
            theme_.textSecondary = HexToColorF("#A0A0A0");
            theme_.accent        = HexToColorF("#007ACC");
            theme_.accentHover   = HexToColorF("#1A8CDC");
        } else if (themeName == "light") {
            // Visual Studio Light 风格 (暗色模式下的浅灰变体)
            theme_.bg            = HexToColorF("#2D2D30");
            theme_.surface       = HexToColorF("#3E3E42");
            theme_.border        = HexToColorF("#4E4E54");
            theme_.text          = HexToColorF("#F5F5F5");
            theme_.textSecondary = HexToColorF("#B0B0B8");
            theme_.accent        = HexToColorF("#0099FF");
            theme_.accentHover   = HexToColorF("#33ADFF");
        } else if (themeName == "white") {
            // 纯白简洁风格
            theme_.bg            = HexToColorF("#2B2B2F");
            theme_.surface       = HexToColorF("#3C3C40");
            theme_.border        = HexToColorF("#4D4D54");
            theme_.text          = HexToColorF("#FFFFFF");
            theme_.textSecondary = HexToColorF("#C8C8D0");
            theme_.accent        = HexToColorF("#3399FF");
            theme_.accentHover   = HexToColorF("#5AB5FF");
        } else {
            // 默认: Visual Studio Blue 风格
            theme_.bg            = HexToColorF("#1E1E2E");
            theme_.surface       = HexToColorF("#252540");
            theme_.border        = HexToColorF("#3A3A5C");
            theme_.text          = HexToColorF("#F0F0F5");
            theme_.textSecondary = HexToColorF("#9B9BB8");
            theme_.accent        = HexToColorF("#0078D4");
            theme_.accentHover   = HexToColorF("#2390E5");
        }
    } else {
        if (themeName == "dark") {
            theme_.bg            = HexToColorF("#F0F0F0");
            theme_.surface       = HexToColorF("#FFFFFF");
            theme_.border        = HexToColorF("#D0D0D0");
            theme_.text          = HexToColorF("#1E1E1E");
            theme_.textSecondary = HexToColorF("#6B6B6B");
            theme_.accent        = HexToColorF("#007ACC");
            theme_.accentHover   = HexToColorF("#1A8CDC");
        } else if (themeName == "light") {
            theme_.bg            = HexToColorF("#FAFAFA");
            theme_.surface       = HexToColorF("#FFFFFF");
            theme_.border        = HexToColorF("#E0E0E0");
            theme_.text          = HexToColorF("#2D2D30");
            theme_.textSecondary = HexToColorF("#8A8A8A");
            theme_.accent        = HexToColorF("#0099FF");
            theme_.accentHover   = HexToColorF("#33ADFF");
        } else if (themeName == "white") {
            theme_.bg            = HexToColorF("#FFFFFF");
            theme_.surface       = HexToColorF("#F8F8F8");
            theme_.border        = HexToColorF("#E8E8E8");
            theme_.text          = HexToColorF("#1A1A1A");
            theme_.textSecondary = HexToColorF("#808080");
            theme_.accent        = HexToColorF("#0078D4");
            theme_.accentHover   = HexToColorF("#2390E5");
        } else {
            // 默认: Visual Studio Blue 风格 (亮色)
            theme_.bg            = HexToColorF("#F5F7FA");
            theme_.surface       = HexToColorF("#FFFFFF");
            theme_.border        = HexToColorF("#D8DEE4");
            theme_.text          = HexToColorF("#1E1E2E");
            theme_.textSecondary = HexToColorF("#6B7280");
            theme_.accent        = HexToColorF("#0078D4");
            theme_.accentHover   = HexToColorF("#2390E5");
        }
    }
    UpdateDrawContext();
}

void D2DSettingsWindow::UpdateDrawContext() {
    drawCtx_.isDarkMode = isDarkMode_;
    if (isDarkMode_) {
        drawCtx_.bg            = theme_.bg;
        drawCtx_.surface       = theme_.surface;
        // 悬停表面：surface 偏亮 20%
        drawCtx_.surfaceHover  = D2D1::ColorF(
            std::min(theme_.surface.r * 1.2f, 1.0f),
            std::min(theme_.surface.g * 1.2f, 1.0f),
            std::min(theme_.surface.b * 1.2f, 1.0f), 1.0f);
        drawCtx_.border        = theme_.border;
        drawCtx_.text          = theme_.text;
        drawCtx_.textSecondary = theme_.textSecondary;
        drawCtx_.accent        = theme_.accent;
        drawCtx_.accentHover   = theme_.accentHover;

        // accentLight 是 accent 的半透明变体
        drawCtx_.accentLight   = D2D1::ColorF(theme_.accent.r, theme_.accent.g, theme_.accent.b, 0.5f);
        drawCtx_.danger        = HexToColorF("#FF6B6B");
    } else {
        drawCtx_.lightBg            = theme_.bg;
        drawCtx_.lightSurface       = theme_.surface;
        drawCtx_.lightBorder        = theme_.border;
        drawCtx_.lightText          = theme_.text;
        drawCtx_.lightTextSecondary = theme_.textSecondary;

        // 亮色模式下也要填充通用字段（accent/danger 等），避免控件使用默认暗色值
        drawCtx_.accent        = theme_.accent;
        drawCtx_.accentHover   = theme_.accentHover;
        drawCtx_.accentLight   = D2D1::ColorF(theme_.accent.r, theme_.accent.g, theme_.accent.b, 0.5f);
        drawCtx_.danger        = HexToColorF("#FF6B6B");
    }
}

void D2DSettingsWindow::CreateBgGradientBrush() {
    if (!renderTarget_ || !d2dFactory_) return;

    RECT rc; GetClientRect(hwnd_, &rc);
    float H = static_cast<float>(rc.bottom) / dpiScale_;

    // 垂直渐变：基于当前主题的 bg 和 surface 颜色
    // 中间色 = bg 和 surface 的混合
    D2D1_COLOR_F topColor = theme_.bg;
    D2D1_COLOR_F midColor = D2D1::ColorF(
        (theme_.bg.r + theme_.surface.r) / 2,
        (theme_.bg.g + theme_.surface.g) / 2,
        (theme_.bg.b + theme_.surface.b) / 2, 1.0f);
    D2D1_COLOR_F bottomColor = theme_.surface;

    D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, topColor },
        { 0.5f, midColor },
        { 1.0f, bottomColor },
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    HRESULT hr = renderTarget_->CreateGradientStopCollection(
        stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection);
    if (FAILED(hr)) return;

    renderTarget_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0, 0), D2D1::Point2F(0, H)),
        collection.Get(), &bgGradientBrush_);
}

void D2DSettingsWindow::StartAnimationTimer() {
    if (hwnd_) {
        // 16ms ≈ 60fps，用于驱动控件动画
        ::SetTimer(hwnd_, kAnimTimerId, 16, nullptr);
    }
}

void D2DSettingsWindow::StopAnimationTimer() {
    if (hwnd_) {
        ::KillTimer(hwnd_, kAnimTimerId);
    }
}

// ═══════════════════════════════
// 显示窗口
// ═══════════════════════════════

bool D2DSettingsWindow::Show(HINSTANCE hInstance, HWND parent, const Config& currentConfig) {
    hInstance_   = hInstance;
    parentWnd_   = parent;
    currentConfig_ = currentConfig;
    editedConfig_ = currentConfig;

    // 单实例检测：若已有设置窗口存在则激活并前置
    {
        HWND existingWnd = FindWindowW(kWindowClass, L"任务栏歌词 - 设置");
        if (existingWnd) {
            if (IsIconic(existingWnd)) {
                ShowWindow(existingWnd, SW_RESTORE);
            }
            SetForegroundWindow(existingWnd);
            return true;
        }
    }

    DetectDarkMode();
    UpdateThemeColors();

    // 注册窗口类（仅一次）
    if (!classRegistered_) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;  // 支持双击标题栏最大化/恢复
        wc.lpfnWndProc   = &WndProc;
        wc.hInstance     = hInstance;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        // 背景画刷：空（自绘）
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); // 兜底背景色（D2D 会覆盖）
        classRegistered_ = (::RegisterClassExW(&wc) != 0);
    }

    // DPI 缩放：确保高 DPI 下窗口足够大
    dpi_ = ::GetDpiForWindow(parent ? parent : ::GetDesktopWindow());
    dpiScale_ = dpi_ / 96.0f;
    const int winW = ::MulDiv(kWinWidthBase, dpi_, 96);
    const int winH = ::MulDiv(kWinHeightBase, dpi_, 96);

    // 创建窗口。WS_OVERLAPPEDWINDOW 含 WS_CAPTION/WS_SYSMENU/WS_MINIMIZEBOX/WS_MAXIMIZEBOX/WS_THICKFRAME，
    // 提供完整任务栏交互（点击图标最小化/恢复）；系统标题栏通过 WM_NCCALCSIZE+WM_NCPAINT 消除。
    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,  // 显示在任务栏
        kWindowClass, L"任务栏歌词 - 设置",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, winW, winH,  // 先创建在 (0,0)，下面再定位
        parent, nullptr, hInstance, this);

    if (!hwnd_) return false;

    // 居中显示到主显示器（或父窗口所在显示器）
    HMONITOR hMon = MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - winW) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - winH) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_FRAMECHANGED);

    // 阻止 DWM 在窗口激活时绘制经典边框：扩展 DWM 帧到客户区底部 1px，
    // 配合 WM_NCACTIVATE / WM_ACTIVATE / WM_DWMCOMPOSITIONCHANGED 中的 DwmExtendFrameIntoClientArea 调用。
    {
        MARGINS dwmMargins = {0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(hwnd_, &dwmMargins);
    }

    // DWM 圆角
    DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // DWM 毛玻璃背景（Mica 效果，Windows 11 22H2+）
    // DWMWA_SYSTEMBACKDROP_TYPE = 38（Win11 22H2+），失败则静默忽略
    {
        INT backdropType = 2;  // 2 = Mica
        DwmSetWindowAttribute(hwnd_, 38, &backdropType, sizeof(backdropType));
        // 设置标题栏颜色跟随当前主题（深色→暗色标题栏，浅色→亮色标题栏）
        BOOL darkMode = isDarkMode_ ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    }

    // 初始化 D2D
    if (!InitD2D()) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // 创建渐变背景画刷
    CreateBgGradientBrush();

    // 构建页面和导航（布局使用 DIP 坐标，渲染目标 DPI 统一映射）
    BuildPages(currentConfig);
    ArrangeUI();

    ShowWindow(hwnd_, SW_SHOW);
    SetFocus(hwnd_);

    // 启动动画帧定时器
    StartAnimationTimer();

    return true;
}

bool D2DSettingsWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void D2DSettingsWindow::Close() {
    StopAnimationTimer();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    ShutdownD2D();
}

// ═══════════════════════════════
// D2D 初始化
// ═══════════════════════════════

bool D2DSettingsWindow::InitD2D() {
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        d2dFactory_.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
    if (FAILED(hr)) return false;

    // 创建文本格式
    dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"zh-Hans", &titleFmt_);

    dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-Hans", &labelFmt_);

    dwriteFactory_->CreateTextFormat(
        L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &valueFmt_);

    dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-Hans", &sectionFmt_);
    sectionFmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); // 不换行

    dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-Hans", &hintFmt_);

    dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-Hans", &btnFmt_);

    // 创建渲染目标（使用实际窗口尺寸，确保非零）
    RECT clientRc;
    ::GetClientRect(hwnd_, &clientRc);
    hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_,
            D2D1_SIZE_U{static_cast<UINT>(clientRc.right), static_cast<UINT>(clientRc.bottom)}),
        &renderTarget_);
    if (FAILED(hr)) return false;

    // 设置渲染目标 DPI 为系统 DPI，使内部坐标统一为 DIP（逻辑像素）。
    // D2D 自动将 DIP 坐标映射到物理像素，所有布局/绘制/交互使用同一套逻辑坐标。
    renderTarget_->SetDpi(static_cast<FLOAT>(dpi_), static_cast<FLOAT>(dpi_));

    // 创建预设画刷
    renderTarget_->CreateSolidColorBrush(theme_.bg, &bgBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.surface, &surfaceBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.border, &borderBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.text, &textBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.textSecondary, &textSecondaryBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.accent, &accentBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.accentHover, &accentHoverBrush_);

    return true;
}

void D2DSettingsWindow::ShutdownD2D() {
    bgGradientBrush_.Reset();
    bgBrush_.Reset();
    surfaceBrush_.Reset();
    borderBrush_.Reset();
    textBrush_.Reset();
    textSecondaryBrush_.Reset();
    accentBrush_.Reset();
    accentHoverBrush_.Reset();

    titleFmt_.Reset();
    labelFmt_.Reset();
    valueFmt_.Reset();
    sectionFmt_.Reset();
    hintFmt_.Reset();
    btnFmt_.Reset();

    renderTarget_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
}

// ═══════════════════════════════
// 标题栏绘制
// ═══════════════════════════════

void D2DSettingsWindow::DrawTitleBar(ID2D1RenderTarget* rt) {
    RECT rc; GetClientRect(hwnd_, &rc);
    // 使用 DIP 坐标（渲染目标 DPI 已设为系统 DPI）
    float W = static_cast<float>(rc.right) / dpiScale_;
    float H = static_cast<float>(kTitleBarHeight);

    // 标题栏背景（深蓝紫半透明）
    ComPtr<ID2D1SolidColorBrush> titleBg;
    D2D1_COLOR_F bgColor = theme_.bg;
    bgColor.a = isDarkMode_ ? 0.97f : 0.85f;
    rt->CreateSolidColorBrush(bgColor, &titleBg);
    rt->FillRectangle(D2D1::RectF(0, 0, W, H), titleBg.Get());

    // 底部分隔线（紫调半透明）
    ComPtr<ID2D1SolidColorBrush> lineBr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.06f), &lineBr);
    rt->DrawLine(D2D1::Point2F(0, H - 0.5f), D2D1::Point2F(W, H - 0.5f), lineBr.Get());

    // 标题文字（暗色：#B0B0B0）
    std::wstring wtitle = Utf8ToWide("任务栏歌词 - 设置");
    DrawTextLine(rt, labelFmt_.Get(), textSecondaryBrush_.Get(),
                 wtitle.c_str(), 14.f, (H - 16.f) / 2.f, W - 100.f);

    // 关闭按钮 × （右侧）
    float btnSize = 28.f;
    float btnY = (H - btnSize) / 2.f;
    float closeX = W - btnSize - 4.f;
    closeBtnRect_ = {static_cast<int>(closeX), static_cast<int>(btnY),
                     static_cast<int>(closeX + btnSize), static_cast<int>(btnY + btnSize)};

    // 最大化/恢复按钮 □/⧉ （关闭按钮左侧）
    float maxX = closeX - btnSize - 2.f;
    maxBtnRect_ = {static_cast<int>(maxX), static_cast<int>(btnY),
                   static_cast<int>(maxX + btnSize), static_cast<int>(btnY + btnSize)};

    // 最小化按钮 — （最大化按钮左侧）
    float minX = maxX - btnSize - 2.f;
    minBtnRect_ = {static_cast<int>(minX), static_cast<int>(btnY),
                   static_cast<int>(minX + btnSize), static_cast<int>(btnY + btnSize)};

    // 绘制最小化按钮
    {
        ComPtr<ID2D1SolidColorBrush> minBg;
        D2D1_COLOR_F c = hoverMin_ ? theme_.surface : bgColor;
        c.a = hoverMin_ ? 0.8f : 0.f;
        rt->CreateSolidColorBrush(c, &minBg);
        if (hoverMin_) {
            FillRoundedRect(rt, minBg.Get(), minX, btnY, btnSize, btnSize, 4.f);
        }
        // 横线 ─
        ComPtr<ID2D1SolidColorBrush> minIcon;
        rt->CreateSolidColorBrush(hoverMin_ ? theme_.text : theme_.textSecondary, &minIcon);
        float ly = btnY + btnSize / 2.f;
        rt->DrawLine(D2D1::Point2F(minX + 7.f, ly), D2D1::Point2F(minX + btnSize - 7.f, ly),
                     minIcon.Get(), 1.5f);
    }

    // 绘制最大化/恢复按钮
    {
        ComPtr<ID2D1SolidColorBrush> maxBg;
        D2D1_COLOR_F c = hoverMax_ ? theme_.surface : bgColor;
        c.a = hoverMax_ ? 0.8f : 0.f;
        rt->CreateSolidColorBrush(c, &maxBg);
        if (hoverMax_) {
            FillRoundedRect(rt, maxBg.Get(), maxX, btnY, btnSize, btnSize, 4.f);
        }
        ComPtr<ID2D1SolidColorBrush> maxIcon;
        rt->CreateSolidColorBrush(hoverMax_ ? theme_.text : theme_.textSecondary, &maxIcon);

        if (isMaximized_) {
            // 恢复图标：重叠的两个矩形 ⧉
            float cx = maxX + btnSize / 2.f;
            float cy = btnY + btnSize / 2.f;
            float s = 5.f;  // 半尺寸
            float off = 3.f; // 偏移
            // 后方矩形（左上）
            rt->DrawRectangle(D2D1::RectF(cx - s - off, cy - s - off, cx + s - off, cy + s - off),
                              maxIcon.Get(), 1.4f);
            // 前方矩形（右下）
            rt->DrawRectangle(D2D1::RectF(cx - s + off, cy - s + off, cx + s + off, cy + s + off),
                              maxIcon.Get(), 1.4f);
        } else {
            // 最大化图标：单个矩形 □
            float cx = maxX + btnSize / 2.f;
            float cy = btnY + btnSize / 2.f;
            float s = 5.5f;
            rt->DrawRectangle(D2D1::RectF(cx - s, cy - s, cx + s, cy + s),
                              maxIcon.Get(), 1.4f);
            // 顶部加粗线（模拟标题栏）
            rt->DrawLine(D2D1::Point2F(cx - s, cy - s), D2D1::Point2F(cx + s, cy - s),
                         maxIcon.Get(), 2.0f);
        }
    }

    // 绘制关闭按钮
    {
        ComPtr<ID2D1SolidColorBrush> closeBg;
        D2D1_COLOR_F c = hoverClose_
            ? (isDarkMode_ ? D2D1::ColorF(0.8f, 0.2f, 0.2f, 0.9f) : D2D1::ColorF(1.f, 0.3f, 0.3f, 0.9f))
            : D2D1::ColorF(0, 0, 0, 0);
        rt->CreateSolidColorBrush(c, &closeBg);
        if (hoverClose_) {
            FillRoundedRect(rt, closeBg.Get(), closeX, btnY, btnSize, btnSize, 4.f);
        }
        // × 符号
        ComPtr<ID2D1SolidColorBrush> xIcon;
        rt->CreateSolidColorBrush(
            hoverClose_ ? D2D1::ColorF(1, 1, 1, 1) : theme_.textSecondary, &xIcon);
        float cx = closeX + btnSize / 2.f;
        float cy = btnY + btnSize / 2.f;
        float d = 5.f;
        rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), xIcon.Get(), 1.6f);
        rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), xIcon.Get(), 1.6f);
    }

    // 更新标题栏区域（用于拖动判定，DIP 坐标）
    titleBarRect_ = {0, 0, static_cast<int>(W), kTitleBarHeight};
}

// ═══════════════════════════════
// 鼠标滚轮
// ═══════════════════════════════
// ComboBox 下拉列表绘制（裁剪区域外，覆盖下方内容）
// ═══════════════════════════════

void D2DSettingsWindow::DrawComboBoxDropdown(ID2D1RenderTarget* rt, ui::ComboBox* combo) {
    if (!combo || combo->items.empty()) return;

    const float boxX = combo->X() + combo->Width() / 2;
    const float boxW = combo->Width() / 2;
    const float boxY = combo->Y() + 5 + 30;  // 与 ComboBox::Draw 中的 boxY+boxH 匹配
    const float itemH = 32;
    const float dropH = static_cast<float>(combo->items.size()) * itemH;

    // 下拉阴影（更明显的投影）
    ComPtr<ID2D1SolidColorBrush> shadowBrush1;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.4f), &shadowBrush1);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(boxX + 3, boxY + 3, boxX + boxW + 3, boxY + dropH + 3), 8, 8),
        shadowBrush1.Get());
    ComPtr<ID2D1SolidColorBrush> shadowBrush2;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.2f), &shadowBrush2);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(boxX + 1, boxY + 1, boxX + boxW + 1, boxY + dropH + 1), 8, 8),
        shadowBrush2.Get());

    // 下拉背景（基于主题 bg 色，亮暗模式自适应）
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dropBg, dropBorder;
    rt->CreateSolidColorBrush(
        D2D1::ColorF(theme_.bg.r * 1.2f, theme_.bg.g * 1.2f, theme_.bg.b * 1.2f, 1.0f), &dropBg);
    rt->CreateSolidColorBrush(drawCtx_.Border(), &dropBorder);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(boxX, boxY, boxX + boxW, boxY + dropH), 8, 8),
        dropBg.Get());
    rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(boxX, boxY, boxX + boxW, boxY + dropH), 8, 8),
        dropBorder.Get(), 1);

    // 绘制每个选项
    static Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
    if (!dwFactory) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> itemFmt;
    dwFactory->CreateTextFormat(L"Segoe UI Variable", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13, L"zh-Hans", itemFmt.GetAddressOf());

    if (itemFmt) {
        itemFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        for (int i = 0; i < static_cast<int>(combo->items.size()); ++i) {
            float iy = boxY + i * itemH;

            // 悬停项背景（紫色半透明）
            if (i == combo->hoveredDropIndex_ && i != combo->selectedIndex) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBg;
                rt->CreateSolidColorBrush(
                    D2D1::ColorF(drawCtx_.accent.r, drawCtx_.accent.g, drawCtx_.accent.b, 0.15f), &hoverBg);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(boxX + 3, iy + 2, boxX + boxW - 3, iy + itemH - 2), 4, 4),
                    hoverBg.Get());
            }

            // 选中项背景（紫色强调色）
            if (i == combo->selectedIndex) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selBg;
                rt->CreateSolidColorBrush(drawCtx_.accent, &selBg);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(boxX + 3, iy + 2, boxX + boxW - 3, iy + itemH - 2), 4, 4),
                    selBg.Get());
            }

            D2D1_COLOR_F textColor = (i == combo->selectedIndex)
                ? D2D1::ColorF(1, 1, 1, 1)
                : (i == combo->hoveredDropIndex_
                    ? drawCtx_.Text()  // 悬停项文本更亮
                    : drawCtx_.TextSecondary());
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> txtBrush;
            rt->CreateSolidColorBrush(textColor, txtBrush.GetAddressOf());

            std::wstring witem = Utf8ToWide(combo->items[i]);
            rt->DrawTextW(witem.c_str(), static_cast<UINT32>(witem.size()),
                          itemFmt.Get(), D2D1::RectF(boxX + 12, iy, boxX + boxW - 12, iy + itemH),
                          txtBrush.Get());
        }
    }
}

// ═══════════════════════════════
// 鼠标滚轮
// ═══════════════════════════════

void D2DSettingsWindow::OnMouseWheel(int delta) {
    // 内容区可见高度（DIP 坐标，扣除标题栏和导航栏）
    const int clientH = [this]() {
        RECT rc; GetClientRect(hwnd_, &rc);
        return static_cast<int>(rc.bottom / dpiScale_);  // DIP 坐标
    }();
    const int visibleH = clientH - kTitleBarHeight;
    int maxScroll = std::max(0, v2ContentHeight_ - visibleH);
    if (maxScroll <= 0) return;  // 内容未溢出，无需滚动

    int prevOffset = v2ScrollOffset_;
    v2ScrollOffset_ -= delta / WHEEL_DELTA * 48;
    v2ScrollOffset_ = std::clamp(v2ScrollOffset_, 0, maxScroll);
    if (v2ScrollOffset_ != prevOffset) {
        ArrangeUI();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ═══════════════════════════════
// 实时应用配置
// ═══════════════════════════════

void D2DSettingsWindow::ApplyChanges() {
    // 从页面控件收集值到 editedConfig_
    for (auto& page : pages_) {
        page->CollectChanges(editedConfig_);
    }
    // 实时回调通知主程序
    if (onConfigChanged_) onConfigChanged_(editedConfig_);
}

void D2DSettingsWindow::CollectAllChanges(Config& cfg) {
    for (auto& page : pages_) {
        page->CollectChanges(cfg);
    }
}

void D2DSettingsWindow::UpdateActiveColorRow(const D2D1_COLOR_F& color, const std::string& hex) {
    if (activeColorRow_) {
        activeColorRow_->colorValue = color;
        activeColorRow_->textValue = hex;
        activeColorRow_ = nullptr;
    }
}

// ═══════════════════════════════
// 导出功能
// ═══════════════════════════════

void D2DSettingsWindow::ExportLogFile() {
    if (onExportAction_) onExportAction_("exportLog");
}

void D2DSettingsWindow::ExportDiagnosticInfo() {
    if (onExportAction_) onExportAction_("exportDiagnostic");
}

// ═══════════════════════════════
// WndProc
// ═══════════════════════════════

LRESULT CALLBACK D2DSettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    D2DSettingsWindow* self = reinterpret_cast<D2DSettingsWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 1;
    }

    case WM_CREATE:
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            // 窗口重获焦点时重新扩展 DWM 帧到客户区，防止 DWM 重绘经典样式边框
            MARGINS dwmMargins = {0, 0, 0, 1};
            DwmExtendFrameIntoClientArea(hwnd, &dwmMargins);
        }
        break;

    case WM_DWMCOMPOSITIONCHANGED:
        // DWM 合成状态变化时（远程桌面等场景）重新扩展帧
        {
            MARGINS dwmMargins = {0, 0, 0, 1};
            DwmExtendFrameIntoClientArea(hwnd, &dwmMargins);
        }
        break;

    case WM_NCCALCSIZE:
        // 消除非客户区（系统标题栏），使客户区覆盖整个窗口。
        // D2D 自绘按钮落在窗口第一行，同时 WS_OVERLAPPEDWINDOW 的任务栏交互不受影响。
        if (wParam == TRUE) return 0;
        break;

    case WM_NCPAINT:
        // 阻止系统绘制标题栏内容（按钮、标题文字等），与 WM_NCCALCSIZE 配合杜绝残留渲染。
        return 0;

    case WM_NCACTIVATE:
        // 返回 TRUE 阻止 DWM 在窗口激活/失活时重绘非客户区经典边框，
        // 与 DwmExtendFrameIntoClientArea 配合使用。
        return TRUE;

    case WM_PAINT:
        if (self) self->DrawV2();
        // D2D 自绘窗口：ValidateRect 直接标记已绘制，避免 BeginPaint 的 GDI 背景画刷覆盖 D2D 内容
        ValidateRect(hwnd, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1; // 防止闪烁

    case WM_LBUTTONDOWN: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            // DPI 转换：鼠标消息坐标为物理像素，控件布局使用逻辑像素（DIP）
            int lx = static_cast<int>(x / self->dpiScale_);
            int ly = static_cast<int>(y / self->dpiScale_);

            // 颜色选择器优先处理（弹窗在最上层）
            if (self->colorPicker_.IsActive()) {
                D2D1_COLOR_F newColor; std::string newHex;
                auto result = self->colorPicker_.HandleMouseDown(lx, ly, &newColor, &newHex);
                if (result == ColorPickerPopup::ActionResult::Confirmed) {
                    // 将选中颜色写回当前 ColorRow 控件
                    self->UpdateActiveColorRow(newColor, newHex);
                    self->colorPicker_.Deactivate(hwnd);
                    self->ApplyChanges();
                } else if (result == ColorPickerPopup::ActionResult::Cancelled) {
                    self->colorPicker_.Deactivate(hwnd);
                }
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            // 标题栏关闭按钮（使用逻辑坐标判定）
            if (PtInRect(&self->closeBtnRect_, {lx, ly})) {
                ::PostMessageW(hwnd, D2DSettingsWindow::kMsgClose, 0, 0);
                return 0;
            }
            // 标题栏最大化/恢复按钮
            if (PtInRect(&self->maxBtnRect_, {lx, ly})) {
                if (self->isMaximized_) {
                    ShowWindow(hwnd, SW_RESTORE);
                    self->isMaximized_ = false;
                } else {
                    ::GetWindowRect(hwnd, &self->restoreRect_);
                    ShowWindow(hwnd, SW_MAXIMIZE);
                    self->isMaximized_ = true;
                }
                return 0;
            }
            // 标题栏最小化按钮（使用逻辑坐标判定）
            if (PtInRect(&self->minBtnRect_, {lx, ly})) {
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            }

            self->OnMouseDownV2(lx, ly);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            int lx = static_cast<int>(x / self->dpiScale_);
            int ly = static_cast<int>(y / self->dpiScale_);
            self->OnMouseUpV2(lx, ly);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            int lx = static_cast<int>(x / self->dpiScale_);
            int ly = static_cast<int>(y / self->dpiScale_);

            // 颜色选择器拖动
            if (self->colorPicker_.IsActive()) {
                bool lb = (wParam & MK_LBUTTON) != 0;
                self->colorPicker_.HandleMouseMove(lx, ly, lb);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                // 不继续分发到页面控件
            } else {
                self->OnMouseMoveV2(lx, ly);
            }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (self) self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }

    case WM_TIMER: {
        if (self && wParam == kAnimTimerId) {
            // 推进所有控件动画
            bool anyActive = false;
            const float deltaMs = 16.0f;  // ≈60fps
            if (self->navView_) {
                anyActive = self->navView_->TickAnimation(deltaMs) || anyActive;
            }
            if (self->currentPage_ >= 0 &&
                self->currentPage_ < static_cast<int>(self->pages_.size())) {
                anyActive = self->pages_[self->currentPage_]->TickAnimation(deltaMs) || anyActive;
            }
            // 有动画在播放时重绘
            if (anyActive) {
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_NCHITTEST: {
        // 无边框窗口的边缘拖拽调整大小
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        RECT rc; GetClientRect(hwnd, &rc);
        const int border = 6;  // 边缘检测宽度（物理像素）

        // 转换为 DPI 缩放后的逻辑坐标
        if (self) {
            int lx = static_cast<int>(pt.x / self->dpiScale_);
            int ly = static_cast<int>(pt.y / self->dpiScale_);

            // 标题栏区域 → 可拖动
            if (ly < kTitleBarHeight) {
                // 排除标题栏按钮区域
                RECT titleBar = self->titleBarRect_;
                if (PtInRect(&self->closeBtnRect_, {lx, ly}) ||
                    PtInRect(&self->maxBtnRect_, {lx, ly}) ||
                    PtInRect(&self->minBtnRect_, {lx, ly})) {
                    return HTCLIENT;  // 按钮区域由 WM_LBUTTONDOWN 处理
                }
                return HTCAPTION;
            }
        }

        // 边缘检测
        bool left   = pt.x < border;
        bool right  = pt.x > rc.right - border;
        bool top    = pt.y < border;
        bool bottom = pt.y > rc.bottom - border;

        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;
        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;

        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        // 设置窗口最小尺寸
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (self) {
            mmi->ptMinTrackSize.x = ::MulDiv(480, self->dpi_, 96);
            mmi->ptMinTrackSize.y = ::MulDiv(360, self->dpi_, 96);
        }
        return 0;
    }

    case WM_SIZE: {
        if (self && self->renderTarget_) {
            // 更新渲染目标大小
            RECT rc; GetClientRect(hwnd, &rc);
            self->renderTarget_->Resize(
                D2D1_SIZE_U{static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom)});
            // 窗口大小变化后重建渐变画刷
            self->CreateBgGradientBrush();
            // 重新布局
            self->ArrangeUI();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DPICHANGED: {
        // 多显示器 DPI 变化时更新缩放因子、窗口大小和布局
        if (self) {
            self->dpi_ = HIWORD(wParam);
            self->dpiScale_ = self->dpi_ / 96.0f;

            // 使用系统建议的窗口矩形
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);

            // 更新渲染目标大小和 DPI（无需完全重建，保留控件状态）
            RECT rc; ::GetClientRect(hwnd, &rc);
            if (self->renderTarget_) {
                self->renderTarget_->Resize(
                    D2D1_SIZE_U{static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom)});
                self->renderTarget_->SetDpi(
                    static_cast<FLOAT>(self->dpi_), static_cast<FLOAT>(self->dpi_));
            }
            // DPI 变化后重建渐变画刷
            self->CreateBgGradientBrush();

            // 重新布局控件（DIP 坐标，渲染目标 DPI 已同步更新）
            self->ArrangeUI();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case D2DSettingsWindow::kMsgClose:
        if (self) self->Close();
        return 0;

    case WM_DESTROY:
        // 设置对话框关闭，不退出主程序（不能调用 PostQuitMessage）
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ═══════════════════════════════════════
// Settings 2.0 页面系统
// ═══════════════════════════════════════

void D2DSettingsWindow::BuildPages(const Config& cfg) {
    // 创建导航
    navView_ = std::make_unique<ui::NavView>();
    navView_->id = "navView";
    navView_->BuildItems({"歌词", "外观", "窗口", "行为", "高级", "关于"});
    navView_->SetOnPageChange([this](int idx) {
        currentPage_ = idx;
        v2ScrollOffset_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
    });

    // 创建页面
    pages_.clear();
    auto lyricsPage = std::make_unique<ui::LyricsPage>();       lyricsPage->BuildContent(cfg);
    auto appearPage = std::make_unique<ui::AppearancePage>();   appearPage->BuildContent(cfg);
    auto windowPage = std::make_unique<ui::WindowPage>();       windowPage->BuildContent(cfg);
    auto behavPage  = std::make_unique<ui::BehaviorPage>();     behavPage->BuildContent(cfg);
    auto advPage    = std::make_unique<ui::AdvancedPage>();     advPage->BuildContent(cfg);
    auto aboutPage  = std::make_unique<ui::AboutPage>();        aboutPage->BuildContent(cfg);

    pages_.push_back(std::move(lyricsPage));
    pages_.push_back(std::move(appearPage));
    pages_.push_back(std::move(windowPage));
    pages_.push_back(std::move(behavPage));
    pages_.push_back(std::move(advPage));
    pages_.push_back(std::move(aboutPage));
}

void D2DSettingsWindow::ArrangeUI() {
    if (!navView_ || pages_.empty()) return;

    RECT rc; GetClientRect(hwnd_, &rc);
    int clientW = static_cast<int>(rc.right / dpiScale_);
    int clientH = static_cast<int>(rc.bottom / dpiScale_);

    const int navW = moekoe::constants::SETTINGS_NAV_WIDTH_BASE_DP;
    int contentW = clientW - navW;

    // 限制内容区最大宽度，最大化时避免卡片过度拉伸
    const int kMaxContentWidth = 700;
    if (contentW > kMaxContentWidth) {
        contentW = kMaxContentWidth;
    }

    // 最大化时整体居中：导航栏+内容区作为整体居中显示，左右对称留白
    const int totalWidth = navW + contentW;
    panelOffsetX_ = (clientW - totalWidth) / 2;
    if (panelOffsetX_ < 0) panelOffsetX_ = 0;

    // 导航区域
    navView_->Arrange(panelOffsetX_, kTitleBarHeight, navW, clientH - kTitleBarHeight);

    // 当前页面
    if (currentPage_ >= 0 && currentPage_ < static_cast<int>(pages_.size())) {
        auto& page = pages_[currentPage_];
        float pageH = page->MeasureHeight(contentW);
        page->Arrange(panelOffsetX_ + navW, kTitleBarHeight - v2ScrollOffset_, contentW, pageH);
        v2ContentHeight_ = static_cast<int>(pageH);
    }
}

void D2DSettingsWindow::DrawV2() {
    if (!renderTarget_) return;

    renderTarget_->BeginDraw();

    // 渐变背景：#202020→#2A2A2A（若画刷无效则回退纯色）
    if (bgGradientBrush_) {
        RECT rc; GetClientRect(hwnd_, &rc);
        float W = static_cast<float>(rc.right) / dpiScale_;
        float H = static_cast<float>(rc.bottom) / dpiScale_;
        renderTarget_->FillRectangle(D2D1::RectF(0, 0, W, H), bgGradientBrush_.Get());
    } else {
        renderTarget_->Clear(theme_.bg);
    }

    // 更新画刷颜色
    bgBrush_->SetColor(theme_.bg);
    surfaceBrush_->SetColor(theme_.surface);
    borderBrush_->SetColor(theme_.border);
    textBrush_->SetColor(theme_.text);
    textSecondaryBrush_->SetColor(theme_.textSecondary);
    accentBrush_->SetColor(theme_.accent);
    accentHoverBrush_->SetColor(theme_.accentHover);

    RECT rc; GetClientRect(hwnd_, &rc);
    const int clientW = static_cast<int>(rc.right / dpiScale_);
    const int clientH = static_cast<int>(rc.bottom / dpiScale_);

    // 绘制标题栏
    DrawTitleBar(renderTarget_.Get());

    // 绘制导航（传递 DrawContext）
    if (navView_) navView_->Draw(renderTarget_.Get(), drawCtx_);

    // 绘制当前页面（裁剪内容区）
    if (currentPage_ >= 0 && currentPage_ < static_cast<int>(pages_.size())) {
        const float contentClipLeft = static_cast<float>(panelOffsetX_ + moekoe::constants::SETTINGS_NAV_WIDTH_BASE_DP);
        renderTarget_->PushAxisAlignedClip(
            D2D1::RectF(contentClipLeft,
                        static_cast<float>(kTitleBarHeight),
                        static_cast<float>(clientW),
                        static_cast<float>(clientH)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        // 先绘制页面（不含 ComboBox 下拉），传递 DrawContext
        pages_[currentPage_]->Draw(renderTarget_.Get(), drawCtx_);
        renderTarget_->PopAxisAlignedClip();

        // ComboBox 下拉列表绘制在裁剪区域外（覆盖下方 Card）
        for (auto& card : pages_[currentPage_]->Children()) {
            for (auto& child : card->Children()) {
                auto* combo = dynamic_cast<ui::ComboBox*>(child.get());
                if (combo && combo->dropped) {
                    DrawComboBoxDropdown(renderTarget_.Get(), combo);
                }
            }
        }
    }

    // 绘制颜色选择器弹窗（在最上层，不受裁剪限制）
    if (colorPicker_.IsActive()) {
        colorPicker_.Draw(renderTarget_.Get(), isDarkMode_, theme_,
                          valueFmt_.Get(), hintFmt_.Get(), textSecondaryBrush_.Get(),
                          v2ScrollOffset_);
    }

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ShutdownD2D();
        InitD2D();
        BuildPages(currentConfig_);
        ArrangeUI();
    }
}

ui::UIElement* D2DSettingsWindow::HitTestV2(int x, int y) {
    // 先检测导航区
    if (navView_) {
        ui::UIElement* hit = navView_->HitTest(static_cast<float>(x), static_cast<float>(y));
        if (hit) return hit;
    }
    // 再检测当前页面
    if (currentPage_ >= 0 && currentPage_ < static_cast<int>(pages_.size())) {
        ui::UIElement* hit = pages_[currentPage_]->HitTest(static_cast<float>(x), static_cast<float>(y));
        if (hit) return hit;
    }
    return nullptr;
}

void D2DSettingsWindow::OnMouseDownV2(int x, int y) {
    // ── 优先处理已展开的 ComboBox（下拉弹窗在最上层）──
    if (currentPage_ >= 0 && currentPage_ < static_cast<int>(pages_.size())) {
        auto& page = pages_[currentPage_];
        for (auto& card : page->Children()) {
            for (auto& child : card->Children()) {
                auto* combo = dynamic_cast<ui::ComboBox*>(child.get());
                if (combo && combo->dropped) {
                    // 检查点击是否在下拉区域内
                    float dropH = static_cast<float>(combo->items.size() * 32);
                    float boxY = combo->Y() + 5 + 30;
                    if (x >= combo->X() && x <= combo->X() + combo->Width() &&
                        y >= boxY && y <= boxY + dropH) {
                        // 选中下拉项
                        float relY = static_cast<float>(y) - boxY;
                        int idx = static_cast<int>(relY / 32);
                        if (idx >= 0 && idx < static_cast<int>(combo->items.size())) {
                            combo->selectedIndex = idx;
                        }
                        combo->dropped = false;
                        // settingsTheme 切换时刷新设置界面主题颜色
                        if (combo->id == "settingsTheme") {
                            ApplyChanges();  // 先收集新值到 editedConfig_
                            UpdateThemeColors();
                            // 同步更新系统标题栏颜色
                            BOOL darkMode = isDarkMode_ ? TRUE : FALSE;
                            DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
                            CreateBgGradientBrush();
                            ArrangeUI();
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            return;
                        }
                        // displayMode 切换时更新外观页和歌词页
                        if (combo->id == "displayMode") {
                            std::string newMode = (combo->selectedIndex == 1) ? "card" : "karaoke";
                            auto* appearPage = dynamic_cast<ui::AppearancePage*>(pages_[1].get());
                            if (appearPage) {
                                appearPage->UpdateVisibility(newMode);
                            }
                            auto* lyricsPage = dynamic_cast<ui::LyricsPage*>(pages_[0].get());
                            if (lyricsPage) {
                                // 需要临时收集当前配置来获取翻译模式值
                                moekoe::Config tmpCfg = currentConfig_;
                                CollectAllChanges(tmpCfg);
                                lyricsPage->UpdateForDisplayMode(newMode, tmpCfg);
                            }
                            ArrangeUI();
                        }
                        ApplyChanges();
                    } else {
                        // 点击下拉区域外 → 关闭下拉（不选择）
                        combo->dropped = false;
                    }
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;  // 下拉展开时拦截所有点击
                }
            }
        }
    }

    ui::UIElement* hit = HitTestV2(x, y);
    if (hit) {
        // Toggle 切换 → 实时应用
        auto* toggle = dynamic_cast<ui::Toggle*>(hit);
        if (toggle) {
            toggle->value = !toggle->value;
            ApplyChanges();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // NavItem 点击
        auto* navItem = dynamic_cast<ui::NavItem*>(hit);
        if (navItem && navItem->OnClick()) {
            navItem->OnClick()(navItem);
            ArrangeUI();
            return;
        }
        // Button 点击 → 实时应用
        auto* btn = dynamic_cast<ui::Button*>(hit);
        if (btn) {
            if (btn->id == "resetPos") {
                // 重置位置
                if (onConfigChanged_) {
                    moekoe::Config tmp = currentConfig_;
                    tmp.MutablePosition().offsetX = 0;
                    tmp.MutablePosition().offsetY = 0;
                    onConfigChanged_(tmp);
                }
            } else if (btn->id == "resetPort") {
                // 重置 WebSocket 端口为默认值 6520
                if (auto* advPage = dynamic_cast<ui::AdvancedPage*>(
                        pages_[4].get())) {
                    for (auto& card : advPage->Children()) {
                        for (auto& child : card->Children()) {
                            if (auto* lr = dynamic_cast<ui::LabelRow*>(child.get())) {
                                if (lr->id == "wsPort") {
                                    lr->textValue = "6520";
                                }
                            }
                        }
                    }
                }
                ApplyChanges();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (btn->id == "exportLog") {
                // 导出日志文件
                ExportLogFile();
            } else if (btn->id == "exportDiagnostic") {
                // 导出诊断信息
                ExportDiagnosticInfo();
            } else if (btn->OnClick()) {
                btn->OnClick()(btn);
            }
            return;
        }
        // Slider 开始拖动（拖动结束才应用，见 OnMouseUpV2）
        auto* slider = dynamic_cast<ui::Slider*>(hit);
        if (slider) {
            capturedElement_ = slider;
            slider->dragging = true;
            float relX = static_cast<float>(x) - slider->X();
            float ratio = std::clamp(relX / slider->Width(), 0.0f, 1.0f);
            slider->value = slider->minValue + ratio * (slider->maxValue - slider->minValue);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // ComboBox → 打开下拉
        auto* combo = dynamic_cast<ui::ComboBox*>(hit);
        if (combo) {
            combo->dropped = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // ColorRow 点击 → 弹出颜色选择器（选择完成后应用）
        auto* colorRow = dynamic_cast<ui::ColorRow*>(hit);
        if (colorRow) {
            activeColorRow_ = colorRow;
            colorPicker_.Activate(hwnd_, colorRow->colorValue, kTitleBarHeight, dpiScale_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // TextBlock 点击（如"关于"页面的项目名 → 跳转 GitHub）
        auto* textBlock = dynamic_cast<ui::TextBlock*>(hit);
        if (textBlock && textBlock->id == "about_name") {
            ShellExecuteW(nullptr, L"open",
                          L"https://github.com/Yngu196/TaskbarLyrics",
                          nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        // LabelRow 点击 → 弹出对话框 → 实时应用
        auto* labelRow = dynamic_cast<ui::LabelRow*>(hit);
        if (labelRow && !labelRow->readOnly) {
            if (labelRow->id == "fontFamily" || labelRow->id == "cardFontFamily") {
                LOGFONTW lf = {};
                lf.lfCharSet = DEFAULT_CHARSET;
                std::wstring wFont = Utf8ToWide(labelRow->textValue);
                wcsncpy_s(lf.lfFaceName, wFont.c_str(), LF_FACESIZE - 1);
                CHOOSEFONTW cf = {};
                cf.lStructSize = sizeof(cf);
                cf.hwndOwner = hwnd_;
                cf.lpLogFont = &lf;
                cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
                if (::ChooseFontW(&cf)) {
                    labelRow->textValue = WideToLocalUtf8(std::wstring(lf.lfFaceName));
                    ApplyChanges();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            } else if (labelRow->id == "wsPort") {
                // 弹出输入对话框修改端口
                std::wstring currentVal = Utf8ToWide(labelRow->textValue);
                // 使用简单的 InputDialog
                wchar_t buf[32] = {};
                wcsncpy_s(buf, currentVal.c_str(), 31);
                // 创建简单输入对话框
                if (SimpleInputDialog(hwnd_, L"修改 WebSocket 端口", L"请输入端口号（1-65535）：", buf, 32)) {
                    int port = _wtoi(buf);
                    if (port >= 1 && port <= 65535) {
                        labelRow->textValue = std::to_string(port);
                        ApplyChanges();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
            }
            return;
        }
        // ThemePresets 点击 → 实时应用
        auto* tp = dynamic_cast<ui::ThemePresets*>(hit);
        if (tp) {
            const int cols = static_cast<int>(tp->presets.size());
            const float gap = 8, swatchW = 36;
            const float totalW = cols * swatchW + (cols - 1) * gap;
            const float startX = tp->X() + (tp->Width() - totalW) / 2;
            float relX = static_cast<float>(x) - startX;
            if (relX >= 0) {
                int idx = static_cast<int>(relX / (swatchW + gap));
                if (idx >= 0 && idx < cols) {
                    tp->selectedIndex = idx;
                    // 同步更新颜色控件
                    if (tp->id == "themePresets") {
                        auto* appearPage = dynamic_cast<ui::AppearancePage*>(pages_[1].get());
                        if (appearPage) {
                            for (auto& card : appearPage->Children()) {
                                for (auto& child : card->Children()) {
                                    auto* cr = dynamic_cast<ui::ColorRow*>(child.get());
                                    if (cr) {
                                        if (cr->id == "normalColor") {
                                            cr->textValue = ColorFToHex(tp->presets[idx].nlColor);
                                            cr->colorValue = tp->presets[idx].nlColor;
                                        } else if (cr->id == "highlightColor") {
                                            cr->textValue = ColorFToHex(tp->presets[idx].hlColor);
                                            cr->colorValue = tp->presets[idx].hlColor;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ApplyChanges();
                }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
}

void D2DSettingsWindow::OnMouseMoveV2(int x, int y) {
    // 标题栏按钮悬停检测
    hoverClose_  = PtInRect(&closeBtnRect_, {x, y});
    hoverMax_    = PtInRect(&maxBtnRect_, {x, y});
    hoverMin_    = PtInRect(&minBtnRect_, {x, y});

    ui::UIElement* hit = HitTestV2(x, y);

    // 清除之前的悬停状态
    if (hoveredElement_ && hoveredElement_ != hit) {
        auto* toggle = dynamic_cast<ui::Toggle*>(hoveredElement_);
        if (toggle) toggle->hovered = false;
        auto* slider = dynamic_cast<ui::Slider*>(hoveredElement_);
        if (slider) slider->hovered = false;
        auto* combo = dynamic_cast<ui::ComboBox*>(hoveredElement_);
        if (combo) { combo->hovered = false; combo->hoveredDropIndex_ = -1; }
        auto* navItem = dynamic_cast<ui::NavItem*>(hoveredElement_);
        if (navItem) navItem->hovered = false;
        auto* btn = dynamic_cast<ui::Button*>(hoveredElement_);
        if (btn) btn->hovered = false;
        auto* colorRow = dynamic_cast<ui::ColorRow*>(hoveredElement_);
        if (colorRow) colorRow->hovered = false;
        auto* labelRow = dynamic_cast<ui::LabelRow*>(hoveredElement_);
        if (labelRow) labelRow->hovered = false;
        auto* tp = dynamic_cast<ui::ThemePresets*>(hoveredElement_);
        if (tp) tp->hoveredIndex = -1;
    }
    hoveredElement_ = hit;

    // 设置新悬停状态
    if (hit) {
        auto* toggle = dynamic_cast<ui::Toggle*>(hit);
        if (toggle) toggle->hovered = true;
        auto* slider = dynamic_cast<ui::Slider*>(hit);
        if (slider) slider->hovered = true;
        auto* combo = dynamic_cast<ui::ComboBox*>(hit);
        if (combo) {
            combo->hovered = true;
            // 更新下拉悬停项索引
            if (combo->dropped) {
                float boxY = combo->Y() + 5 + 30;
                float relY = static_cast<float>(y) - boxY;
                int idx = static_cast<int>(relY / 32);
                combo->hoveredDropIndex_ = (idx >= 0 && idx < static_cast<int>(combo->items.size())) ? idx : -1;
            } else {
                combo->hoveredDropIndex_ = -1;
            }
        }
        auto* navItem = dynamic_cast<ui::NavItem*>(hit);
        if (navItem) navItem->hovered = true;
        auto* btn = dynamic_cast<ui::Button*>(hit);
        if (btn) btn->hovered = true;
        auto* colorRow = dynamic_cast<ui::ColorRow*>(hit);
        if (colorRow) colorRow->hovered = true;
        auto* labelRow = dynamic_cast<ui::LabelRow*>(hit);
        if (labelRow) labelRow->hovered = true;
        // ThemePresets 悬停检测
        auto* tp = dynamic_cast<ui::ThemePresets*>(hit);
        if (tp) {
            const int cols = static_cast<int>(tp->presets.size());
            const float gap = 8, swatchW = 36;
            const float totalW = cols * swatchW + (cols - 1) * gap;
            const float startX = tp->X() + (tp->Width() - totalW) / 2;
            float relX = static_cast<float>(x) - startX;
            if (relX >= 0) {
                int idx = static_cast<int>(relX / (swatchW + gap));
                tp->hoveredIndex = (idx >= 0 && idx < cols) ? idx : -1;
            } else {
                tp->hoveredIndex = -1;
            }
        }
    }

    // Slider 拖动
    if (capturedElement_) {
        auto* slider = dynamic_cast<ui::Slider*>(capturedElement_);
        if (slider && slider->dragging) {
            float relX = static_cast<float>(x) - slider->X();
            float ratio = std::clamp(relX / slider->Width(), 0.0f, 1.0f);
            slider->value = slider->minValue + ratio * (slider->maxValue - slider->minValue);
        }
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void D2DSettingsWindow::OnMouseUpV2(int x, int y) {
    if (capturedElement_) {
        auto* slider = dynamic_cast<ui::Slider*>(capturedElement_);
        if (slider) {
            slider->dragging = false;
            // 滑块拖动结束 → 实时应用
            ApplyChanges();
        }
        capturedElement_ = nullptr;
    }
}

} // namespace moekoe
