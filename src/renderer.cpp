// SPDX-License-Identifier: GPL-3.0
// renderer.cpp - Direct2D + DirectWrite 渲染核心实现
// 窗口管理、D2D 资源生命周期、主循环
#include "renderer.h"
#include "renderer_utils.h"
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
using renderer_utils::Utf8ToWide;
using renderer_utils::FirstUtf8CharAsWide;
using renderer_utils::GetCurrentTimeSeconds;

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
        // 排空无锁队列中可能残留的封面数据
        std::vector<uint8_t> stale;
        while (pendingCoverQueue_.try_dequeue(stale)) { }
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

} // namespace moekoe
