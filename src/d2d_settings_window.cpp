// SPDX-License-Identifier: GPL-2.0
// d2d_settings_window.cpp - Direct2D 原生自绘设置界面实现
#include "d2d_settings_window.h"
#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <dwmapi.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

namespace moekoe {

using namespace Microsoft::WRL;

// 本地辅助：宽字符 → UTF-8
static std::string WideToLocalUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// 本地辅助：UTF-8 → 宽字符（正确处理多字节中文）
static std::string LocalUtf8ToWide(const std::string& utf8) {
    // 返回的是 UTF-8（因为 D2D/DWrite 接收的是 wchar_t，这里返回原始字符串）
    // 实际转换在 DrawTextLine 调用处进行
    return utf8;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

bool D2DSettingsWindow::classRegistered_ = false;

// ═══════════════════════════════
// 构造 / 析构
// ═══════════════════════════════

D2DSettingsWindow::D2DSettingsWindow() = default;
D2DSettingsWindow::~D2DSettingsWindow() { Close(); }

// ═══════════════════════════════
// 颜色工具
// ═══════════════════════════════

D2D1_COLOR_F D2DSettingsWindow::HexToColorF(const std::string& hex, float alpha) {
    D2D1_COLOR_F c = {0, 0, 0, alpha};
    if (hex.size() >= 7 && hex[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        if (std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            c.r = r / 255.0f; c.g = g / 255.0f; c.b = b / 255.0f;
        }
    }
    return c;
}

std::string D2DSettingsWindow::ColorFToHex(const D2D1_COLOR_F& c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                  static_cast<int>(c.r * 255),
                  static_cast<int>(c.g * 255),
                  static_cast<int>(c.b * 255));
    return buf;
}

D2D1_COLOR_F D2DSettingsWindow::Lerp(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// ═══════════════════════════════
// 暗色模式检测
// ═══════════════════════════════

void D2DSettingsWindow::DetectDarkMode() {
    // 检查 Windows 应用模式设置
    BOOL dark = FALSE;
    HRESULT hr = ::DwmGetWindowAttribute(
        ::GetDesktopWindow(), DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    isDarkMode_ = SUCCEEDED(hr) && dark;
}

void D2DSettingsWindow::UpdateThemeColors() {
    if (isDarkMode_) {
        theme_.bg            = HexToColorF("#1a1a2e");
        theme_.surface       = HexToColorF("#252542");
        theme_.border        = HexToColorF("#3a3a5c");
        theme_.text          = HexToColorF("#e8e8f0");
        theme_.textSecondary = HexToColorF("#9ca3af");
        theme_.accent        = HexToColorF("#4CC2FF");
        theme_.accentHover   = HexToColorF("#6bcfff");
    } else {
        theme_.bg            = HexToColorF("#ffffff");
        theme_.surface       = HexToColorF("#f5f7fa");
        theme_.border        = HexToColorF("#e0e4e8");
        theme_.text          = HexToColorF("#1a1a2e");
        theme_.textSecondary = HexToColorF("#6b7280");
        theme_.accent        = HexToColorF("#4CC2FF");
        theme_.accentHover   = HexToColorF("#3ab8f5");
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

    DetectDarkMode();
    UpdateThemeColors();

    // 注册窗口类（仅一次）
    if (!classRegistered_) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = &WndProc;
        wc.hInstance     = hInstance;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        // 背景画刷：空（自绘）
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); // 兜底背景色（D2D 会覆盖）
        classRegistered_ = (::RegisterClassExW(&wc) != 0);
    }

    // 创建窗口（不使用 WS_EX_LAYERED，避免 D2D 渲染异常）
    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,  // 显示在任务栏
        kWindowClass, L"任务栏歌词 - 设置",
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, kWinWidth, kWinHeight,  // 先创建在 (0,0)，下面再定位
        parent, nullptr, hInstance, this);

    if (!hwnd_) return false;

    // 居中显示到主显示器（或父窗口所在显示器）
    HMONITOR hMon = MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - kWinWidth) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - kWinHeight) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, kWinWidth, kWinHeight, SWP_NOZORDER);

    // DWM 圆角
    DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // 初始化 D2D
    if (!InitD2D()) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // 构建控件
    BuildControls(currentConfig);
    LayoutControls(contentWidth_);

    ShowWindow(hwnd_, SW_SHOW);
    SetFocus(hwnd_);
    return true;
}

bool D2DSettingsWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void D2DSettingsWindow::Close() {
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

    // 创建渲染目标（使用固定窗口尺寸，确保非零）
    hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_,
            D2D1_SIZE_U{static_cast<UINT>(kWinWidth), static_cast<UINT>(kWinHeight)}),
        &renderTarget_);
    if (FAILED(hr)) return false;

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
    controls_.clear();
}

// ═══════════════════════════════
// 控件构建
// ═══════════════════════════════

void D2DSettingsWindow::BuildControls(const Config& cfg) {
    controls_.clear();
    const auto& a = cfg.Appearance();
    const auto& adv = cfg.Advanced();

    auto addSection = [&](const std::string& title) {
        Control c;
        c.type = CtrlType::SectionHeader;
        c.label = title;
        controls_.push_back(c);
    };

    auto addLabelRow = [&](const std::string& id, const std::string& label,
                           const std::string& value, bool readOnly = false, int maxLen = 32) {
        Control c;
        c.type = CtrlType::LabelRow;
        c.id = id; c.label = label;
        c.textValue = value; c.readOnly = readOnly; c.textMaxLen = maxLen;
        controls_.push_back(c);
    };

    auto addToggle = [&](const std::string& id, const std::string& label, bool value) {
        Control c;
        c.type = CtrlType::ToggleRow;
        c.id = id; c.label = label; c.toggleValue = value;
        controls_.push_back(c);
    };

    auto addSlider = [&](const std::string& id, const std::string& label,
                         float minV, float maxV, float val, const std::string& suffix) {
        Control c;
        c.type = CtrlType::SliderRow;
        c.id = id; c.label = label;
        c.sliderMin = minV; c.sliderMax = maxV; c.sliderValue = val;
        c.sliderSuffix = suffix;
        controls_.push_back(c);
    };

    auto addColor = [&](const std::string& id, const std::string& label, const std::string& hex) {
        Control c;
        c.type = CtrlType::ColorRow;
        c.id = id; c.label = label;
        c.colorValue = HexToColorF(hex);
        c.textValue = hex;
        controls_.push_back(c);
    };

    auto addDropdown = [&](const std::string& id, const std::string& label,
                           const std::vector<std::string>& items, int selected) {
        Control c;
        c.type = CtrlType::DropdownRow;
        c.id = id; c.label = label;
        c.dropdownItems = items; c.dropdownSelected = selected;
        controls_.push_back(c);
    };

    auto addButton = [&](const std::string& id, const std::string& label,
                         const std::string& text, bool primary = false, bool danger = false) {
        Control c;
        c.type = CtrlType::ButtonRow;
        c.id = id; c.label = label;
        c.buttonText = text; c.isPrimary = primary; c.isDanger = danger;
        controls_.push_back(c);
    };

    auto addHint = [&](const std::string& text) {
        Control c;
        c.type = CtrlType::HintText;
        c.label = text;
        controls_.push_back(c);
    };

    auto addSpacer = [&]() {
        Control c;
        c.type = CtrlType::Spacer;
        controls_.push_back(c);
    };

    // ===== 外观 =====
    addSection("外观");

    // 显示模式
    addDropdown("displayMode", "显示模式",
                {"卡拉OK（默认）", "卡片样式"},
                a.displayMode == "card" ? 1 : 0);

    // 卡拉OK 区域的控件（始终构建，通过 visible 控制）
    addSlider("fontSize", "字号", 10.f, 28.f, static_cast<float>(a.fontSize), "");
    addLabelRow("fontFamily", "字体", a.fontFamily, /*readOnly*/true);
    addColor("normalColor", "普通歌词颜色", a.normalColor);
    addColor("highlightColor", "高亮歌词颜色", a.highlightColor);
    addSlider("opacity", "不透明度", 20.f, 100.f, a.normalOpacity * 100.f, "%");
    addToggle("karaoke", "卡拉OK 效果", a.enableKaraoke);
    addToggle("translation", "显示翻译", a.enableTranslation);

    // 跑马灯
    addSection("长歌词滚动（跑马灯）");
    addToggle("marquee", "启用跑马灯", a.enableMarquee);
    addDropdown("marqueeMode", "滚动模式",
                {"往返滚动（推荐）", "循环跑马灯", "关闭（截断显示）"},
                a.marqueeMode == "loop" ? 1 : (a.marqueeMode == "off" ? 2 : 0));
    addSlider("marqueeDelay", "开始延迟", 0.f, 5000.f, static_cast<float>(a.marqueeDelayMs), " ms");
    addSlider("marqueePause", "端点暂停", 0.f, 3000.f, static_cast<float>(a.marqueePauseMs), " ms");
    addSlider("marqueeSpeed", "滚动速度", 10.f, 200.f, a.marqueeSpeedPxPerSec, " px/s");

    // 预设主题
    {
        Control c;
        c.type = CtrlType::ThemePresets;
        c.id = "themePresets";
        c.themePresets = {
            {HexToColorF("#4CC2FF"), HexToColorF("#FFFFFF"), "默认"},
            {HexToColorF("#EC4141"), HexToColorF("#FFFFFF"), "网易云红"},
            {HexToColorF("#31C27C"), HexToColorF("#FFFFFF"), "QQ音乐绿"},
            {HexToColorF("#FF9800"), HexToColorF("#FFFFFF"), "暖橙"},
            {HexToColorF("#E040FB"), HexToColorF("#E0E0E0"), "紫罗兰"},
            {HexToColorF("#00E676"), HexToColorF("#B0BEC5"), "薄荷"},
        };
        // 检查当前是否匹配某个预设
        for (int i = 0; i < static_cast<int>(c.themePresets.size()); ++i) {
            if (ColorFToHex(c.themePresets[i].hlColor) == a.highlightColor &&
                ColorFToHex(c.themePresets[i].nlColor) == a.normalColor) {
                c.themeSelected = i;
                break;
            }
        }
        controls_.push_back(c);
    }

    // 卡片模式
    addSection("卡片样式设置");
    addSlider("cardFontSizeCurrent", "当前行字号", 10.f, 20.f,
              static_cast<float>(a.cardFontSizeCurrent), "");
    addSlider("cardFontSizeNext", "下一行字号", 8.f, 18.f,
              static_cast<float>(a.cardFontSizeNext), "");
    addColor("cardCurrentColor", "当前行颜色", a.cardCurrentColor);
    addColor("cardNextColor", "下一行颜色", a.cardNextColor);

    // ===== 位置 =====
    addSection("位置");
    addHint("拖动歌词窗口可直接调整位置");
    addButton("resetPos", "重置位置", "重置", false, /*danger*/true);

    // ===== 高级 =====
    addSection("高级");
    addLabelRow("wsPort", "WebSocket 端口", std::to_string(adv.websocketPort));
    addSlider("refreshRate", "刷新率", 15.f, 120.f, static_cast<float>(adv.refreshRateHz), " FPS");
    addToggle("debugLog", "调试日志", adv.debugLog);

    // ===== 通用 =====
    addSection("通用");
    addToggle("autoStart", "开机自动启动", cfg.IsAutoStart());

    // ===== 切换按钮 =====
    addSpacer();
    {
        Control c;
        c.type = CtrlType::SwitchUIBtn;
        c.id = "switchUI";
        c.buttonText = "切换到 WebView2 设置";
        controls_.push_back(c);
    }

    // ===== 操作按钮 =====
    addSpacer();
    {
        Control c;
        c.type = CtrlType::ButtonRow;
        c.id = "applyBtn";
        c.label = ""; // 右对齐按钮不需要标签
        c.buttonText = "应用并保存";
        c.isPrimary = true;
        controls_.push_back(c);
    }
    {
        Control c;
        c.type = CtrlType::ButtonRow;
        c.id = "cancelBtn";
        c.label = "";
        c.buttonText = "取消";
        controls_.push_back(c);
    }
}

void D2DSettingsWindow::LayoutControls(int contentWidth) {
    const int leftPad = 16;
    const int rightPad = 16;
    const int controlRight = contentWidth - rightPad;
    const int inputWidth = 140;
    const int sliderWidth = 160;
    const int colorBtnSize = 28;
    const int gap = 10;

    int y = kTitleBarHeight + 8; // 标题栏下方开始

    for (auto& c : controls_) {
        switch (c.type) {
        case CtrlType::SectionHeader:
            c.rect = {leftPad, y, controlRight, y + 24};
            y += 24 + sectionPadding_;
            break;

        case CtrlType::LabelRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::ToggleRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::SliderRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::ColorRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::DropdownRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::ButtonRow:
            if (c.id == "applyBtn" || c.id == "cancelBtn") {
                // 操作按钮右对齐
                int btnW = 100;
                c.rect = {controlRight - btnW - gap, y, controlRight, y + 32};
            } else {
                c.rect = {leftPad, y, controlRight, y + rowHeight_};
            }
            y += (c.id == "applyBtn" || c.id == "cancelBtn") ? 40 : rowHeight_;
            break;

        case CtrlType::ThemePresets:
            c.rect = {leftPad, y, controlRight, y + 44};
            y += 44;
            break;

        case CtrlType::HintText:
            c.rect = {leftPad, y, controlRight, y + 22};
            y += 22;
            break;

        case CtrlType::Spacer:
            y += 8;
            c.rect = {0, y, 0, y};
            break;

        case CtrlType::SwitchUIBtn:
            c.rect = {leftPad, y, controlRight, y + 34};
            y += 34;
            break;
        }
    }

    totalContentHeight_ = y + 20;
}

// ═══════════════════════════════
// 命中测试
// ═══════════════════════════════

D2DSettingsWindow::Control* D2DSettingsWindow::HitTest(int x, int y) {
    int testY = y + scrollOffset_; // 转换为内容坐标
    for (auto& c : controls_) {
        if (!c.visible) continue;
        if (testY >= c.rect.top && testY < c.rect.bottom &&
            x >= c.rect.left && x < c.rect.right) {
            return &c;
        }
    }
    return nullptr;
}

// ═══════════════════════════════
// 绘制
// ═══════════════════════════════

void D2DSettingsWindow::DrawAll() {
    if (!renderTarget_) return;

    renderTarget_->BeginDraw();
    renderTarget_->Clear(theme_.bg);

    // 更新画刷颜色（暗色模式切换后可能变化）
    bgBrush_->SetColor(theme_.bg);
    surfaceBrush_->SetColor(theme_.surface);
    borderBrush_->SetColor(theme_.border);
    textBrush_->SetColor(theme_.text);
    textSecondaryBrush_->SetColor(theme_.textSecondary);
    accentBrush_->SetColor(theme_.accent);
    accentHoverBrush_->SetColor(theme_.accentHover);

    const int clientH = [this]() {
        RECT rc; GetClientRect(hwnd_, &rc); return rc.bottom; }();

    // 绘制每个可见控件
    for (const auto& c : controls_) {
        if (!c.visible) continue;
        // 裁剪：只绘制在可视区域内的控件
        if (c.rect.bottom - scrollOffset_ < 0 || c.rect.top - scrollOffset_ > clientH)
            continue;

        switch (c.type) {
        case CtrlType::SectionHeader:   DrawSectionHeader(renderTarget_.Get(), c); break;
        case CtrlType::LabelRow:        DrawLabelRow(renderTarget_.Get(), c);      break;
        case CtrlType::ToggleRow:       DrawToggleRow(renderTarget_.Get(), c);      break;
        case CtrlType::SliderRow:       DrawSliderRow(renderTarget_.Get(), c);      break;
        case CtrlType::ColorRow:        DrawColorRow(renderTarget_.Get(), c);       break;
        case CtrlType::DropdownRow:     DrawDropdownRow(renderTarget_.Get(), c);    break;
        case CtrlType::ButtonRow:       DrawButtonRow(renderTarget_.Get(), c);      break;
        case CtrlType::ThemePresets:    DrawThemePresets(renderTarget_.Get(), c);   break;
        case CtrlType::HintText:        DrawHintText(renderTarget_.Get(), c);       break;
        case CtrlType::SwitchUIBtn:     DrawSwitchUIButton(renderTarget_.Get(), c);  break;
        default: break;
        }
    }

    // 绘制标题栏（在控件之上）
    DrawTitleBar(renderTarget_.Get());

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        renderTarget_.Reset();
        RECT rc; GetClientRect(hwnd_, &rc);
        d2dFactory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd_,
                D2D1_SIZE_U{static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom)}),
            &renderTarget_);
        // 重建画刷
        if (renderTarget_) {
            renderTarget_->CreateSolidColorBrush(theme_.bg, &bgBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.surface, &surfaceBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.border, &borderBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.text, &textBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.textSecondary, &textSecondaryBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.accent, &accentBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.accentHover, &accentHoverBrush_);
        }
    }
}

// ── 绘制辅助：圆角矩形 ──
static void FillRoundedRect(ID2D1RenderTarget* rt, ID2D1Brush* brush,
                              float x, float y, float w, float h, float r) {
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x, y, x+w, y+h), r, r};
    rt->FillRoundedRectangle(rr, brush);
}

static void DrawRoundedRect(ID2D1RenderTarget* rt, ID2D1Brush* brush, float strokeWidth,
                             float x, float y, float w, float h, float r) {
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x, y, x+w, y+h), r, r};
    rt->DrawRoundedRectangle(rr, brush, strokeWidth);
}

// ── 绘制文字 ──
static void DrawTextLine(ID2D1RenderTarget* rt, IDWriteTextFormat* fmt,
                          ID2D1Brush* brush, const wchar_t* text,
                          float x, float y, float maxWidth) {
    rt->DrawText(text, static_cast<UINT>(wcslen(text)), fmt,
                 D2D1::RectF(x, y, x + maxWidth, y + 200), brush);
}

void D2DSettingsWindow::DrawSectionHeader(ID2D1RenderTarget* rt, const Control& c) {
    float cy = static_cast<float>(c.rect.top) - scrollOffset_ + 12.f;

    // 左侧竖线装饰
    ComPtr<ID2D1SolidColorBrush> accentBr;
    rt->CreateSolidColorBrush(theme_.accent, &accentBr);
    FillRoundedRect(rt, accentBr.Get(),
                    static_cast<float>(c.rect.left), cy - 7.f, 3.f, 14.f, 1.5f);

    // 标题文字
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, sectionFmt_.Get(), textSecondaryBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left) + 10.f, cy - 9.f, 400.f);
}

void D2DSettingsWindow::DrawLabelRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 160.f);

    // 输入框区域
    float inputLeft = static_cast<float>(c.rect.right) - 150.f;
    float inputTop = top + 6.f;
    float inputW = 140.f;
    float inputH = 26.f;

    // 背景
    ComPtr<ID2D1SolidColorBrush> inputBg;
    rt->CreateSolidColorBrush(c.editing ? theme_.bg : theme_.surface, &inputBg);
    FillRoundedRect(rt, inputBg.Get(), inputLeft, inputTop, inputW, inputH, 5.f);

    // 边框（焦点时高亮）
    ComPtr<ID2D1SolidColorBrush> borderBr;
    rt->CreateSolidColorBrush(c.editing ? theme_.accent : theme_.border, &borderBr);
    DrawRoundedRect(rt, borderBr.Get(), 1.f, inputLeft, inputTop, inputW, inputH, 5.f);

    // 文字
    std::wstring wideVal = Utf8ToWide(c.textValue);;
    ComPtr<ID2D1SolidColorBrush> txtBr;
    rt->CreateSolidColorBrush(c.readOnly ? theme_.textSecondary : theme_.text, &txtBr);
    DrawTextLine(rt, valueFmt_.Get(), txtBr.Get(),
                 wideVal.c_str(), inputLeft + 8.f, mid - 7.f, inputW - 16.f);

    // 光标
    if (c.editing && c.showCaret) {
        ComPtr<ID2D1SolidColorBrush> caretBr;
        rt->CreateSolidColorBrush(theme_.text, &caretBr);
        // 计算光标 X 位置
        float caretX = inputLeft + 8.f;
        if (c.caretPos > 0 && !wideVal.empty()) {
            DWRITE_TEXT_METRICS metrics{};
            ComPtr<IDWriteTextLayout> layout;
            dwriteFactory_->CreateTextLayout(
                wideVal.c_str(), static_cast<UINT>(c.caretPos),
                valueFmt_.Get(), 200.f, 30.f, &layout);
            if (layout) layout->GetMetrics(&metrics);
            caretX += metrics.widthIncludingTrailingWhitespace;
        }
        rt->DrawLine(D2D1::Point2F(caretX, inputTop + 4.f),
                     D2D1::Point2F(caretX, inputTop + inputH - 4.f),
                     caretBr.Get(), 1.f);
    }

    // 字体选择按钮（仅 fontFamily 控件）
    if (c.id == "fontFamily") {
        float btnX = inputLeft - 50.f;
        float btnW = 42.f;
        float btnH = 26.f;
        ComPtr<ID2D1SolidColorBrush> btnBg;
        rt->CreateSolidColorBrush(hoverCtrl_ == &c ? theme_.surface : theme_.bg, &btnBg);
        FillRoundedRect(rt, btnBg.Get(), btnX, top + 6.f, btnW, btnH, 5.f);
        ComPtr<ID2D1SolidColorBrush> btnBorder;
        rt->CreateSolidColorBrush(hoverCtrl_ == &c ? theme_.accent : theme_.border, &btnBorder);
        DrawRoundedRect(rt, btnBorder.Get(), 1.f, btnX, top + 6.f, btnW, btnH, 5.f);
        const wchar_t* pickText = L"选择";
        DrawTextLine(rt, hintFmt_.Get(), textBrush_.Get(),
                     pickText, btnX + 5.f, mid - 6.f, 36.f);
    }
}

void D2DSettingsWindow::DrawToggleRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 220.f);

    // Toggle 开关（右侧）
    float tw = 36.f, th = 20.f, tr = 10.f;
    float tx = static_cast<float>(c.rect.right) - tw - 16.f;
    float ty = mid - th / 2.f;

    ComPtr<ID2D1SolidColorBrush> trackBr;
    rt->CreateSolidColorBrush(c.toggleValue ? theme_.accent : theme_.border, &trackBr);
    FillRoundedRect(rt, trackBr.Get(), tx, ty, tw, th, tr);

    // 圆形滑块
    float knobR = 8.f;
    float knobX = c.toggleValue ? tx + tw - knobR - 2.f : tx + 2.f;
    float knobY = ty + th / 2.f;
    ComPtr<ID2D1SolidColorBrush> knobBr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &knobBr);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX + knobR, knobY), knobR, knobR), knobBr.Get());
}

void D2DSettingsWindow::DrawSliderRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 120.f);

    // 滑块轨道
    float sl = static_cast<float>(c.rect.right) - 180.f;
    float sw = sliderWidth_; // 160
    float st = mid - 2.f;
    float sh = 4.f;

    ComPtr<ID2D1SolidColorBrush> trackBr;
    rt->CreateSolidColorBrush(theme_.border, &trackBr);
    FillRoundedRect(rt, trackBr.Get(), sl, st, sw, sh, 2.f);

    // 已填充部分
    float fillRatio = (c.sliderValue - c.sliderMin) / (c.sliderMax - c.sliderMin);
    float fillW = sw * fillRatio;
    ComPtr<ID2D1SolidColorBrush> fillBr;
    rt->CreateSolidColorBrush(theme_.accent, &fillBr);
    FillRoundedRect(rt, fillBr.Get(), sl, st, fillW, sh, 2.f);

    // 滑块手柄
    float handleX = sl + fillW;
    float handleR = 7.f;
    ComPtr<ID2D1SolidColorBrush> handleBr;
    rt->CreateSolidColorBrush(theme_.accent, &handleBr);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(handleX, mid), handleR, handleR), handleBr.Get());
    ComPtr<ID2D1SolidColorBrush> handleBorder;
    rt->CreateSolidColorBrush(theme_.bg, &handleBorder);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(handleX, mid), handleR, handleR), handleBorder.Get(), 2.f);

    // 数值显示
    char valBuf[32];
    if (c.sliderSuffix == "%")
        std::snprintf(valBuf, sizeof(valBuf), "%.0f%s", c.sliderValue, c.sliderSuffix.c_str());
    else if (c.sliderSuffix.find("ms") != std::string::npos)
        std::snprintf(valBuf, sizeof(valBuf), "%.0f %s", c.sliderValue, c.sliderSuffix.c_str());
    else if (c.sliderSuffix.find("px/s") != std::string::npos)
        std::snprintf(valBuf, sizeof(valBuf), "%.0f %s", c.sliderValue, c.sliderSuffix.c_str());
    else if (c.sliderSuffix.find("FPS") != std::string::npos)
        std::snprintf(valBuf, sizeof(valBuf), "%.0f %s", c.sliderValue, c.sliderSuffix.c_str());
    else
        std::snprintf(valBuf, sizeof(valBuf), "%.0f", c.sliderValue);

    std::wstring wval(valBuf, valBuf + strlen(valBuf));
    DrawTextLine(rt, valueFmt_.Get(), textSecondaryBrush_.Get(),
                 wval.c_str(), sl + sw + 10.f, mid - 7.f, 60.f);
}

void D2DSettingsWindow::DrawColorRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 120.f);

    // 颜色方块
    float cbSz = 28.f;
    float cbX = static_cast<float>(c.rect.right) - 95.f - cbSz;
    float cbY = top + (static_cast<float>(rowHeight_) - cbSz) / 2.f;
    ComPtr<ID2D1SolidColorBrush> colorBr;
    rt->CreateSolidColorBrush(c.colorValue, &colorBr);
    FillRoundedRect(rt, colorBr.Get(), cbX, cbY, cbSz, cbSz, 5.f);
    ComPtr<ID2D1SolidColorBrush> colorBorder;
    rt->CreateSolidColorBrush(theme_.border, &colorBorder);
    DrawRoundedRect(rt, colorBorder.Get(), 1.5f, cbX, cbY, cbSz, cbSz, 5.f);

    // Hex 输入
    float hexX = cbX + cbSz + 6.f;
    float hexW = 75.f;
    float hexH = 26.f;
    float hexY = top + 6.f;
    ComPtr<ID2D1SolidColorBrush> hexBg;
    rt->CreateSolidColorBrush(theme_.surface, &hexBg);
    FillRoundedRect(rt, hexBg.Get(), hexX, hexY, hexW, hexH, 5.f);
    ComPtr<ID2D1SolidColorBrush> hexBorder;
    rt->CreateSolidColorBrush(theme_.border, &hexBorder);
    DrawRoundedRect(rt, hexBorder.Get(), 1.f, hexX, hexY, hexW, hexH, 5.f);

    std::wstring wideVal = Utf8ToWide(c.textValue);;
    DrawTextLine(rt, valueFmt_.Get(), textBrush_.Get(),
                 wideVal.c_str(), hexX + 6.f, mid - 7.f, hexW - 12.f);
}

void D2DSettingsWindow::DrawDropdownRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 120.f);

    // 下拉框背景
    float ddW = 150.f, ddH = 26.f;
    float ddX = static_cast<float>(c.rect.right) - ddW - 16.f;
    float ddY = top + 6.f;
    ComPtr<ID2D1SolidColorBrush> ddBg;
    rt->CreateSolidColorBrush(theme_.surface, &ddBg);
    FillRoundedRect(rt, ddBg.Get(), ddX, ddY, ddW, ddH, 5.f);
    ComPtr<ID2D1SolidColorBrush> ddBorder;
    rt->CreateSolidColorBrush(hoverCtrl_ == &c ? theme_.accent : theme_.border, &ddBorder);
    DrawRoundedRect(rt, ddBorder.Get(), 1.f, ddX, ddY, ddW, ddH, 5.f);

    // 选中项文字
    if (c.dropdownSelected >= 0 && c.dropdownSelected < static_cast<int>(c.dropdownItems.size())) {
        std::wstring sel = Utf8ToWide(c.dropdownItems[c.dropdownSelected]);
        DrawTextLine(rt, valueFmt_.Get(), textBrush_.Get(),
                     sel.c_str(), ddX + 8.f, mid - 7.f, ddW - 30.f);
    }

    // 下拉箭头 ▾
    const wchar_t* arrow = L"\u25BE"; // ▾
    DrawTextLine(rt, labelFmt_.Get(), textSecondaryBrush_.Get(),
                 arrow, ddX + ddW - 18.f, mid - 8.f, 14.f);
}

void D2DSettingsWindow::DrawButtonRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;

    if (c.id == "applyBtn" || c.id == "cancelBtn") {
        // 底部操作按钮
        float bw = c.isPrimary ? 110.f : 70.f;
        float bh = 30.f;
        float bx = static_cast<float>(c.rect.left);
        float by = top + 4.f;

        bool hovered = hoverCtrl_ == &c;
        ComPtr<ID2D1SolidColorBrush> btnBg, btnBorder, btnText;

        if (c.isPrimary) {
            rt->CreateSolidColorBrush(hovered ? theme_.accentHover : theme_.accent, &btnBg);
            rt->CreateSolidColorBrush(hovered ? theme_.accentHover : theme_.accent, &btnBorder);
            rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &btnText);
        } else if (c.isDanger) {
            rt->CreateSolidColorBrush(hovered ? theme_.surface : D2D1::ColorF(1,1,1,1), &btnBg);
            rt->CreateSolidColorBrush(hovered ? HexToColorF("#ff4d4f") : theme_.border, &btnBorder);
            rt->CreateSolidColorBrush(hovered ? HexToColorF("#ff4d4f") : theme_.text, &btnText);
        } else {
            rt->CreateSolidColorBrush(hovered ? theme_.surface : theme_.bg, &btnBg);
            rt->CreateSolidColorBrush(hovered ? theme_.accent : theme_.border, &btnBorder);
            rt->CreateSolidColorBrush(hovered ? theme_.accent : theme_.text, &btnText);
        }

        FillRoundedRect(rt, btnBg.Get(), bx, by, bw, bh, 6.f);
        DrawRoundedRect(rt, btnBorder.Get(), 1.f, bx, by, bw, bh, 6.f);

        std::wstring wtxt = Utf8ToWide(c.buttonText);;
        DrawTextLine(rt, btnFmt_.Get(), btnText.Get(),
                     wtxt.c_str(), bx + 14.f, by + 6.f, bw - 28.f);
    } else {
        // 行内按钮（如重置位置）
        float mid = top + static_cast<float>(rowHeight_) / 2.f;
        std::wstring wide = Utf8ToWide(c.label);
        DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                     wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 160.f);

        float bw = 56.f, bh = 26.f;
        float bx = static_cast<float>(c.rect.right) - bw - 16.f;
        float by = top + 6.f;
        bool hovered = hoverCtrl_ == &c;
        ComPtr<ID2D1SolidColorBrush> btnBg, btnBorder, btnTxt;
        rt->CreateSolidColorBrush(hovered ? theme_.surface : theme_.bg, &btnBg);
        rt->CreateSolidColorBrush(hovered ? (c.isDanger ? HexToColorF("#ff4d4f") : theme_.accent) : theme_.border, &btnBorder);
        rt->CreateSolidColorBrush(hovered ? (c.isDanger ? HexToColorF("#ff4d4f") : theme_.accent) : theme_.text, &btnTxt);
        FillRoundedRect(rt, btnBg.Get(), bx, by, bw, bh, 6.f);
        DrawRoundedRect(rt, btnBorder.Get(), 1.f, bx, by, bw, bh, 6.f);
        std::wstring wtxt = Utf8ToWide(c.buttonText);;
        DrawTextLine(rt, btnFmt_.Get(), btnTxt.Get(),
                     wtxt.c_str(), bx + 10.f, by + 6.f, bw - 20.f);
    }
}

void D2DSettingsWindow::DrawThemePresets(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float startX = static_cast<float>(c.rect.left) + 4.f;
    float y = top + 8.f;
    float sz = 24.f;
    float gap = 6.f;

    for (int i = 0; i < static_cast<int>(c.themePresets.size()); ++i) {
        float x = startX + i * (sz + gap);
        const auto& preset = c.themePresets[i];
        bool selected = (c.themeSelected == i);
        bool hovered = (hoverCtrl_ == &c); // 简化：整个区域悬停

        // 渐变背景色块
        ComPtr<ID2D1LinearGradientBrush> gradBr;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgbp = {};
        lgbp.startPoint = D2D1::Point2F(x, y);
        lgbp.endPoint   = D2D1::Point2F(x + sz, y + sz);
        D2D1_GRADIENT_STOP stops[2] = {
            {0.f, preset.hlColor},
            {1.f, preset.nlColor},
        };
        ComPtr<ID2D1GradientStopCollection> gradStops;
        rt->CreateGradientStopCollection(stops, 2, &gradStops);
        rt->CreateLinearGradientBrush(lgbp, gradStops.Get(), &gradBr);

        // 圆形色块
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + sz/2, y + sz/2), sz/2, sz/2), gradBr.Get());

        // 选中边框
        if (selected) {
            ComPtr<ID2D1SolidColorBrush> selBr;
            rt->CreateSolidColorBrush(theme_.text, &selBr);
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + sz/2, y + sz/2), sz/2 + 2.f, sz/2 + 2.f), selBr.Get(), 2.f);
        } else {
            ComPtr<ID2D1SolidColorBrush> defBr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0,0,0,0), &defBr);
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + sz/2, y + sz/2), sz/2, sz/2), defBr.Get(), 2.f);
        }
    }
}

void D2DSettingsWindow::DrawHintText(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, hintFmt_.Get(), textSecondaryBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), top, 380.f);
}

void D2DSettingsWindow::DrawSwitchUIButton(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(c.rect.bottom - c.rect.top) / 2.f;

    bool hovered = hoverCtrl_ == &c;

    ComPtr<ID2D1SolidColorBrush> btnBg, btnBorder, btnText;
    rt->CreateSolidColorBrush(hovered ? theme_.surface : theme_.bg, &btnBg);
    rt->CreateSolidColorBrush(hovered ? theme_.accent : theme_.border, &btnBorder);
    rt->CreateSolidColorBrush(hovered ? theme_.accent : theme_.textSecondary, &btnText);

    float bw = 200.f, bh = 28.f;
    float bx = static_cast<float>(c.rect.left);
    float by = mid - bh / 2.f;

    FillRoundedRect(rt, btnBg.Get(), bx, by, bw, bh, 6.f);
    DrawRoundedRect(rt, btnBorder.Get(), 1.f, bx, by, bw, bh, 6.f);

    std::wstring wtxt = Utf8ToWide(c.buttonText);;
    DrawTextLine(rt, btnFmt_.Get(), btnText.Get(),
                 wtxt.c_str(), bx + 16.f, by + 5.f, bw - 32.f);
}

void D2DSettingsWindow::DrawTitleBar(ID2D1RenderTarget* rt) {
    RECT rc; GetClientRect(hwnd_, &rc);
    float W = static_cast<float>(rc.right);
    float H = static_cast<float>(kTitleBarHeight);

    // 标题栏背景（半透明毛玻璃效果）
    ComPtr<ID2D1SolidColorBrush> titleBg;
    D2D1_COLOR_F bgColor = theme_.bg;
    bgColor.a = isDarkMode_ ? 0.92f : 0.85f;
    rt->CreateSolidColorBrush(bgColor, &titleBg);
    rt->FillRectangle(D2D1::RectF(0, 0, W, H), titleBg.Get());

    // 底部分隔线
    ComPtr<ID2D1SolidColorBrush> lineBr;
    rt->CreateSolidColorBrush(theme_.border, &lineBr);
    rt->DrawLine(D2D1::Point2F(0, H - 0.5f), D2D1::Point2F(W, H - 0.5f), lineBr.Get());

    // 标题文字
    std::wstring wtitle = Utf8ToWide("任务栏歌词 - 设置");
    DrawTextLine(rt, labelFmt_.Get(), textSecondaryBrush_.Get(),
                 wtitle.c_str(), 14.f, (H - 16.f) / 2.f, W - 100.f);

    // 关闭按钮 × （右侧）
    float btnSize = 28.f;
    float btnY = (H - btnSize) / 2.f;
    float closeX = W - btnSize - 4.f;
    closeBtnRect_ = {static_cast<int>(closeX), static_cast<int>(btnY),
                     static_cast<int>(closeX + btnSize), static_cast<int>(btnY + btnSize)};

    // 最小化按钮 — （关闭按钮左侧）
    float minX = closeX - btnSize - 2.f;
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

    // 更新标题栏区域（用于拖动判定）
    titleBarRect_ = {0, 0, rc.right, kTitleBarHeight};
}

// ═══════════════════════════════
// 事件处理
// ═══════════════════════════════

void D2DSettingsWindow::OnMouseDown(int x, int y) {
    Control* hit = HitTest(x, y);
    if (!hit) return;

    captureCtrl_ = hit;
    hit->pressed = true;

    switch (hit->type) {
    case CtrlType::ToggleRow:
        hit->toggleValue = !hit->toggleValue;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;

    case CtrlType::LabelRow:
        if (hit->id == "fontFamily") {
            // 点击了"选择"按钮区域
            LOGFONT lf{};
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfHeight = -12;
            wcsncpy_s(lf.lfFaceName, LF_FACESIZE,
                      Utf8ToWide(hit->textValue).c_str(), _TRUNCATE);
            CHOOSEFONTW cf{};
            cf.lStructSize = sizeof(cf);
            cf.hwndOwner = hwnd_;
            cf.lpLogFont = &lf;
            cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
            if (::ChooseFontW(&cf)) {
                hit->textValue = WideToLocalUtf8(std::wstring(lf.lfFaceName));
                editedConfig_.MutableAppearance().fontFamily = hit->textValue;
            }
        } else if (!hit->readOnly) {
            // 开始编辑文本
            for (auto& c : controls_) c.editing = false;
            hit->editing = true;
            hit->caretPos = static_cast<int>(hit->textValue.size());
            hit->showCaret = true;
            hit->caretBlinkTime = 0;
            SetCapture(hwnd_);
        }
        break;

    case CtrlType::ColorRow: {
        // 打开系统颜色选择器
        COLORREF customColors[16]{};
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = hwnd_;
        cc.rgbResult = RGB(
            static_cast<int>(hit->colorValue.r * 255),
            static_cast<int>(hit->colorValue.g * 255),
            static_cast<int>(hit->colorValue.b * 255));
        cc.lpCustColors = customColors;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (::ChooseColorW(&cc)) {
            hit->colorValue.r = GetRValue(cc.rgbResult) / 255.f;
            hit->colorValue.g = GetGValue(cc.rgbResult) / 255.f;
            hit->colorValue.b = GetBValue(cc.rgbResult) / 255.f;
            hit->textValue = ColorFToHex(hit->colorValue);
        }
        break;
    }

    case CtrlType::DropdownRow:
        // 循环选择
        hit->dropdownSelected = (hit->dropdownSelected + 1) %
                               static_cast<int>(hit->dropdownItems.size());
        break;

    case CtrlType::ButtonRow:
        if (hit->id == "applyBtn") ::PostMessageW(hwnd_, kMsgApplySave, 0, 0);
        else if (hit->id == "cancelBtn") ::PostMessageW(hwnd_, kMsgCancel, 0, 0);
        else if (hit->id == "resetPos") {
            editedConfig_.MutablePosition().offsetX = 0;
            editedConfig_.MutablePosition().offsetY = 0;
        }
        break;

    case CtrlType::ThemePresets: {
        // 计算点击了哪个预设
        float relX = static_cast<float>(x) - (hit->rect.left + 4.f);
        float relY = static_cast<float>(y) + scrollOffset_ - (hit->rect.top + 8.f);
        float sz = 24.f, gap = 6.f;
        int idx = static_cast<int>((relX) / (sz + gap));
        if (idx >= 0 && idx < static_cast<int>(hit->themePresets.size())) {
            hit->themeSelected = idx;
            // 应用预设到对应的颜色控件
            const auto& preset = hit->themePresets[idx];
            for (auto& c : controls_) {
                if (c.id == "highlightColor") {
                    c.colorValue = preset.hlColor;
                    c.textValue = ColorFToHex(preset.hlColor);
                }
                if (c.id == "normalColor") {
                    c.colorValue = preset.nlColor;
                    c.textValue = ColorFToHex(preset.nlColor);
                }
            }
        }
        break;
    }

    case CtrlType::SwitchUIBtn:
        // 延迟切换：PostMessage 避免 delete this 后返回已销毁对象
        pendingSwitchMode_ = "webview";
        ::PostMessageW(hwnd_, kMsgSwitchMode, 0, 0);
        break;

    default:
        break;
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void D2DSettingsWindow::OnMouseUp(int x, int y) {
    if (captureCtrl_) {
        captureCtrl_->pressed = false;
        captureCtrl_ = nullptr;
    }
    ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void D2DSettingsWindow::OnMouseMove(int x, int y) {
    // 标题栏按钮悬停检测
    bool newHoverClose = PtInRect(&closeBtnRect_, {x, y});
    bool newHoverMin   = PtInRect(&minBtnRect_, {x, y});
    if (newHoverClose != hoverClose_ || newHoverMin != hoverMin_) {
        hoverClose_ = newHoverClose;
        hoverMin_ = newHoverMin;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return; // 标题栏按钮悬停变化时优先处理
    }

    Control* hit = HitTest(x, y);

    // 滑块拖动
    if (captureCtrl_ && captureCtrl_->type == CtrlType::SliderRow) {
        float relX = static_cast<float>(x) -
                     (static_cast<float>(captureCtrl_->rect.right) - 180.f);
        float ratio = relX / sliderWidth_;
        ratio = std::clamp(ratio, 0.f, 1.f);
        captureCtrl_->sliderValue = captureCtrl_->sliderMin +
                                   ratio * (captureCtrl_->sliderMax - captureCtrl_->sliderMin);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (hit != hoverCtrl_) {
        hoverCtrl_ = hit;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void D2DSettingsWindow::OnMouseWheel(int delta) {
    const int clientH = [this]() {
        RECT rc; GetClientRect(hwnd_, &rc); return rc.bottom; }();
    int maxScroll = std::max(0, totalContentHeight_ - clientH);
    scrollOffset_ -= delta / WHEEL_DELTA * 48;
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void D2DSettingsWindow::OnChar(wchar_t ch) {
    // 找到正在编辑的控件
    for (auto& c : controls_) {
        if (c.editing) {
            if (ch == VK_BACK) {
                if (c.caretPos > 0) {
                    c.textValue.erase(c.caretPos - 1, 1);
                    --c.caretPos;
                }
            } else if (ch >= 32 && static_cast<int>(c.textValue.size()) < c.textMaxLen) {
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) {
                    c.textValue.insert(c.caretPos, mb, n);
                    ++c.caretPos;
                }
            }
            c.showCaret = true;
            c.caretBlinkTime = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
    }
}

void D2DSettingsWindow::OnKeyDown(UINT key) {
    for (auto& c : controls_) {
        if (c.editing) {
            switch (key) {
            case VK_LEFT:
                if (c.caretPos > 0) --c.caretPos;
                break;
            case VK_RIGHT:
                if (c.caretPos < static_cast<int>(c.textValue.size())) ++c.caretPos;
                break;
            case VK_HOME: c.caretPos = 0; break;
            case VK_END:  c.caretPos = static_cast<int>(c.textValue.size()); break;
            case VK_RETURN:
                c.editing = false;
                ReleaseCapture();
                break;
            case VK_ESCAPE:
                c.editing = false;
                ReleaseCapture();
                break;
            default:
                return;
            }
            c.showCaret = true;
            c.caretBlinkTime = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
    }
}

void D2DSettingsWindow::OnLoseFocus() {
    for (auto& c : controls_) c.editing = false;
    ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ═══════════════════════════════
// 应用配置 / 取消
// ═══════════════════════════════

void D2DSettingsWindow::ApplyAndSave() {
    // 从控件收集值到 editedConfig_
    for (const auto& c : controls_) {
        auto& ap = editedConfig_.MutableAppearance();
        auto& adv = editedConfig_.MutableAdvanced();

        if (c.id == "displayMode") {
            ap.displayMode = (c.dropdownSelected == 1) ? "card" : "karaoke";
        } else if (c.id == "fontSize") {
            ap.fontSize = static_cast<int>(c.sliderValue);
        } else if (c.id == "fontFamily") {
            ap.fontFamily = c.textValue;
        } else if (c.id == "normalColor") {
            ap.normalColor = ColorFToHex(c.colorValue);
        } else if (c.id == "highlightColor") {
            ap.highlightColor = ColorFToHex(c.colorValue);
        } else if (c.id == "opacity") {
            ap.normalOpacity = c.sliderValue / 100.f;
        } else if (c.id == "karaoke") {
            ap.enableKaraoke = c.toggleValue;
        } else if (c.id == "translation") {
            ap.enableTranslation = c.toggleValue;
        } else if (c.id == "marquee") {
            ap.enableMarquee = c.toggleValue;
        } else if (c.id == "marqueeMode") {
            ap.marqueeMode = (c.dropdownSelected == 1) ? "loop" :
                             (c.dropdownSelected == 2) ? "off" : "bounce";
        } else if (c.id == "marqueeDelay") {
            ap.marqueeDelayMs = static_cast<int>(c.sliderValue);
        } else if (c.id == "marqueePause") {
            ap.marqueePauseMs = static_cast<int>(c.sliderValue);
        } else if (c.id == "marqueeSpeed") {
            ap.marqueeSpeedPxPerSec = c.sliderValue;
        } else if (c.id == "cardFontSizeCurrent") {
            ap.cardFontSizeCurrent = static_cast<int>(c.sliderValue);
        } else if (c.id == "cardFontSizeNext") {
            ap.cardFontSizeNext = static_cast<int>(c.sliderValue);
        } else if (c.id == "cardCurrentColor") {
            ap.cardCurrentColor = ColorFToHex(c.colorValue);
        } else if (c.id == "cardNextColor") {
            ap.cardNextColor = ColorFToHex(c.colorValue);
        } else if (c.id == "wsPort") {
            adv.websocketPort = atoi(c.textValue.c_str());
        } else if (c.id == "refreshRate") {
            adv.refreshRateHz = static_cast<int>(c.sliderValue);
        } else if (c.id == "debugLog") {
            adv.debugLog = c.toggleValue;
        } else if (c.id == "autoStart") {
            editedConfig_.SetAutoStart(c.toggleValue);
        }
    }

    // 回调通知主程序（确保 settingsUiMode 为 d2d）
    editedConfig_.MutableAdvanced().settingsUiMode = "d2d";
    if (onConfigChanged_) onConfigChanged_(editedConfig_);

    Log("[D2D-SETTINGS] Config applied and saved\n");
    Close();
}

void D2DSettingsWindow::Cancel() {
    Log("[D2D-SETTINGS] Settings cancelled\n");
    Close();
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

    case WM_PAINT:
        if (self) self->DrawAll();
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;

    case WM_ERASEBKGND:
        return 1; // 防止闪烁

    case WM_LBUTTONDOWN: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

            // 标题栏关闭按钮
            if (PtInRect(&self->closeBtnRect_, {x, y})) {
                ::PostMessageW(hwnd, D2DSettingsWindow::kMsgCancel, 0, 0);
                return 0;
            }
            // 标题栏最小化按钮
            if (PtInRect(&self->minBtnRect_, {x, y})) {
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            }
            // 标题栏拖动区域
            if (PtInRect(&self->titleBarRect_, {x, y})) {
                ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                               MAKELPARAM(x, y));
                return 0;
            }

            Control* hit = self->HitTest(x, y);
            if (hit) {
                self->OnMouseDown(x, y); // 点击了控件，正常处理
            } else {
                // 点击空白区域，启动系统拖动
                ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                               MAKELPARAM(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            self->OnMouseUp(x, y);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            self->OnMouseMove(x, y);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (self) self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }
    case WM_CHAR:
        if (self) self->OnChar(static_cast<wchar_t>(wParam));
        return 0;
    case WM_KEYDOWN:
        if (self) self->OnKeyDown(static_cast<UINT>(wParam));
        return 0;
    case WM_KILLFOCUS:
        if (self) self->OnLoseFocus();
        return 0;

    case D2DSettingsWindow::kMsgSwitchMode:
        // 延迟执行的 UI 模式切换（此时已脱离 OnMouseDown 调用栈，安全 delete this）
        if (self && self->onSwitchMode_ && !self->pendingSwitchMode_.empty()) {
            std::string mode = std::move(self->pendingSwitchMode_);
            self->onSwitchMode_(mode);
        }
        return 0;

    case D2DSettingsWindow::kMsgApplySave:
        if (self) self->ApplyAndSave();
        return 0;

    case D2DSettingsWindow::kMsgCancel:
        if (self) self->Cancel();
        return 0;

    case WM_TIMER: {
        // 光标闪烁
        if (self && wParam == 1) {
            LARGE_INTEGER freq, now;
            ::QueryPerformanceFrequency(&freq);
            ::QueryPerformanceCounter(&now);
            double nowSec = static_cast<double>(now.QuadPart) / freq.QuadPart;
            for (auto& c : self->controls_) {
                if (c.editing && (nowSec - c.caretBlinkTime) > 0.53) {
                    c.showCaret = !c.showCaret;
                    c.caretBlinkTime = nowSec;
                    InvalidateRect(self->hwnd_, nullptr, FALSE);
                    break;
                }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        // 设置对话框关闭，不退出主程序（不能调用 PostQuitMessage）
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace moekoe
