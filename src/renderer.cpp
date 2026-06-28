// SPDX-License-Identifier: GPL-2.0
// renderer.cpp - Direct2D + DirectWrite 渲染实现
// 完全透明背景: WIC + UpdateLayeredWindow + 逐字高亮
#include "renderer.h"
#include "constants.h"
#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "urlmon.lib")

namespace moekoe {

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len);
    return out;
}

/// 取 UTF-8 字符串首字符（wchar_t）。空串 / 无效 UTF-8 返回 '?'。处理多字节码点（中文等）。
wchar_t FirstUtf8CharAsWide(const std::string& s) {
    if (s.empty()) return L'?';
    // 只转首字符：MultiByteToWideChar 配合 MB_ERR_INVALID_CHARS 确保取完整码点
    int len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), 4, nullptr, 0);
    if (len <= 0) {
        // 无效 UTF-8 回退单字节 → '?'
        return L'?';
    }
    wchar_t ch = L'?';
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), 4, &ch, 1);
    return ch;
}

} // namespace

TaskbarRenderer::TaskbarRenderer() = default;

TaskbarRenderer::~TaskbarRenderer() {
    Shutdown();
}

D2D1_COLOR_F TaskbarRenderer::ParseColor(const std::string& hex, float alpha) {
    if (hex.size() == 7 && hex[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf_s(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, alpha);
        }
    }
    return D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha);
}

bool TaskbarRenderer::Initialize(HWND hwnd) {
    if (initialized_) return true;
    if (!hwnd) return false;
    hwnd_ = hwnd;

    dpi_ = ::GetDpiForWindow(hwnd);
    RECT rc{};
    ::GetWindowRect(hwnd, &rc);
    width_  = static_cast<UINT>(std::max<LONG>(rc.right - rc.left, 1));
    height_ = static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 1));

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        d2dFactory_.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = ::CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IWICImagingFactory),
        reinterpret_cast<void**>(wicFactory_.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
    if (FAILED(hr)) return false;

    CreateRenderTarget();

    // 创建画刷（与 Resize 中逻辑一致）
    if (renderTarget_) {
        highlightBrush_.Reset();
        normalBrush_.Reset();
        translationBrush_.Reset();
        spectrumBrush_.Reset();
        cardCurrentBrush_.Reset();
        cardNextBrush_.Reset();
        cardBackgroundBrush_.Reset();
        const D2D1_COLOR_F hi = ParseColor(settings_.highlightColor, 1.0f);
        const D2D1_COLOR_F no = ParseColor(settings_.normalColor, static_cast<float>(settings_.normalOpacity));
        renderTarget_->CreateSolidColorBrush(hi, highlightBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(no, normalBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.7f, 0.7f, 0.7f, 0.8f),
            translationBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(ParseColor(settings_.highlightColor), spectrumBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            ParseColor(settings_.cardCurrentColor, 1.0f),
            cardCurrentBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            ParseColor(settings_.cardNextColor, 1.0f),
            cardNextBrush_.GetAddressOf());
        coverThemeColor_ = D2D1::ColorF(0.45f, 0.45f, 0.50f, 1.0f);
        renderTarget_->CreateSolidColorBrush(coverThemeColor_, cardBackgroundBrush_.GetAddressOf());
    }

    const std::wstring family = Utf8ToWide(settings_.fontFamily);
    if (dwriteFactory_ && !family.empty()) {
        dwriteFactory_->CreateTextFormat(
            family.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            static_cast<FLOAT>(settings_.fontSize),
            L"zh-CN",
            textFormat_.GetAddressOf());
        if (textFormat_) {
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        dwriteFactory_->CreateTextFormat(
            family.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            std::max<FLOAT>(8.0f, static_cast<FLOAT>(settings_.fontSize) - constants::TRANSLATION_FONT_SIZE_DELTA),
            L"zh-CN",
            translationFormat_.GetAddressOf());
        if (translationFormat_) {
            translationFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            translationFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            translationFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        // 控制按钮图标文字格式（字体大小基于窗口高度）
        dwriteFactory_->CreateTextFormat(
            L"Segoe UI Symbol", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            std::max<FLOAT>(8.0f, static_cast<FLOAT>(height_) * 0.49f),
            L"en-US",
            btnFormat_.GetAddressOf());
        if (btnFormat_) {
            btnFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            btnFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // 卡片模式文本格式（两行不同字号）
        // 优先使用卡片专属字体 cardFontFamily，未设置时回落 fontFamily
        const std::wstring cardFamily = settings_.cardFontFamily.empty()
            ? family
            : Utf8ToWide(settings_.cardFontFamily);
        dwriteFactory_->CreateTextFormat(
            cardFamily.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            (std::max)(8.0f, static_cast<FLOAT>(settings_.cardFontSizeCurrent)),
            L"zh-CN",
            cardCurrentFormat_.GetAddressOf());
        if (cardCurrentFormat_) {
            cardCurrentFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            cardCurrentFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            cardCurrentFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        dwriteFactory_->CreateTextFormat(
            cardFamily.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            (std::max)(8.0f, static_cast<FLOAT>(settings_.cardFontSizeNext)),
            L"zh-CN",
            cardNextFormat_.GetAddressOf());
        if (cardNextFormat_) {
            cardNextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            cardNextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            cardNextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        // 卡片模式 fallback 格式：无封面时绘制歌名首字符，字号与封面区域匹配
        dwriteFactory_->CreateTextFormat(
            cardFamily.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            (std::max)(10.0f, static_cast<FLOAT>(height_) * 0.35f),
            L"zh-CN",
            cardFallbackFormat_.GetAddressOf());
        if (cardFallbackFormat_) {
            cardFallbackFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            cardFallbackFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            cardFallbackFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    if (renderTarget_) {
        const D2D1_COLOR_F hi = ParseColor(settings_.highlightColor, 1.0f);
        const D2D1_COLOR_F no = ParseColor(settings_.normalColor, static_cast<float>(settings_.normalOpacity));
        renderTarget_->CreateSolidColorBrush(hi, highlightBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(no, normalBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.7f, 0.7f, 0.7f, 0.8f),
            translationBrush_.GetAddressOf());
        // 频谱画刷（使用高亮色）
        renderTarget_->CreateSolidColorBrush(ParseColor(settings_.highlightColor), spectrumBrush_.GetAddressOf());
        // 卡片模式专用颜色画刷
        renderTarget_->CreateSolidColorBrush(
            ParseColor(settings_.cardCurrentColor, 1.0f),
            cardCurrentBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            ParseColor(settings_.cardNextColor, 1.0f),
            cardNextBrush_.GetAddressOf());
    }

    initialized_ = (renderTarget_ != nullptr);
    return initialized_;
}

void TaskbarRenderer::CreateRenderTarget() {
    if (!d2dFactory_ || !wicFactory_) return;

    wicBitmap_.Reset();
    renderTarget_.Reset();
    // 依赖旧 renderTarget 的 Layer/Geometry 一并失效
    coverLayer_.Reset();
    coverClipGeo_.Reset();
    cachedCoverSize_ = -1.0f;

    HRESULT hr = wicFactory_->CreateBitmap(
        std::max<UINT>(1, width_), std::max<UINT>(1, height_),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnDemand,
        wicBitmap_.GetAddressOf());
    if (FAILED(hr)) return;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = d2dFactory_->CreateWicBitmapRenderTarget(
        wicBitmap_.Get(), props,
        reinterpret_cast<ID2D1RenderTarget**>(renderTarget_.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        renderTarget_->SetDpi(
            static_cast<FLOAT>(dpi_), static_cast<FLOAT>(dpi_));
    }
}

void TaskbarRenderer::ApplySettings(const AppearanceConfig& s) {
    settings_ = s;
    if (initialized_) {
        textFormat_.Reset();
        translationFormat_.Reset();
        highlightBrush_.Reset();
        normalBrush_.Reset();
        translationBrush_.Reset();
        HWND h = hwnd_;
        Shutdown();
        Initialize(h);
    }
}

void TaskbarRenderer::Shutdown() {
    cardNextBrush_.Reset();
    cardCurrentBrush_.Reset();
    cardBackgroundBrush_.Reset();
    d2dCoverBitmap_.Reset();
    blurredCoverBg_.Reset();
    blurredBgBrush_.Reset();
    blurredBgBitmapW_ = 0.0f;
    cachedCoverUrl_.clear();       // 清除 URL 缓存，避免重建后误判无需下载
    coverLoadInProgress_.store(false, std::memory_order_release);
    coverDownloadGen_.store(0, std::memory_order_release);  // 重置代际计数器
    {
        std::lock_guard<std::mutex> lock(pendingCoverMutex_);
        pendingCoverData_.reset();
    }
    coverLayer_.Reset();
    coverClipGeo_.Reset();
    cachedCoverSize_ = -1.0f;
    cardNextFormat_.Reset();
    cardCurrentFormat_.Reset();
    cardFallbackFormat_.Reset();
    translationBrush_.Reset();
    highlightBrush_.Reset();
    normalBrush_.Reset();
    spectrumBrush_.Reset();
    translationFormat_.Reset();
    textFormat_.Reset();
    renderTarget_.Reset();
    wicBitmap_.Reset();
    dwriteFactory_.Reset();
    wicFactory_.Reset();
    d2dFactory_.Reset();
    initialized_ = false;
    hwnd_ = nullptr;
}

void TaskbarRenderer::Resize(UINT width, UINT height, UINT dpi) {
    width_  = width;
    height_ = height;
    dpi_    = dpi;
    CreateRenderTarget();
    if (renderTarget_) {
        highlightBrush_.Reset();
        normalBrush_.Reset();
        translationBrush_.Reset();
        spectrumBrush_.Reset();
        cardCurrentBrush_.Reset();
        cardNextBrush_.Reset();
        const D2D1_COLOR_F hi = ParseColor(settings_.highlightColor, 1.0f);
        const D2D1_COLOR_F no = ParseColor(settings_.normalColor, static_cast<float>(settings_.normalOpacity));
        renderTarget_->CreateSolidColorBrush(hi, highlightBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(no, normalBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.7f, 0.7f, 0.7f, 0.8f),
            translationBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(ParseColor(settings_.highlightColor), spectrumBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            ParseColor(settings_.cardCurrentColor, 1.0f),
            cardCurrentBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            ParseColor(settings_.cardNextColor, 1.0f),
            cardNextBrush_.GetAddressOf());
        coverThemeColor_ = D2D1::ColorF(0.45f, 0.45f, 0.50f, 1.0f);
        renderTarget_->CreateSolidColorBrush(coverThemeColor_, cardBackgroundBrush_.GetAddressOf());
    }
    // 重建按钮文字格式（高度可能变化）
    btnFormat_.Reset();
    if (dwriteFactory_) {
        dwriteFactory_->CreateTextFormat(
            L"Segoe UI Symbol", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            std::max<FLOAT>(8.0f, static_cast<FLOAT>(height_) * 0.49f),
            L"en-US",
            btnFormat_.GetAddressOf());
        if (btnFormat_) {
            btnFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            btnFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

void TaskbarRenderer::DrawCentered(const std::wstring& text, ID2D1Brush* brush, float yOffset) {
    if (!renderTarget_ || !textFormat_ || !brush || text.empty()) return;
    
    // 水平偏移量（像素）
    const float paddingX = constants::TEXT_PADDING_X;
    
    D2D1_RECT_F layout = D2D1::RectF(
        paddingX, yOffset,
        static_cast<FLOAT>(width_) - paddingX, yOffset + static_cast<FLOAT>(height_));
    renderTarget_->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()),
        textFormat_.Get(), layout, brush);
}

void TaskbarRenderer::DrawHighlightedTextPerCharacter(const std::wstring& text,
                                                      double progress,
                                                      bool enableKaraoke,
                                                      float scrollOffset,
                                                      const float* overridePaddingX,
                                                      float opacity) {
    if (!renderTarget_ || !textFormat_ || text.empty() ||
        !highlightBrush_ || !normalBrush_) {
        return;
    }
    const UINT32 length = static_cast<UINT32>(text.size());
    if (length == 0) return;

    const float paddingX = overridePaddingX ? *overridePaddingX : constants::TEXT_PADDING_X;
    const float availableWidth = static_cast<FLOAT>(width_) - paddingX * 2.0f;

    D2D1_RECT_F layoutRect = D2D1::RectF(
        paddingX, 0.0f, static_cast<FLOAT>(width_) - paddingX, static_cast<FLOAT>(height_));

    // ── 缓存文本布局：仅在歌词内容变化时重建 CreateTextLayout ──
    bool layoutValid = false;
    DWRITE_TEXT_METRICS metrics{};
    if (cachedLayout_ && text == cachedKaraokeText_) {
        layoutValid = true;
    } else {
        // 文本变化 → 重建布局并缓存
        Microsoft::WRL::ComPtr<IDWriteTextLayout> newLayout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                text.c_str(), length, textFormat_.Get(),
                layoutRect.right - layoutRect.left, static_cast<FLOAT>(height_),
                newLayout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(newLayout->GetMetrics(&m))) {
                cachedLayout_ = newLayout;
                cachedKaraokeText_ = text;
                cachedTextWidth_ = m.width;
                layoutValid = true;
            }
        }
    }

    const float textWidth = layoutValid ? cachedTextWidth_ : 0.0f;

    // ── 判断是否需要跑马灯滚动 ──
    const bool needsMarquee = (layoutValid && textWidth > availableWidth + 1.0f
                               && marqueeState_ != MarqueeState::Idle);

    // P3-①: 传入 opacity<1 时为歌词行切换 fade 过渡，用 PushLayer 整体做透明度
    Microsoft::WRL::ComPtr<ID2D1Layer> fadeLayer;
    bool layerPushed = false;
    if (opacity < 1.0f) {
        // 创建匿名 layer（不指定 geometry → 覆盖整个 render target 区域）
        HRESULT hrLayer = renderTarget_->CreateLayer(nullptr, fadeLayer.GetAddressOf());
        if (SUCCEEDED(hrLayer) && fadeLayer) {
            D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
                D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(),
                opacity, nullptr,
                D2D1_LAYER_OPTIONS_NONE);
            renderTarget_->PushLayer(layerParams, fadeLayer.Get());
            layerPushed = true;
        }
    }

    if (needsMarquee) {
        cachedLayout_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const float textLeft = paddingX - scrollOffset;

        renderTarget_->DrawTextLayout(
            D2D1::Point2F(textLeft, 0.0f), cachedLayout_.Get(), normalBrush_.Get());

        if (enableKaraoke && progress > 0.0) {
            const float highlightWidth = std::min(textWidth * static_cast<float>(progress), textWidth);
            if (highlightWidth > 0.0f) {
                D2D1_RECT_F clipRect = D2D1::RectF(
                    textLeft, 0.0f,
                    textLeft + highlightWidth,
                    static_cast<FLOAT>(height_));
                renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                renderTarget_->DrawTextLayout(
                    D2D1::Point2F(textLeft, 0.0f), cachedLayout_.Get(), highlightBrush_.Get());
                renderTarget_->PopAxisAlignedClip();
            }
        }
    } else {
        // ═══════ 非滚动模式：居中显示 ═══════
        renderTarget_->DrawTextW(
            text.c_str(), length, textFormat_.Get(), layoutRect, normalBrush_.Get());

        if (enableKaraoke && progress > 0.0 && layoutValid) {
            const float highlightWidth = std::min(textWidth * static_cast<float>(progress), textWidth);
            if (highlightWidth > 0.0f) {
                const float centeredLeft = paddingX + (availableWidth - textWidth) / 2.0f;
                D2D1_RECT_F clipRect = D2D1::RectF(
                    centeredLeft, 0.0f,
                    centeredLeft + highlightWidth,
                    static_cast<FLOAT>(height_));
                renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                renderTarget_->DrawTextW(
                    text.c_str(), length, textFormat_.Get(), layoutRect, highlightBrush_.Get());
                renderTarget_->PopAxisAlignedClip();
            }
        }
    }

    if (layerPushed) {
        renderTarget_->PopLayer();
    }
}

void TaskbarRenderer::DrawTranslatedText(const std::wstring& text, const float* overridePaddingX, float opacity) {
    if (!translationFormat_ || !translationBrush_ || text.empty()) return;

    // 水平偏移量（像素）：支持垂直模式下的自定义内边距
    const float paddingX = overridePaddingX ? *overridePaddingX : constants::TEXT_PADDING_X;

    // P3-①: 歌词行切换 fade 过渡期间，旧行翻译通过 opacity<1 渐隐
    if (opacity < 1.0f) {
        translationBrush_->SetOpacity(opacity);
    }
    
    D2D1_RECT_F layout = D2D1::RectF(
        paddingX, static_cast<FLOAT>(height_) * 0.55f,
        static_cast<FLOAT>(width_) - paddingX, static_cast<FLOAT>(height_));
    renderTarget_->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()),
        translationFormat_.Get(),
        layout, translationBrush_.Get());

    if (opacity < 1.0f) {
        translationBrush_->SetOpacity(1.0f); // 恢复默认
    }
}

void TaskbarRenderer::DrawHoverControls(bool isPlaying) {
    if (!renderTarget_ || !normalBrush_) return;

    const FLOAT w = static_cast<FLOAT>(width_);
    const FLOAT h = static_cast<FLOAT>(height_);

    if (isVerticalTaskbar_) {
        // ── 垂直任务栏：按钮垂直堆叠（窄窗口放不下水平排列）──
        const FLOAT btnSize = std::min(w * 0.7f, 28.0f);
        const FLOAT spacing = constants::BUTTON_SPACING;
        const FLOAT totalBtnHeight = btnSize * 3.0f + spacing * 2.0f;
        const FLOAT btnX = (w - btnSize) / 2.0f;
        const FLOAT startY = (h - totalBtnHeight) / 2.0f;

        // 半透明背景（竖条）
        D2D1_RECT_F bgRect = D2D1::RectF(
            btnX - constants::BUTTON_BG_PADDING_X, startY - constants::BUTTON_BG_PADDING_Y,
            btnX + btnSize + constants::BUTTON_BG_PADDING_X, startY + totalBtnHeight + constants::BUTTON_BG_PADDING_Y);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f),
            bgBrush.GetAddressOf());
        if (bgBrush) {
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(bgRect, constants::BUTTON_BG_BORDER_RADIUS, constants::BUTTON_BG_BORDER_RADIUS), bgBrush.Get());
        }

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f),
            iconBrush.GetAddressOf());
        if (!iconBrush || !btnFormat_) return;

        // 上一首 ⏮ (顶部)
        D2D1_RECT_F prevRect = D2D1::RectF(btnX, startY, btnX + btnSize, startY + btnSize);
        renderTarget_->DrawTextW(L"\u23EE", 1, btnFormat_.Get(), prevRect, iconBrush.Get());

        // 暂停/播放 ⏸/▶ (中间)
        FLOAT ppY = startY + btnSize + spacing;
        D2D1_RECT_F ppRect = D2D1::RectF(btnX, ppY, btnX + btnSize, ppY + btnSize);
        renderTarget_->DrawTextW(isPlaying ? L"\u23F8" : L"\u25B6", 1, btnFormat_.Get(), ppRect, iconBrush.Get());

        // 下一首 ⏭ (底部)
        FLOAT nextY = startY + (btnSize + spacing) * 2.0f;
        D2D1_RECT_F nextRect = D2D1::RectF(btnX, nextY, btnX + btnSize, nextY + btnSize);
        renderTarget_->DrawTextW(L"\u23ED", 1, btnFormat_.Get(), nextRect, iconBrush.Get());
    } else {
        // ── 水平任务栏：按钮水平排列（原有逻辑）──
        const FLOAT btnSize = h * 0.7f;
        const FLOAT spacing = constants::BUTTON_SPACING;
        const FLOAT totalBtnWidth = btnSize * 3.0f + spacing * 2.0f;
        const FLOAT startX = (w - totalBtnWidth) / 2.0f;
        const FLOAT btnY = (h - btnSize) / 2.0f;

        // 半透明背景
        D2D1_RECT_F bgRect = D2D1::RectF(
            startX - constants::BUTTON_BG_PADDING_X, btnY - constants::BUTTON_BG_PADDING_Y,
            startX + totalBtnWidth + constants::BUTTON_BG_PADDING_X, btnY + btnSize + constants::BUTTON_BG_PADDING_Y);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f),
            bgBrush.GetAddressOf());
        if (bgBrush) {
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(bgRect, constants::BUTTON_BG_BORDER_RADIUS, constants::BUTTON_BG_BORDER_RADIUS), bgBrush.Get());
        }

        // 按钮符号颜色
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f),
            iconBrush.GetAddressOf());
        if (!iconBrush || !btnFormat_) return;

        // 上一首 ⏮ (U+23EE)
        D2D1_RECT_F prevRect = D2D1::RectF(startX, btnY, startX + btnSize, btnY + btnSize);
        renderTarget_->DrawTextW(L"\u23EE", 1, btnFormat_.Get(), prevRect, iconBrush.Get());

        // 暂停/播放 ⏸ (U+23F8) / ▶ (U+25B6)
        FLOAT ppX = startX + btnSize + spacing;
        D2D1_RECT_F ppRect = D2D1::RectF(ppX, btnY, ppX + btnSize, btnY + btnSize);
        renderTarget_->DrawTextW(isPlaying ? L"\u23F8" : L"\u25B6", 1, btnFormat_.Get(), ppRect, iconBrush.Get());

        // 下一首 ⏭ (U+23ED)
        FLOAT nextX = startX + (btnSize + spacing) * 2.0f;
        D2D1_RECT_F nextRect = D2D1::RectF(nextX, btnY, nextX + btnSize, btnY + btnSize);
        renderTarget_->DrawTextW(L"\u23ED", 1, btnFormat_.Get(), nextRect, iconBrush.Get());
    }
}

// ═════════════════════════════════════════
// 卡片模式渲染（无卡拉OK效果）
// ═════════════════════════════════════════

void TaskbarRenderer::RenderCardStyle(const RenderState& state) {
    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float coverSize = static_cast<float>(settings_.cardCoverSize) * dpiScale;
    const float gap = static_cast<float>(settings_.cardGap) * dpiScale;
    const float paddingX = constants::TEXT_PADDING_X;

    // ═════ P1-①+P1-④: 卡片背景（柔焦封面 + 主题色叠加） ═════
    if (cardBackgroundBrush_) {
        D2D1_RECT_F bgRect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        D2D1_ROUNDED_RECT bgRR = D2D1::RoundedRect(bgRect,
            constants::CARD_COVER_RADIUS_DP * dpiScale,
            constants::CARD_COVER_RADIUS_DP * dpiScale);
        // P1-④: 先绘制柔焦封面背景（若有）
        if (blurredBgBrush_ && blurredBgBitmapW_ > 0.0f) {
            float sx = static_cast<float>(width_) / blurredBgBitmapW_;
            float sy = static_cast<float>(height_) / blurredBgBitmapW_;
            blurredBgBrush_->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy));
            blurredBgBrush_->SetOpacity(constants::COVER_BLUR_BG_ALPHA);
            renderTarget_->FillRoundedRectangle(bgRR, blurredBgBrush_.Get());
            blurredBgBrush_->SetOpacity(1.0f);
            blurredBgBrush_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        // P1-①: 再叠加半透明主题色调（统一卡片氛围）
        cardBackgroundBrush_->SetOpacity(constants::COVER_THEME_ALPHA);
        renderTarget_->FillRoundedRectangle(bgRR, cardBackgroundBrush_.Get());
        cardBackgroundBrush_->SetOpacity(1.0f);
    }

    // ═════ 1. 绘制封面（左侧） ═════
    wchar_t fallback = FirstUtf8CharAsWide(state.songName);
    DrawCoverArt(state.coverArtUrl, fallback, paddingX,
                 (static_cast<float>(height_) - coverSize) / 2.0f, coverSize);

    // ═════ 2. 绘制双行歌词（封面右侧，无卡拉OK逐字效果） ═════
    const float lyricsX = paddingX + coverSize + gap;
    const float lyricsWidth = static_cast<float>(width_) - lyricsX - paddingX;

    if (lyricsWidth <= 10.0f) return;

    std::wstring curW = Utf8ToWide(state.currentLine);
    std::wstring nextW = Utf8ToWide(state.nextLine);
    const float h = static_cast<float>(height_);
    const float halfH = h * 0.50f;

    if (cardAnimState_ == CardAnimState::Animating && cardAnimProgress_ > 0.001f
        && cardAnimProgress_ < 1.0f) {
        // ═════ 动画中：双轨淡入淡出 + 位移 ═════
        //
        // 设计：新旧歌词分别绘制在两个"轨道"上：
        //   旧轨道：向上位移 -halfH + 淡出 alpha 1→0
        //   新轨道：从下方 halfH 处位移滑入 0 + 淡入 alpha 0→1
        //
        // 两条轨道有独立的位移/透明度曲线，中间有自然"过渡"，
        // 不会出现生硬的卷轴切变感。

        const float t = cardAnimProgress_;

        // 旧轨道：位移 0→-halfH（向上滑出，线性更快离开）；透明度 (1-t)² 快淡出
        const float upOffset = -t * halfH;
        const float fadeOutAlpha = std::max(0.0f, (1.0f - t) * (1.0f - t));

        // 新轨道：位移 halfH→0（从下方滑入）；透明度 0→1
        const float inOffset = (1.0f - EaseOutBack(t)) * halfH;
        const float fadeInAlpha = EaseOutCubic(std::min(t / 0.85f, 1.0f));

        std::wstring oldCurW = Utf8ToWide(cardPrevCurrentLine_);
        std::wstring oldNextW = Utf8ToWide(cardPrevNextLine_);

        // ── 裁剪到歌词区域，避免文本溢出 ──
        D2D1_RECT_F clipRect = D2D1::RectF(lyricsX, 0.0f, lyricsX + lyricsWidth, h);
        renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // ═════ 旧轨道（仅保留旧下一行：向上滑入当前行位置 + 淡出） ═════
        // 旧当前行直接消失（无淡出动画），避免同位置出现两行文字的重影
        const float vNudgeCur = 4.0f * dpiScale;
        const float vNudgeNext = 2.0f * dpiScale;
        if (!oldNextW.empty()) {
            DrawCardLyricsSingle(oldNextW, lyricsX, halfH - vNudgeNext, lyricsWidth,
                                 upOffset, fadeOutAlpha,
                                 /*isCurrent=*/false);
        }

        // ═════ 新轨道（从下方滑入 + 淡入） ═════
        if (!curW.empty()) {
            DrawCardLyricsSingle(curW, lyricsX, vNudgeCur, lyricsWidth,
                                 inOffset, fadeInAlpha,
                                 /*isCurrent=*/true);
        }
        if (!nextW.empty()) {
            DrawCardLyricsSingle(nextW, lyricsX, halfH - vNudgeNext, lyricsWidth,
                                 inOffset, fadeInAlpha,
                                 /*isCurrent=*/false);
        }

        renderTarget_->PopAxisAlignedClip();
    } else {
        // ═════ 非动画状态：正常双行绘制 ═════
        DrawCardLyrics(curW, nextW, lyricsX, 0.0f, lyricsWidth, 0.0f, 1.0f);
    }
}

// ═══════════════════════════════════════
// 垂直任务栏卡片模式：堆叠式布局
// ═══════════════════════════════════════
//
// 窗口窄而高（~180px 宽），水平并排的封面+歌词放不下。
// 改为垂直堆叠：
//   ┌──────────────┐
//   │   [封面]     │  ← 居中，缩小
//   │              │
//   │ 当前行歌词   │  ← 居中单行
//   │ 下一行歌词   │  ← 居中单行（灰色小字）
//   └──────────────┘

void TaskbarRenderer::RenderCardStyleVertical(const RenderState& state) {
    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float paddingX = constants::TEXT_PADDING_X * 0.5f;  // 窄窗口减小左右边距
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);

    // 封面尺寸：缩小以适应窄窗口
    const float coverSize = std::min(
        static_cast<float>(settings_.cardCoverSize) * dpiScale * 0.8f,
        w - paddingX * 2.0f);

    // ═════ P1-①+P1-④: 绘制卡片背景（柔焦封面 + 主题色叠加） ═════
    if (cardBackgroundBrush_) {
        D2D1_RECT_F bgRect = D2D1::RectF(0.0f, 0.0f, w, h);
        D2D1_ROUNDED_RECT bgRR = D2D1::RoundedRect(bgRect,
            constants::CARD_COVER_RADIUS_DP * dpiScale,
            constants::CARD_COVER_RADIUS_DP * dpiScale);
        // P1-④: 先绘制柔焦封面背景（若有）
        if (blurredBgBrush_ && blurredBgBitmapW_ > 0.0f) {
            float sx = w / blurredBgBitmapW_;
            float sy = h / blurredBgBitmapW_;
            blurredBgBrush_->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy));
            blurredBgBrush_->SetOpacity(constants::COVER_BLUR_BG_ALPHA);
            renderTarget_->FillRoundedRectangle(bgRR, blurredBgBrush_.Get());
            blurredBgBrush_->SetOpacity(1.0f);
            blurredBgBrush_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        cardBackgroundBrush_->SetOpacity(constants::COVER_THEME_ALPHA);
        renderTarget_->FillRoundedRectangle(bgRR, cardBackgroundBrush_.Get());
        cardBackgroundBrush_->SetOpacity(1.0f);
    }

    // ═════ 1. 封面（居中顶部） ═════
    wchar_t fallback = FirstUtf8CharAsWide(state.songName);
    const float coverX = (w - coverSize) / 2.0f;
    const float coverY = paddingX;
    DrawCoverArt(state.coverArtUrl, fallback, coverX, coverY, coverSize);

    // ═════ 2. 歌词文本（封面下方，居中对齐） ═════
    const float lyricsTop = coverY + coverSize + paddingX * 0.5f;
    const float lyricsWidth = w - paddingX * 2.0f;

    if (lyricsWidth <= 10.0f) return;

    std::wstring curW = Utf8ToWide(state.currentLine);
    std::wstring nextW = Utf8ToWide(state.nextLine);

    // 动画处理：垂直模式下仅使用淡入淡出（不做位移，避免在窄窗口中跳动）
    if (cardAnimState_ == CardAnimState::Animating && cardAnimProgress_ > 0.001f
        && cardAnimProgress_ < 1.0f) {

        const float t = cardAnimProgress_;
        const float fadeOutAlpha = std::max(0.0f, (1.0f - t) * (1.0f - t));
        const float fadeInAlpha = EaseOutCubic(std::min(t / 0.85f, 1.0f));

        std::wstring oldCurW = Utf8ToWide(cardPrevCurrentLine_);
        std::wstring oldNextW = Utf8ToWide(cardPrevNextLine_);

        D2D1_RECT_F clipRect = D2D1::RectF(paddingX, lyricsTop, w - paddingX, h);
        renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const float vNudgeCur = 4.0f * dpiScale;
        const float vNudgeNext = 2.0f * dpiScale;
        const float halfH = h * 0.50f;

        // 旧内容淡出（仅保留旧下一行）
        if (!oldNextW.empty()) {
            DrawCardLyricsSingle(oldNextW, paddingX, lyricsTop + halfH - vNudgeNext, lyricsWidth,
                                 0.0f, fadeOutAlpha,
                                 /*isCurrent=*/false);
        }

        // 新内容淡入
        if (!curW.empty()) {
            DrawCardLyricsSingle(curW, paddingX, lyricsTop + vNudgeCur, lyricsWidth,
                                 0.0f, fadeInAlpha,
                                 /*isCurrent=*/true);
        }
        if (!nextW.empty()) {
            DrawCardLyricsSingle(nextW, paddingX, lyricsTop + halfH - vNudgeNext, lyricsWidth,
                                 0.0f, fadeInAlpha,
                                 /*isCurrent=*/false);
        }

        renderTarget_->PopAxisAlignedClip();
    } else {
        // 非动画状态：正常绘制
        DrawCardLyrics(curW, nextW, paddingX, lyricsTop, lyricsWidth, 0.0f, 1.0f);
    }
}

void TaskbarRenderer::DrawCoverArt(const std::string& url, wchar_t fallbackChar,
                                    float x, float y, float size) {
    if (!renderTarget_ || size <= 0.0f) return;

    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float radius = constants::CARD_COVER_RADIUS_DP * dpiScale;

    // ═════ 异步下载封面图到内存（无磁盘 I/O） ═════
    // URL 变更时启动后台线程下载到 std::vector<uint8_t>，通过 atomic swap 交给渲染线程。
    // 使用代际计数器 coverDownloadGen_ 解决切歌时旧下载未完成导致新封面被跳过的竞态：
    // URL 变化时 gen++，下载线程完成后比对 gen——不匹配则丢弃过期结果。
    if (!url.empty() && url != cachedCoverUrl_) {
        cachedCoverUrl_ = url;
        d2dCoverBitmap_.Reset();       // 立即清除旧位图，切歌瞬间显示兜底符号
        coverFadingIn_ = false;        // 重置 fade-in 状态，下次封面到位时重新触发
        coverFadeAlpha_ = 1.0f;
        {
            std::lock_guard<std::mutex> lock(pendingCoverMutex_);
            pendingCoverData_.reset();
        }
        coverLoadInProgress_.store(true, std::memory_order_relaxed);

        int gen = ++coverDownloadGen_;  // 递增代际，使可能还在跑的旧下载失效
        std::string targetUrl = url;
        std::thread([this, targetUrl, gen]() {
            // 下载到临时文件，然后读入内存立即删除（避免磁盘持久化）
            wchar_t tempPath[MAX_PATH] = {0};
            ::GetTempPathW(MAX_PATH, tempPath);
            wchar_t tempFile[MAX_PATH] = {0};
            ::GetTempFileNameW(tempPath, L"mkl_", 0, tempFile);

            std::wstring wUrl(targetUrl.begin(), targetUrl.end());
            HRESULT hr = ::URLDownloadToFileW(nullptr, wUrl.c_str(), tempFile, 0, nullptr);

            // 代际校验：若期间又切歌（gen != coverDownloadGen_），丢弃过期下载结果
            int curGen = coverDownloadGen_.load(std::memory_order_relaxed);
            if (gen != curGen) {
                ::DeleteFileW(tempFile);
                coverLoadInProgress_.store(false, std::memory_order_release);
                if (debugLog_) Log("[COVER] Discard stale download (gen=%d, cur=%d)\n", gen, curGen);
                return;
            }

            if (SUCCEEDED(hr)) {
                // 读入内存
                HANDLE hFile = ::CreateFileW(tempFile, GENERIC_READ, FILE_SHARE_READ,
                                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD fileSize = ::GetFileSize(hFile, nullptr);
                    if (fileSize > 0 && fileSize < 8 * 1024 * 1024) {  // 拒绝 > 8MB 的图片
                        auto* data = new std::vector<uint8_t>(fileSize);
                        DWORD bytesRead = 0;
                        if (::ReadFile(hFile, data->data(), fileSize, &bytesRead, nullptr) && bytesRead == fileSize) {
                            {
                                std::lock_guard<std::mutex> lock(pendingCoverMutex_);
                                pendingCoverData_ = std::shared_ptr<std::vector<uint8_t>>(data);
                            }
                        } else {
                            delete data;
                        }
                    }
                    ::CloseHandle(hFile);
                }
                ::DeleteFileW(tempFile);
            } else {
                ::DeleteFileW(tempFile);
            }

            coverLoadInProgress_.store(false, std::memory_order_release);
            if (debugLog_) Log("[COVER] Download %s, url='%.60s'\n",
                SUCCEEDED(hr) ? "OK" : "FAIL", targetUrl.c_str());
        }).detach();
    }

    // ═════ 消费后台下载结果：从内存直接解码 ═════
    std::shared_ptr<std::vector<uint8_t>> data;
    {
        std::lock_guard<std::mutex> lock(pendingCoverMutex_);
        data.swap(pendingCoverData_);
    }
    if (data) {

        // 通过 IWICStream::InitializeFromMemory 直接从内存解码，消除磁盘 I/O
        Microsoft::WRL::ComPtr<IWICStream> stream;
        HRESULT hr = wicFactory_->CreateStream(stream.GetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = stream->InitializeFromMemory(data->data(), static_cast<DWORD>(data->size()));
        }

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(hr)) {
            hr = wicFactory_->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                decoder.GetAddressOf());
        }

        if (SUCCEEDED(hr)) {
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            hr = decoder->GetFrame(0, frame.GetAddressOf());
            if (SUCCEEDED(hr)) {
                Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
                hr = wicFactory_->CreateBitmapScaler(scaler.GetAddressOf());
                if (SUCCEEDED(hr)) {
                    // 获取原始图片尺寸，计算等比例缩放目标尺寸
                    UINT srcW = 0, srcH = 0;
                    frame->GetSize(&srcW, &srcH);
                    UINT targetSize = static_cast<UINT>(std::ceil(size));
                    UINT scaleW = targetSize, scaleH = targetSize;
                    if (srcW > 0 && srcH > 0) {
                        double srcAspect = static_cast<double>(srcW) / static_cast<double>(srcH);
                        if (srcAspect > 1.0001) {
                            // 宽图：宽度撑满，高度按比例
                            scaleH = static_cast<UINT>(std::max(1.0, targetSize / srcAspect));
                        } else if (srcAspect < 0.9999) {
                            // 高图：高度撑满，宽度按比例
                            scaleW = static_cast<UINT>(std::max(1.0, targetSize * srcAspect));
                        }
                    }
                    hr = scaler->Initialize(frame.Get(), scaleW, scaleH,
                                            WICBitmapInterpolationModeHighQualityCubic);
                    if (SUCCEEDED(hr)) {
                        // 将缩放后的图像转换为 BGRA 像素数据
                        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                        hr = wicFactory_->CreateFormatConverter(converter.GetAddressOf());
                        if (SUCCEEDED(hr)) {
                            hr = converter->Initialize(
                                scaler.Get(),
                                GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapDitherTypeNone,
                                nullptr, 0.0,
                                WICBitmapPaletteTypeCustom);
                            if (SUCCEEDED(hr)) {
                                UINT w = 0, h = 0;
                                converter->GetSize(&w, &h);
                                const UINT stride = w * 4; // BGRA = 4 bytes/pixel
                                const UINT bufSize = stride * h;

                                std::vector<BYTE> pixels(bufSize);
                                hr = converter->CopyPixels(nullptr, stride,
                                                          static_cast<UINT>(pixels.size()),
                                                          pixels.data());
                                if (SUCCEEDED(hr)) {
                                    // 直接用像素数据创建 D2D Bitmap（与 renderTarget_ 同域）
                                    D2D1_BITMAP_PROPERTIES bp = {};
                                    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                                    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                                    // 位图 DPI 设为 96（基准值），使像素尺寸与 DIP 1:1 对应。
                                    // 因为此处的 w/h 已是按当前 DPI 缩放后的物理像素，
                                    // 绘制矩形的 size 也同样是物理像素值（render target DPI == 屏幕 DPI），
                                    // 若设为 dpi_（>96）会导致 D2D 将位图解释为比实际更小的 DIPs，
                                    // 造成 FillRoundedRectangle 时位图区域不足，边缘像素被 CLAMP 拉伸。
                                    bp.dpiX = 96.0f;
                                    bp.dpiY = 96.0f;

                                    hr = renderTarget_->CreateBitmap(
                                        D2D1::SizeU(w, h),
                                        pixels.data(),
                                        stride,
                                        &bp,
                                        d2dCoverBitmap_.GetAddressOf());

                                    // ═════ P1-①: 从封面像素提取主色调（色相桶投票） ═════
                                    // 缩略采样：每 4px 一个样本，减少 CPU 开销。
                                    // 仅对不透明的像素参与投票（alpha < 128 的透明像素跳过）。
                                    // 结果经过欠饱和处理后存入 coverThemeColor_，供卡片背景使用。
                                    {
                                        const int step = 4;
                                        struct HueBucket { float r, g, b, weight; int count; } buckets[12] = {};
                                        for (UINT py = 0; py < h; py += step) {
                                            const BYTE* row = pixels.data() + py * stride;
                                            for (UINT px = 0; px < w; px += step) {
                                                const BYTE* p = row + px * 4;
                                                if (p[3] < 128) continue;  // 跳过透明像素
                                                float rf = p[2] / 255.0f;  // B=0, G=1, R=2 (BGRA)
                                                float gf = p[1] / 255.0f;
                                                float bf = p[0] / 255.0f;
                                                float maxC = std::max({rf, gf, bf});
                                                float minC = std::min({rf, gf, bf});
                                                float delta = maxC - minC;
                                                float sat = (maxC > 0.001f) ? delta / maxC : 0.0f;
                                                float val = maxC;
                                                float weight = sat * val;
                                                if (weight < 0.05f) continue;  // 跳过近灰色像素
                                                float hue = 0.0f;
                                                if (delta > 0.001f) {
                                                    if (maxC == rf)       hue = 60.0f * fmodf((gf - bf) / delta + 6.0f, 6.0f);
                                                    else if (maxC == gf)  hue = 60.0f * ((bf - rf) / delta + 2.0f);
                                                    else                  hue = 60.0f * ((rf - gf) / delta + 4.0f);
                                                }
                                                int bin = static_cast<int>(hue / 30.0f) % 12;
                                                buckets[bin].r += rf * weight;
                                                buckets[bin].g += gf * weight;
                                                buckets[bin].b += bf * weight;
                                                buckets[bin].weight += weight;
                                                buckets[bin].count++;
                                            }
                                        }
                                        int bestBin = 0;
                                        float bestWeight = 0.0f;
                                        for (int i = 0; i < 12; ++i) {
                                            if (buckets[i].weight > bestWeight) {
                                                bestWeight = buckets[i].weight;
                                                bestBin = i;
                                            }
                                        }
                                        auto& b = buckets[bestBin];
                                        if (b.weight > 0.001f) {
                                            // 欠饱和处理：将饱和度降低 50%，避免背景过于鲜艳
                                            float avgR = std::max(0.0f, std::min(1.0f, b.r / b.weight));
                                            float avgG = std::max(0.0f, std::min(1.0f, b.g / b.weight));
                                            float avgB = std::max(0.0f, std::min(1.0f, b.b / b.weight));
                                            // 与中灰混合以降低饱和度
                                            const float gray = 0.5f;
                                            const float desat = 0.5f;
                                            coverThemeColor_ = D2D1::ColorF(
                                                avgR + (gray - avgR) * desat,
                                                avgG + (gray - avgG) * desat,
                                                avgB + (gray - avgB) * desat,
                                                1.0f);
                                        } else {
                                            coverThemeColor_ = D2D1::ColorF(0.45f, 0.45f, 0.50f, 1.0f);
                                        }
                                        if (cardBackgroundBrush_) {
                                            cardBackgroundBrush_->SetColor(coverThemeColor_);
                                        }
                                        if (debugLog_) Log("[COVER] Theme extracted: RGB(%.2f,%.2f,%.2f) bin=%d\n",
                                            coverThemeColor_.r, coverThemeColor_.g, coverThemeColor_.b, bestBin);
                                    }

                                    if (debugLog_) Log("[COVER] D2D bitmap via pixels: hr=0x%08X, bitmap=%d size=%ux%u\n",
                                        hr, d2dCoverBitmap_ ? 1 : 0, w, h);

                                    // ═════ P1-②: 触发封面 fade-in 动画 ═════
                                    // 新封面位图刚刚创建完成，启动 fade-in 过渡。
                                    coverFadingIn_ = true;
                                    coverFadeStartTime_ = static_cast<double>(GetTickCount64());
                                    coverFadeAlpha_ = 0.0f;

                                    // ═════ P1-④: 创建柔焦背景 ═════
                                    // 将封面缩放到 32×32 小尺寸 → D2D 位图画刷 → 拉伸铺满卡片
                                    // 利用双线性插值产生自然柔焦效果，无需 D2D 1.1
                                    blurredCoverBg_.Reset();
                                    blurredBgBrush_.Reset();
                                    blurredBgBitmapW_ = 0.0f;
                                    {
                                        Microsoft::WRL::ComPtr<IWICBitmapScaler> blurScaler;
                                        hr = wicFactory_->CreateBitmapScaler(blurScaler.GetAddressOf());
                                        if (SUCCEEDED(hr)) {
                                            hr = blurScaler->Initialize(
                                                frame.Get(),
                                                constants::COVER_BLUR_SAMPLE_SIZE,
                                                constants::COVER_BLUR_SAMPLE_SIZE,
                                                WICBitmapInterpolationModeHighQualityCubic);
                                        }
                                        if (SUCCEEDED(hr)) {
                                            hr = renderTarget_->CreateBitmapFromWicBitmap(
                                                blurScaler.Get(), nullptr,
                                                blurredCoverBg_.GetAddressOf());
                                        }
                                        if (SUCCEEDED(hr) && blurredCoverBg_) {
                                            blurredBgBitmapW_ = blurredCoverBg_->GetSize().width;
                                            renderTarget_->CreateBitmapBrush(
                                                blurredCoverBg_.Get(),
                                                D2D1::BitmapBrushProperties(
                                                    D2D1_EXTEND_MODE_CLAMP,
                                                    D2D1_EXTEND_MODE_CLAMP,
                                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                                D2D1::BrushProperties(
                                                    1.0f,
                                                    D2D1::Matrix3x2F::Identity()),
                                                blurredBgBrush_.GetAddressOf());
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ═════ 绘制 ═════
    // 驱动封面 fade-in 进度（每帧调用，动画完成后自动跳过）
    UpdateCoverFade();

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + size, y + size), radius, radius);

    if (d2dCoverBitmap_) {
        // 有封面位图：使用圆角矩形几何裁剪 + DrawBitmap 绘制，
        // 替代 BitmapBrush+FillRoundedRectangle 以避免画笔原点错位问题。
        // BitmapBrush 默认从 (0,0) 采样，需平移才能对齐到 (x,y)；
        // 非正方形位图还需居中，图层裁剪方案统一处理这两种情况。

        D2D1_SIZE_F bmpSize = d2dCoverBitmap_->GetSize();
        float bmpW = bmpSize.width;
        float bmpH = bmpSize.height;

        // 将位图居中于封面正方形区域内
        float drawX = x + (size - bmpW) / 2.0f;
        float drawY = y + (size - bmpH) / 2.0f;
        D2D1_RECT_F destRect = D2D1::RectF(drawX, drawY, drawX + bmpW, drawY + bmpH);

        // 缓存裁剪几何和 Layer，仅在封面尺寸变化时重建
        // 避免每帧 CreateLayer 造成 D2D 内部资源耗尽（频谱引入后 card 模式每帧重绘）
        if (!coverClipGeo_ || std::abs(cachedCoverSize_ - size) > 0.5f) {
            coverClipGeo_.Reset();
            coverLayer_.Reset();
            d2dFactory_->CreateRoundedRectangleGeometry(rr, coverClipGeo_.GetAddressOf());
            D2D1_SIZE_F layerSize = D2D1::SizeF(size, size);
            renderTarget_->CreateLayer(&layerSize, coverLayer_.GetAddressOf());
            cachedCoverSize_ = size;
        }
        if (coverClipGeo_ && coverLayer_) {
            D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
                D2D1::InfiniteRect(), coverClipGeo_.Get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(),
                1.0f, nullptr,
                D2D1_LAYER_OPTIONS_NONE);
            renderTarget_->PushLayer(layerParams, coverLayer_.Get());
            renderTarget_->DrawBitmap(
                d2dCoverBitmap_.Get(), destRect, coverFadeAlpha_,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
            renderTarget_->PopLayer();
        }
    } else {
        // 无封面：显示灰底圆角矩形 + 歌名首字 fallback
        // 同时重置主题色为默认灰蓝色，使卡片背景回归中性。
        coverThemeColor_ = D2D1::ColorF(0.45f, 0.45f, 0.50f, 1.0f);
        if (cardBackgroundBrush_) {
            cardBackgroundBrush_->SetColor(coverThemeColor_);
        }
        // 无封面时清除柔焦背景，回落纯色
        blurredCoverBg_.Reset();
        blurredBgBrush_.Reset();
        blurredBgBitmapW_ = 0.0f;
        // 无封面时重置 fade-in 状态
        coverFadingIn_ = false;
        coverFadeAlpha_ = 1.0f;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.45f, 0.45f, 0.5f, 0.85f), bgBrush.GetAddressOf());
        if (bgBrush) {
            renderTarget_->FillRoundedRectangle(rr, bgBrush.Get());
        }

        // 绘制歌名首字符（fallbackChar），使用卡片字体 + next 颜色，高透明度弱化
        if (cardFallbackFormat_ && fallbackChar != L'\0') {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fgBrush;
            renderTarget_->CreateSolidColorBrush(
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.60f), fgBrush.GetAddressOf());
            if (fgBrush) {
                D2D1_RECT_F textRect = D2D1::RectF(x, y, x + size, y + size);
                renderTarget_->DrawTextW(&fallbackChar, 1, cardFallbackFormat_.Get(),
                                          textRect, fgBrush.Get());
            }
        }
    }
}

/// 绘制单行卡片模式歌词（动画时使用，可独立控制透明度、偏移）
/// isCurrent = true  → 使用当前行字号/颜色（大字，亮色）
/// isCurrent = false → 使用下一行字号/颜色（小字，灰色）
void TaskbarRenderer::DrawCardLyricsSingle(const std::wstring& line,
                                           float x, float y, float availWidth,
                                           float yOffset, float alpha,
                                           bool isCurrent) {
    if (!renderTarget_ || line.empty() || alpha <= 0.001f) return;

    const float h = static_cast<float>(height_);
    const float midY = h * 0.50f;

    IDWriteTextFormat* format = isCurrent ? cardCurrentFormat_.Get() : cardNextFormat_.Get();
    ID2D1SolidColorBrush* brush = isCurrent ? cardCurrentBrush_.Get() : cardNextBrush_.Get();
    if (!format || !brush) return;

    D2D1_COLOR_F origColor = brush->GetColor();
    D2D1_COLOR_F animColor = origColor;
    animColor.a = std::max(0.0f, std::min(1.0f, origColor.a * alpha));
    brush->SetColor(animColor);

    D2D1_RECT_F layout = D2D1::RectF(x, y + yOffset, x + availWidth, y + midY + yOffset);
    renderTarget_->DrawTextW(
        line.c_str(), static_cast<UINT32>(line.size()),
        format, layout, brush);

    brush->SetColor(origColor);
}

void TaskbarRenderer::DrawCardLyrics(const std::wstring& currentLine,
                                     const std::wstring& nextLine,
                                     float x, float y, float availWidth,
                                     float yOffset, float alpha,
                                     float /*curFontSizeScale*/, float /*nextFontSizeScale*/) {
    if (!renderTarget_ || alpha <= 0.001f) return;

    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float vNudgeCur = 4.0f * dpiScale;   // 主歌词下移 4dp
    const float vNudgeNext = 2.0f * dpiScale;  // 副歌词上移 2dp
    const float halfH = static_cast<float>(height_) * 0.50f;

    if (!currentLine.empty()) {
        DrawCardLyricsSingle(currentLine, x, y + vNudgeCur, availWidth, yOffset, alpha,
                             /*isCurrent=*/true);
    }

    if (!nextLine.empty()) {
        DrawCardLyricsSingle(nextLine, x, y + halfH - vNudgeNext, availWidth, yOffset, alpha,
                             /*isCurrent=*/false);
    }
}

void TaskbarRenderer::PresentToLayeredWindow() {
    if (!wicBitmap_ || !hwnd_) return;

    // 检查窗口是否可见
    if (!::IsWindowVisible(hwnd_)) {
        return;
    }

    WICRect rcLock = { 0, 0, static_cast<INT>(width_), static_cast<INT>(height_) };
    Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
    HRESULT hr = wicBitmap_->Lock(&rcLock, WICBitmapLockRead, lock.GetAddressOf());
    if (FAILED(hr)) {
        Log("[PRESENT] Lock failed: 0x%08X\n", hr);
        return;
    }

    UINT cbStride = 0, cbSize = 0;
    BYTE* pData = nullptr;
    hr = lock->GetStride(&cbStride);
    if (FAILED(hr)) return;
    hr = lock->GetDataPointer(&cbSize, &pData);
    if (FAILED(hr) || !pData) return;

    HDC hdcScreen = ::GetDC(nullptr);
    HDC hdcMem = ::CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(width_);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(height_);
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    VOID* pBits = nullptr;
    HBITMAP hBmp = ::CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (hBmp && pBits) {
        const UINT totalBytes = width_ * height_ * 4;
        memcpy(pBits, pData, std::min<UINT>(totalBytes, cbSize));

        HBITMAP hOld = static_cast<HBITMAP>(::SelectObject(hdcMem, hBmp));

        POINT ptSrc = { 0, 0 };
        SIZE sz = { static_cast<LONG>(width_), static_cast<LONG>(height_) };
        POINT ptDst;
        RECT winRect{};
        ::GetWindowRect(hwnd_, &winRect);
        ptDst.x = winRect.left;
        ptDst.y = winRect.top;

        BLENDFUNCTION bf = {};
        bf.BlendOp             = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat         = AC_SRC_ALPHA;

        BOOL result = ::UpdateLayeredWindow(
            hwnd_, hdcScreen, &ptDst, &sz,
            hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

        ::SelectObject(hdcMem, hOld);
        ::DeleteObject(hBmp);
    }

    ::DeleteDC(hdcMem);
    ::ReleaseDC(nullptr, hdcScreen);
}

// 前置声明：P3 动画时间戳获取（匿名命名空间，定义在文件尾部跑马灯区域）
namespace { double GetCurrentTimeSeconds(); }

void TaskbarRenderer::Render(const RenderState& state) {
    if (!initialized_ || !renderTarget_) return;

    static int renderEntryLogCount = 0;
    if (++renderEntryLogCount <= 5) {
        Log("[RENDER] entry #%d: displayMode='%s' hasLyrics=%d isPlaying=%d curLine='%s' coverUrl='%s'\n",
            renderEntryLogCount,
            settings_.displayMode.c_str(),
            state.hasLyrics, state.isPlaying,
            state.currentLine.empty() ? "(empty)" : state.currentLine.substr(0, 20).c_str(),
            state.coverArtUrl.empty() ? "(empty)" : state.coverArtUrl.substr(0, 40).c_str());
    }

    // 跑马灯状态机更新（仅 karaoke 模式）
    bool marqueeNeedsRedraw = false;
    float scrollOffset = 0.0f;
    if (settings_.displayMode != "card") {
        scrollOffset = UpdateMarquee(state.currentLine, static_cast<float>(state.progress), marqueeNeedsRedraw);
    }

    // 卡片模式歌词切换动画更新
    bool cardScrollNeedsRedraw = false;
    if (settings_.displayMode == "card") {
        cardScrollNeedsRedraw = UpdateCardAnim(state.currentLine, state.nextLine);
    }

    // P3: 歌词切换动画 + 进度弹簧（仅 karaoke 模式）
    bool kP3NeedsRedraw = false;
    // 先获取当前时间供后续弹簧步进重用（避免多次 QPC 调用）
    const double now = GetCurrentTimeSeconds();
    double smoothProgress = state.progress; // 默认值：弹簧未启动时直接用原始进度
    if (settings_.displayMode != "card" && state.hasLyrics && !state.currentLine.empty()) {
        const std::wstring lineW = Utf8ToWide(state.currentLine);
        if (UpdateLyricFade(lineW)) {
            kP3NeedsRedraw = true;
        }
        if (UpdateProgressSpring(state.progress, now)) {
            kP3NeedsRedraw = true;
        }
        smoothProgress = springProgress_;
    }

    bool stateChanged = (state.hasLyrics != lastState_.hasLyrics ||
                         state.currentLine != lastState_.currentLine ||
                         state.currentTranslated != lastState_.currentTranslated ||
                         state.isPlaying != lastState_.isPlaying ||
                         state.isHovering != lastState_.isHovering ||
                         state.isDragging != lastState_.isDragging ||
                         state.nextLine != lastState_.nextLine ||
                         state.coverArtUrl != lastState_.coverArtUrl ||
                         state.spectrumBands != lastState_.spectrumBands ||
                         std::abs(state.progress - lastState_.progress) > 0.001);
    // 跑马灯滚动动画期间也需要重绘
    // 卡片模式无跑马灯：无封面图时每帧重绘（确保 fallback 始终可见），
    // 有封面图后按需重绘（state 变化时才更新）
    const bool needCardRedraw = (settings_.displayMode == "card" && !d2dCoverBitmap_);
    if (!stateChanged && !marqueeNeedsRedraw && !needCardRedraw && !cardScrollNeedsRedraw && !kP3NeedsRedraw) {
        return;
    }

    // P3-①: 歌词行切换 fade 启动后的首帧 —— 缓存旧行翻译（此时 lastState_ 尚未更新，仍保留上一帧的值）
    if (lyricFadeActive_ && lyricFadeOldTrans_.empty()) {
        lyricFadeOldTrans_ = Utf8ToWide(lastState_.currentTranslated);
    }

    lastState_ = state;

    RECT rc{};
    ::GetWindowRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>(std::max<LONG>(rc.right - rc.left, 1));
    const UINT h = static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 1));
    if (w != width_ || h != height_) {
        Resize(w, h, dpi_);
    }

    renderTarget_->BeginDraw();

    // 悬停时填充极低 alpha 背景，使整个窗口区域可接收鼠标消息
    // alpha ≈ 1/255 肉眼不可见，但 Windows 不会将鼠标消息穿透
    if (state.isDragging) {
        // 拖动时显示可见边框，让用户看清窗口范围
        renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0.15f));
    } else if (state.isHovering) {
        renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0.004f));
    } else {
        renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0.0f));
    }

    if (settings_.displayMode == "card") {
        // ═════ 卡片样式渲染路径 ═════
        if (state.hasLyrics && !state.currentLine.empty()) {
            if (isVerticalTaskbar_) {
                RenderCardStyleVertical(state);
            } else {
                RenderCardStyle(state);
            }
        } else if (state.isPlaying) {
            // 纯音乐：封面 + 频谱
            const float dpiScale = static_cast<float>(dpi_) / 96.0f;
            const float coverSize = static_cast<float>(settings_.cardCoverSize) * dpiScale;
            const float gap = static_cast<float>(settings_.cardGap) * dpiScale;
            const float paddingX = constants::TEXT_PADDING_X;
            wchar_t fallback = FirstUtf8CharAsWide(state.songName);
            DrawCoverArt(state.coverArtUrl, fallback, paddingX,
                         (static_cast<float>(height_) - coverSize) / 2.0f, coverSize);
            if (!state.spectrumBands.empty()) {
                const float specX = paddingX + coverSize + gap;
                const float specW = static_cast<float>(width_) - specX - paddingX;
                if (specW > 10.0f) {
                    const float specH = constants::SPECTRUM_CARD_HEIGHT_DP * dpiScale;
                    const float specY = (static_cast<float>(height_) - specH) * 0.5f;
                    DrawSpectrumBars(state.spectrumBands, specX, specW, specY, specH);
                }
            }
        }
    } else {
        // ═════ 卡拉OK渲染路径 ═════
        // 垂直任务栏时减小内边距以适应窄窗口
        const float vertPaddingX = isVerticalTaskbar_
            ? constants::TEXT_PADDING_X * 0.4f
            : constants::TEXT_PADDING_X;

        if (state.hasLyrics && !state.currentLine.empty()) {
            const std::wstring lineW = Utf8ToWide(state.currentLine);
            const float* padPtr = isVerticalTaskbar_ ? &vertPaddingX : nullptr;

            // P3-①: 歌词行切换 fade 过渡 —— 旧行淡出 + 新行淡入，EaseOut 交叉
            if (lyricFadeActive_) {
                const double elapsed = now - lyricFadeStartTime_;
                const double dur = static_cast<double>(constants::LYRIC_FADE_DURATION_MS) / 1000.0;
                double rawT = elapsed / dur;
                if (rawT >= 1.0) rawT = 1.0;
                const float fadeT = EaseOutCubic(static_cast<float>(rawT));

                // 旧行：progress=1.0（已完成），无卡拉OK、无跑马灯，渐隐
                DrawHighlightedTextPerCharacter(lyricFadeOldText_, 1.0, false, 0.0f,
                                               padPtr, 1.0f - fadeT);
                // 旧行翻译（同步渐隐）
                if (settings_.enableTranslation && !lyricFadeOldTrans_.empty()) {
                    DrawTranslatedText(lyricFadeOldTrans_, padPtr, 1.0f - fadeT);
                }

                // 新行：弹簧平滑进度 + 卡拉OK + 跑马灯，渐显
                DrawHighlightedTextPerCharacter(lineW, smoothProgress, settings_.enableKaraoke,
                                               scrollOffset, padPtr, fadeT);
                if (settings_.enableTranslation && !state.currentTranslated.empty()) {
                    const std::wstring trW = Utf8ToWide(state.currentTranslated);
                    DrawTranslatedText(trW, padPtr, fadeT);
                }
            } else {
                // 非 fade：正常渲染（使用弹簧平滑进度）
                DrawHighlightedTextPerCharacter(lineW, smoothProgress, settings_.enableKaraoke,
                                               scrollOffset, padPtr);

                if (settings_.enableTranslation && !state.currentTranslated.empty()) {
                    const std::wstring trW = Utf8ToWide(state.currentTranslated);
                    DrawTranslatedText(trW, padPtr);
                }
            }
        } else {
            if (state.isPlaying) {
                if (!state.spectrumBands.empty()) {
                    // 全高频谱
                    DrawSpectrumBars(state.spectrumBands,
                                     constants::TEXT_PADDING_X,
                                     static_cast<float>(width_) - 2.0f * constants::TEXT_PADDING_X,
                                     0.0f, static_cast<float>(height_));
                } else {
                    DrawCentered(L"...", normalBrush_.Get(), 0.0f);
                }
            }
        }
    }

    // 鼠标悬停时绘制控制按钮
    if (state.isHovering) {
        DrawHoverControls(state.isPlaying);
    }

    // 拖动时绘制可见边框
    if (state.isDragging) {
        D2D1_RECT_F borderRect = D2D1::RectF(0, 0, static_cast<FLOAT>(width_), static_cast<FLOAT>(height_));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dragBorderBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.3f, 0.6f, 1.0f, 0.6f),
            dragBorderBrush.GetAddressOf());
        if (dragBorderBrush) {
            renderTarget_->DrawRectangle(borderRect, dragBorderBrush.Get(), 2.0f);
        }
    }

    HRESULT hr = renderTarget_->EndDraw();

    if (SUCCEEDED(hr)) {
        PresentToLayeredWindow();
    } else if (hr == D2DERR_RECREATE_TARGET) {
        CreateRenderTarget();
    }
}

// ═══════════════════════════════════════════
// 跑马灯状态机实现
// ═══════════════════════════════════════════

namespace {

/// 获取高精度当前时间（秒，基于 QueryPerformanceCounter）
double GetCurrentTimeSeconds() {
    LARGE_INTEGER freq, counter;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
}

} // namespace

TaskbarRenderer::MarqueeMode TaskbarRenderer::ParseMarqueeMode(const std::string& mode) {
    if (mode == "loop" || mode == "Loop") return MarqueeMode::Loop;
    if (mode == "off" || mode == "Off")   return MarqueeMode::Off;
    return MarqueeMode::Bounce;  // default
}

float TaskbarRenderer::UpdateMarquee(const std::string& lyricText, float progress, bool& needRedraw) {
    needRedraw = false;

    const MarqueeMode mode = ParseMarqueeMode(settings_.marqueeMode);

    // 跑马灯关闭 → 始终不滚动
    if (!settings_.enableMarquee || mode == MarqueeMode::Off) {
        if (marqueeState_ != MarqueeState::Idle) {
            marqueeState_ = MarqueeState::Idle;
            scrollOffset_ = 0.0f;
            needRedraw = true;
        }
        return 0.0f;
    }

    const float paddingX = constants::TEXT_PADDING_X;
    const float availableWidth = static_cast<FLOAT>(width_) - paddingX * 2.0f;

    // 检测歌词文本变化 → 重置状态机
    if (lyricText != marqueeLastText_) {
        marqueeLastText_ = lyricText;
        scrollOffset_ = 0.0f;
        marqueeProgress_ = 0.0f;

        // 测量文本宽度
        marqueeTextWidth_ = 0.0f;
        marqueeMaxOffset_ = 0.0f;
        if (!lyricText.empty() && dwriteFactory_ && textFormat_) {
            const std::wstring wText = Utf8ToWide(lyricText);
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    wText.c_str(), static_cast<UINT32>(wText.size()),
                    textFormat_.Get(), availableWidth, static_cast<FLOAT>(height_),
                    layout.GetAddressOf()))) {
                DWRITE_TEXT_METRICS m{};
                if (SUCCEEDED(layout->GetMetrics(&m))) {
                    marqueeTextWidth_ = m.width;
                }
            }
        }

        // 判断是否需要滚动：文本宽度 > 可用宽度
        if (marqueeTextWidth_ > availableWidth + 1.0f) {
            marqueeMaxOffset_ = marqueeTextWidth_ - availableWidth;
            // 长歌词直接进入滚动状态（跟随高亮进度），不需要先 Delay 等待。
            // Delay 仅用于 bounce 模式的回位后循环。
            marqueeState_ = MarqueeState::ScrollLeft;
        } else {
            // 短文本不需要滚动
            marqueeState_ = MarqueeState::Idle;
            marqueeMaxOffset_ = 0.0f;
        }

        stateStartTime_ = GetCurrentTimeSeconds();
        needRedraw = true;
        return 0.0f;
    }

    // 空文本或无需滚动
    if (marqueeState_ == MarqueeState::Idle) {
        return 0.0f;
    }

    // 记录当前高亮进度（用于控制回位时机）
    marqueeProgress_ = progress;

    const double now = GetCurrentTimeSeconds();
    const double elapsed = now - stateStartTime_;

    // 计算有效滚动速度（超长歌词自动加速）
    float speed = settings_.marqueeSpeedPxPerSec;
    if (marqueeTextWidth_ > availableWidth * constants::MARQUEE_SPEEDUP_THRESHOLD) {
        // 超出越多越快，最高 3 倍速
        const float ratio = marqueeTextWidth_ / availableWidth;
        speed *= std::min(ratio / constants::MARQUEE_SPEEDUP_THRESHOLD, 3.0f);
    }

    switch (marqueeState_) {
    case MarqueeState::Idle:
        return 0.0f;

    case MarqueeState::Delay:
        // 等待 delayMs 后开始向左滚动
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueeDelayMs)) {
            marqueeState_ = MarqueeState::ScrollLeft;
            stateStartTime_ = now;
            scrollOffset_ = 0.0f;
        }
        return 0.0f;

    case MarqueeState::ScrollLeft: {
        // 基于高亮进度计算目标滚动位置，然后以恒定速度平滑逼近。
        // 这样滚动速度始终为配置的 marqueeSpeedPxPerSec，不会出现先快后慢的突变。
        const float progressClamped = std::clamp(progress, 0.0f, 1.0f);
        const float targetOffset = progressClamped * marqueeMaxOffset_;

        const float maxStep = static_cast<float>(elapsed) * speed;
        if (scrollOffset_ < targetOffset) {
            scrollOffset_ = std::min(scrollOffset_ + maxStep, targetOffset);
        } else if (scrollOffset_ > targetOffset) {
            // 进度回退时（罕见），也平滑跟回
            scrollOffset_ = std::max(scrollOffset_ - maxStep, targetOffset);
        }
        needRedraw = true;

        // 只有同时满足以下两个条件才触发回位序列：
        // 1. 已滚动到最大偏移量（整句歌词末端已可见）
        // 2. 高亮进度已完成（progress >= 1.0）
        if (scrollOffset_ >= marqueeMaxOffset_ && progress >= 1.0f) {
            scrollOffset_ = marqueeMaxOffset_;
            if (mode == MarqueeMode::Bounce) {
                marqueeState_ = MarqueeState::PauseRight;
                stateStartTime_ = now;
            } else {
                // Loop 模式：立即回到 Delay 重新开始
                marqueeState_ = MarqueeState::Delay;
                stateStartTime_ = now;
                scrollOffset_ = 0.0f;
            }
        }
        return scrollOffset_;
    }

    case MarqueeState::PauseRight:
        // 右端点暂停 pauseMs
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueePauseMs)) {
            marqueeState_ = MarqueeState::ScrollRight;
            stateStartTime_ = now;
        }
        return marqueeMaxOffset_;

    case MarqueeState::ScrollRight: {
        const float distance = static_cast<float>(elapsed) * speed;
        scrollOffset_ = marqueeMaxOffset_ - std::min(distance, marqueeMaxOffset_);
        needRedraw = true;

        if (scrollOffset_ <= 0.0f) {
            scrollOffset_ = 0.0f;
            marqueeState_ = MarqueeState::PauseLeft;
            stateStartTime_ = now;
        }
        return scrollOffset_;
    }

    case MarqueeState::PauseLeft:
        // 左端点暂停 pauseMs 后回到 Delay
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueePauseMs)) {
            marqueeState_ = MarqueeState::Delay;
            stateStartTime_ = now;
        }
        return 0.0f;
    }

    return 0.0f;
}

// ═════════════════════════════════════════
// 卡片模式歌词切换动画（淡入淡出 + 位移）
// ═════════════════════════════════════════

float TaskbarRenderer::EaseOutCubic(float t) {
    // ease-out cubic: f(t) = 1 - (1-t)^3
    float c = std::clamp(t, 0.0f, 1.0f);
    float inv = 1.0f - c;
    return 1.0f - inv * inv * inv;
}

float TaskbarRenderer::EaseInOutQuad(float t) {
    // ease-in-out quad: t<0.5 → 2*t^2, 否则 1-(2-2t)^2/2
    float c = std::clamp(t, 0.0f, 1.0f);
    if (c < 0.5f) {
        return 2.0f * c * c;
    }
    float inv = 1.0f - c;
    return 1.0f - 2.0f * inv * inv;
}

float TaskbarRenderer::EaseOutBack(float t) {
    // ease-out back：末端轻微"冲出"然后回弹
    // f(t) = 1 + c3*(t-1)^3 + c2*(t-1)^2, c1=1.70158, c2=c1+1, c3=c1+1
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float c = std::clamp(t, 0.0f, 1.0f);
    float v = c - 1.0f;
    return 1.0f + c3 * v * v * v + c1 * v * v;
}

bool TaskbarRenderer::UpdateCardAnim(const std::string& currentLine,
                                     const std::string& nextLine) {
    // 动画持续时间：350ms（快速过渡，减少新旧歌词重叠）
    constexpr double kDuration = 0.35;

    bool lineChanged = (currentLine != cardLastCurrentLine_);
    if (lineChanged && !currentLine.empty() && !cardLastCurrentLine_.empty()) {
        cardPrevCurrentLine_ = cardLastCurrentLine_;
        cardPrevNextLine_ = cardLastNextLine_;
        cardAnimState_ = CardAnimState::Animating;
        cardAnimProgress_ = 0.0f;

        LARGE_INTEGER li, freq;
        ::QueryPerformanceCounter(&li);
        ::QueryPerformanceFrequency(&freq);
        cardAnimStartTime_ = static_cast<double>(li.QuadPart) / static_cast<double>(freq.QuadPart);
    }

    cardLastCurrentLine_ = currentLine;
    cardLastNextLine_ = nextLine;

    if (cardAnimState_ == CardAnimState::Idle) {
        return false;
    }

    LARGE_INTEGER li, freq;
    ::QueryPerformanceCounter(&li);
    ::QueryPerformanceFrequency(&freq);
    double now = static_cast<double>(li.QuadPart) / static_cast<double>(freq.QuadPart);
    double elapsed = now - cardAnimStartTime_;
    cardAnimProgress_ = static_cast<float>(std::min(elapsed / kDuration, 1.0));

    if (cardAnimProgress_ >= 1.0f) {
        cardAnimState_ = CardAnimState::Idle;
        cardAnimProgress_ = 0.0f;
        return false;
    }

    return true;
}

// ═════ P3-①: 歌词行切换 fade 动画 ═════
// 检测歌词行变化 → 缓存旧行文本 → 启动 200ms EaseOut 交叉淡化。
// 返回 true 表示动画进行中，调用方据此触发持续重绘。
bool TaskbarRenderer::UpdateLyricFade(const std::wstring& newText)
{
    // 首帧：新行文本入库，不做 fade
    static std::wstring lastText;
    if (lastText.empty()) {
        lastText = newText;
        return false;
    }

    // 歌词行发生变化且新旧均非空 → 启动 fade
    const bool lineChanged = (newText != lastText);
    if (lineChanged && !newText.empty() && !lastText.empty()) {
        lyricFadeOldText_ = lastText;

        LARGE_INTEGER li, freq;
        ::QueryPerformanceCounter(&li);
        ::QueryPerformanceFrequency(&freq);
        lyricFadeStartTime_ = static_cast<double>(li.QuadPart) / static_cast<double>(freq.QuadPart);
        lyricFadeActive_ = true;
    }
    lastText = newText;

    if (!lyricFadeActive_) {
        return false;
    }

    // 检查动画是否到期
    LARGE_INTEGER li2, freq2;
    ::QueryPerformanceCounter(&li2);
    ::QueryPerformanceFrequency(&freq2);
    const double now = static_cast<double>(li2.QuadPart) / static_cast<double>(freq2.QuadPart);
    const double elapsed = now - lyricFadeStartTime_;
    const double dur = static_cast<double>(constants::LYRIC_FADE_DURATION_MS) / 1000.0;

    if (elapsed >= dur) {
        lyricFadeActive_ = false;
        lyricFadeOldText_.clear();
        lyricFadeOldTrans_.clear();
        return false;
    }

    return true;
}

// ═════ P3-②: 卡拉OK进度弹簧 ═════
// 使用弹簧-阻尼物理模型将显示进度向目标平滑收敛。
// stiffness=120（刚度）、damping=14（接近临界阻尼），约 50ms 内完成主要运动。
// 返回 true 表示弹簧仍在运动中，需持续重绘。
bool TaskbarRenderer::UpdateProgressSpring(double target, double now)
{
    // 首次调用初始化
    if (springLastTime_ == 0.0) {
        springProgress_ = target;
        springVelocity_ = 0.0;
        springLastTime_ = now;
        return false;
    }

    const double dt = now - springLastTime_;
    springLastTime_ = now;

    // 时间步长钳位：避免暂停/调试期间超大 dt 导致弹簧爆炸
    const double clampedDt = (dt > 0.1) ? 0.1 : ((dt <= 0.0) ? 1.0 / 60.0 : dt);

    // 弹簧-阻尼物理：F = -k*(x-target) - c*v
    const double k = constants::KARAOKE_PROGRESS_SPRING_STIFFNESS;
    const double c = constants::KARAOKE_PROGRESS_SPRING_DAMPING;
    const double dx = springProgress_ - target;
    const double acceleration = -k * dx - c * springVelocity_;

    // 半隐式欧拉积分（先更新速度再更新位置，数值更稳定）
    springVelocity_ += acceleration * clampedDt;
    springProgress_ += springVelocity_ * clampedDt;

    // 收敛判定：位移和速度均足够小时视为静止，吸附到目标值
    const bool converged = (std::abs(springProgress_ - target) < 0.0005 &&
                            std::abs(springVelocity_) < 0.001);
    if (converged) {
        springProgress_ = target;
        springVelocity_ = 0.0;
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════
// 频谱渲染实现
// ═══════════════════════════════════════════

void TaskbarRenderer::DrawSpectrumBars(const std::vector<float>& bands, float x, float width, float y, float height, float alpha) {
    if (bands.empty() || !renderTarget_) return;

    const size_t n = bands.size();
    const float totalGap = constants::SPECTRUM_BAR_GAP * (static_cast<float>(n) - 1);
    const float availW = width;
    const float barWidth = (std::max)(2.0f, (availW - totalGap) / static_cast<float>(n));
    const float step = barWidth + constants::SPECTRUM_BAR_GAP;
    const float startX = x;

    for (size_t i = 0; i < n; ++i) {
        float barH = bands[i] * height;
        if (barH < constants::SPECTRUM_BAR_MIN_HEIGHT) barH = constants::SPECTRUM_BAR_MIN_HEIGHT;
        const float barX = startX + static_cast<float>(i) * step;
        const float barY = y + height - barH;

        D2D1_RECT_F rect = D2D1::RectF(barX, barY, barX + barWidth, barY + barH);
        spectrumBrush_->SetOpacity(alpha * (0.12f + bands[i] * 0.88f));
        renderTarget_->FillRectangle(rect, spectrumBrush_.Get());
    }
}

// ═════ P1-②: 封面 fade-in 过渡动画 ═════
// 使用 ease-out cubic 缓动：1 - (1 - t)^3。
// 350ms 内 coverFadeAlpha_ 从 0→1，后续帧跳过不计算。
// 返回 true 表示动画进行中（调用方可据此触发额外重绘）。
bool TaskbarRenderer::UpdateCoverFade()
{
    if (!coverFadingIn_) return false;

    const double now = static_cast<double>(GetTickCount64());
    const double elapsed = now - coverFadeStartTime_;
    const double dur = static_cast<double>(constants::COVER_FADE_DURATION_MS);

    if (elapsed >= dur) {
        // 动画完成
        coverFadeAlpha_ = 1.0f;
        coverFadingIn_ = false;
        return false;
    }

    // ease-out cubic
    const double t = elapsed / dur;
    const double eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    coverFadeAlpha_ = static_cast<float>(eased);
    return true;
}

} // namespace moekoe
