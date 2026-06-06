// SPDX-License-Identifier: GPL-2.0
// renderer.cpp - Direct2D + DirectWrite 渲染实现
// 完全透明背景: WIC + UpdateLayeredWindow + 逐字高亮
#include "renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

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

} // namespace

TaskbarRenderer::TaskbarRenderer() = default;

TaskbarRenderer::~TaskbarRenderer() {
    Shutdown();
}

D2D1_COLOR_F TaskbarRenderer::ParseColor(const std::string& hex, float alpha) {
    if (hex.size() == 7 && hex[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        if (std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
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
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        dwriteFactory_->CreateTextFormat(
            family.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            std::max<FLOAT>(8.0f, static_cast<FLOAT>(settings_.fontSize) - 3.0f),
            L"zh-CN",
            translationFormat_.GetAddressOf());
        if (translationFormat_) {
            translationFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            translationFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            translationFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    if (renderTarget_) {
        const D2D1_COLOR_F hi = ParseColor(settings_.highlightColor, 1.0f);
        const D2D1_COLOR_F no = ParseColor(settings_.normalColor, settings_.normalOpacity);
        renderTarget_->CreateSolidColorBrush(hi, highlightBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(no, normalBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.7f, 0.7f, 0.7f, 0.8f),
            translationBrush_.GetAddressOf());
    }

    initialized_ = (renderTarget_ != nullptr);
    return initialized_;
}

void TaskbarRenderer::CreateRenderTarget() {
    if (!d2dFactory_ || !wicFactory_) return;

    wicBitmap_.Reset();
    renderTarget_.Reset();

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

void TaskbarRenderer::ApplySettings(const RendererSettings& s) {
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
    translationBrush_.Reset();
    highlightBrush_.Reset();
    normalBrush_.Reset();
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
        const D2D1_COLOR_F hi = ParseColor(settings_.highlightColor, 1.0f);
        const D2D1_COLOR_F no = ParseColor(settings_.normalColor, settings_.normalOpacity);
        renderTarget_->CreateSolidColorBrush(hi, highlightBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(no, normalBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.7f, 0.7f, 0.7f, 0.8f),
            translationBrush_.GetAddressOf());
    }
}

void TaskbarRenderer::DrawCentered(const std::wstring& text, ID2D1Brush* brush, float yOffset) {
    if (!renderTarget_ || !textFormat_ || !brush || text.empty()) return;
    
    // 水平偏移量（像素）
    const float paddingX = 20.0f;
    
    D2D1_RECT_F layout = D2D1::RectF(
        paddingX, yOffset,
        static_cast<FLOAT>(width_) - paddingX, yOffset + static_cast<FLOAT>(height_));
    renderTarget_->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()),
        textFormat_.Get(), layout, brush);
}

void TaskbarRenderer::DrawHighlightedTextPerCharacter(const std::wstring& text,
                                                      double progress,
                                                      bool enableKaraoke) {
    if (!renderTarget_ || !textFormat_ || text.empty() ||
        !highlightBrush_ || !normalBrush_) {
        return;
    }
    const UINT32 length = static_cast<UINT32>(text.size());
    if (length == 0) return;

    // 水平偏移量（像素）
    const float paddingX = 20.0f;

    // 先画全部普通颜色
    D2D1_RECT_F layoutRect = D2D1::RectF(
        paddingX, 0.0f, static_cast<FLOAT>(width_) - paddingX, static_cast<FLOAT>(height_));
    renderTarget_->DrawTextW(
        text.c_str(), length, textFormat_.Get(), layoutRect, normalBrush_.Get());

    if (enableKaraoke && progress > 0.0) {
        // 计算应该高亮的字数
        const size_t charsToHighlight = static_cast<size_t>(
            std::min(progress * length + 0.5, static_cast<double>(length)));

        if (charsToHighlight > 0) {
            // 使用 IDWriteTextLayout 来逐个处理字符
            Microsoft::WRL::ComPtr<IDWriteTextLayout> fullLayout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                text.c_str(), length, textFormat_.Get(),
                static_cast<FLOAT>(width_) - paddingX * 2.0f, static_cast<FLOAT>(height_),
                fullLayout.GetAddressOf()))) {

                DWRITE_TEXT_METRICS metrics{};
                fullLayout->GetMetrics(&metrics);

                // 文本左侧位置
                const FLOAT textLeft = paddingX;

                // 为了逐字高亮，我们需要知道每个字符的位置
                // 这里我们用简单的方法：先计算出截止字符的位置
                std::wstring highlightText = text.substr(0, charsToHighlight);

                Microsoft::WRL::ComPtr<IDWriteTextLayout> highlightLayout;
                if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    highlightText.c_str(), static_cast<UINT32>(highlightText.size()),
                    textFormat_.Get(),
                    static_cast<FLOAT>(width_) - paddingX * 2.0f, static_cast<FLOAT>(height_),
                    highlightLayout.GetAddressOf()))) {

                    DWRITE_TEXT_METRICS hlMetrics{};
                    highlightLayout->GetMetrics(&hlMetrics);

                    const FLOAT highlightWidth = hlMetrics.width;

                    // 用裁剪区域画出高亮的部分
                    D2D1_RECT_F clipRect = D2D1::RectF(
                        textLeft,
                        0.0f,
                        textLeft + highlightWidth,
                        static_cast<FLOAT>(height_));
                    renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    renderTarget_->DrawTextW(
                        text.c_str(), length, textFormat_.Get(), layoutRect, highlightBrush_.Get());
                    renderTarget_->PopAxisAlignedClip();
                }
            }
        }
    }
}

void TaskbarRenderer::DrawTranslatedText(const std::wstring& text) {
    if (!translationFormat_ || !translationBrush_ || text.empty()) return;
    
    // 水平偏移量（像素）
    const float paddingX = 20.0f;
    
    D2D1_RECT_F layout = D2D1::RectF(
        paddingX, static_cast<FLOAT>(height_) * 0.55f,
        static_cast<FLOAT>(width_) - paddingX, static_cast<FLOAT>(height_));
    renderTarget_->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()),
        translationFormat_.Get(),
        layout, translationBrush_.Get());
}

void TaskbarRenderer::DrawHoverControls(bool isPlaying) {
    if (!renderTarget_ || !normalBrush_) return;

    const FLOAT w = static_cast<FLOAT>(width_);
    const FLOAT h = static_cast<FLOAT>(height_);
    const FLOAT btnSize = h * 0.7f;
    const FLOAT spacing = 2.0f;
    const FLOAT totalBtnWidth = btnSize * 3.0f + spacing * 2.0f;
    const FLOAT startX = w - totalBtnWidth - 8.0f;
    const FLOAT btnY = (h - btnSize) / 2.0f;

    // 半透明背景
    D2D1_RECT_F bgRect = D2D1::RectF(
        startX - 4.0f, btnY - 2.0f,
        startX + totalBtnWidth + 4.0f, btnY + btnSize + 2.0f);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
    renderTarget_->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f),
        bgBrush.GetAddressOf());
    if (bgBrush) {
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(bgRect, 3.0f, 3.0f), bgBrush.Get());
    }

    // 按钮符号颜色
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
    renderTarget_->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f),
        iconBrush.GetAddressOf());
    if (!iconBrush) return;

    // 创建按钮文字格式（小号字体）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> btnFormat;
    if (dwriteFactory_) {
        dwriteFactory_->CreateTextFormat(
            L"Segoe UI Symbol", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            btnSize * 0.7f,
            L"en-US",
            btnFormat.GetAddressOf());
        if (btnFormat) {
            btnFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            btnFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    if (!btnFormat) return;

    // 上一首 ⏮ (U+23EE)
    D2D1_RECT_F prevRect = D2D1::RectF(startX, btnY, startX + btnSize, btnY + btnSize);
    renderTarget_->DrawTextW(L"\u23EE", 1, btnFormat.Get(), prevRect, iconBrush.Get());

    // 暂停/播放 ⏸ (U+23F8) / ▶ (U+25B6)
    FLOAT ppX = startX + btnSize + spacing;
    D2D1_RECT_F ppRect = D2D1::RectF(ppX, btnY, ppX + btnSize, btnY + btnSize);
    if (isPlaying) {
        renderTarget_->DrawTextW(L"\u23F8", 1, btnFormat.Get(), ppRect, iconBrush.Get());
    } else {
        renderTarget_->DrawTextW(L"\u25B6", 1, btnFormat.Get(), ppRect, iconBrush.Get());
    }

    // 下一首 ⏭ (U+23ED)
    FLOAT nextX = startX + (btnSize + spacing) * 2.0f;
    D2D1_RECT_F nextRect = D2D1::RectF(nextX, btnY, nextX + btnSize, btnY + btnSize);
    renderTarget_->DrawTextW(L"\u23ED", 1, btnFormat.Get(), nextRect, iconBrush.Get());
}

void TaskbarRenderer::PresentToLayeredWindow() {
    if (!wicBitmap_ || !hwnd_) return;

    WICRect rcLock = { 0, 0, static_cast<INT>(width_), static_cast<INT>(height_) };
    Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
    HRESULT hr = wicBitmap_->Lock(&rcLock, WICBitmapLockRead, lock.GetAddressOf());
    if (FAILED(hr)) return;

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

        ::UpdateLayeredWindow(
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

    bool stateChanged = (state.hasLyrics != lastState_.hasLyrics ||
                         state.currentLine != lastState_.currentLine ||
                         state.currentTranslated != lastState_.currentTranslated ||
                         state.isPlaying != lastState_.isPlaying ||
                         state.isHovering != lastState_.isHovering ||
                         std::abs(state.progress - lastState_.progress) > 0.001);
    if (!stateChanged) {
        return;
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
    if (state.isHovering) {
        renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0.004f));
    } else {
        renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0.0f));
    }

    if (state.hasLyrics && !state.currentLine.empty()) {
        const std::wstring lineW = Utf8ToWide(state.currentLine);
        DrawHighlightedTextPerCharacter(lineW, state.progress, settings_.enableKaraoke);

        if (settings_.enableTranslation && !state.currentTranslated.empty()) {
            const std::wstring trW = Utf8ToWide(state.currentTranslated);
            DrawTranslatedText(trW);
        }
    } else {
        if (state.isPlaying) {
            DrawCentered(L"...", normalBrush_.Get(), 0.0f);
        }
    }

    // 鼠标悬停时绘制控制按钮
    if (state.isHovering) {
        DrawHoverControls(state.isPlaying);
    }

    HRESULT hr = renderTarget_->EndDraw();

    if (SUCCEEDED(hr)) {
        PresentToLayeredWindow();
    } else if (hr == D2DERR_RECREATE_TARGET) {
        CreateRenderTarget();
    }
}

} // namespace moekoe
