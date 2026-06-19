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
        dwriteFactory_->CreateTextFormat(
            family.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            std::max<FLOAT>(8.0f, settings_.cardCurrentFontSize),
            L"zh-CN",
            cardCurrentFormat_.GetAddressOf());
        if (cardCurrentFormat_) {
            cardCurrentFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            cardCurrentFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            cardCurrentFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        dwriteFactory_->CreateTextFormat(
            family.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            std::max<FLOAT>(8.0f, settings_.cardNextFontSize),
            L"zh-CN",
            cardNextFormat_.GetAddressOf());
        if (cardNextFormat_) {
            cardNextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            cardNextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            cardNextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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
    cardNextBrush_.Reset();
    cardCurrentBrush_.Reset();
    d2dCoverBitmap_.Reset();
    cachedCoverUrl_.clear();       // 清除 URL 缓存，避免重建后误判无需下载
    delete pendingCoverFile_.exchange(nullptr);     // 清除待消费的临时文件
    cardNextFormat_.Reset();
    cardCurrentFormat_.Reset();
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
        cardCurrentBrush_.Reset();
        cardNextBrush_.Reset();
        const D2D1_COLOR_F hi = ParseColor(settings_.highlightColor, 1.0f);
        const D2D1_COLOR_F no = ParseColor(settings_.normalColor, settings_.normalOpacity);
        renderTarget_->CreateSolidColorBrush(hi, highlightBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(no, normalBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.7f, 0.7f, 0.7f, 0.8f),
            translationBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            ParseColor(settings_.cardCurrentColor, 1.0f),
            cardCurrentBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(
            ParseColor(settings_.cardNextColor, 1.0f),
            cardNextBrush_.GetAddressOf());
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
                                                      const float* overridePaddingX) {
    if (!renderTarget_ || !textFormat_ || text.empty() ||
        !highlightBrush_ || !normalBrush_) {
        return;
    }
    const UINT32 length = static_cast<UINT32>(text.size());
    if (length == 0) return;

    // 水平偏移量（像素）：支持垂直模式下的自定义内边距
    const float paddingX = overridePaddingX ? *overridePaddingX : constants::TEXT_PADDING_X;
    const float availableWidth = static_cast<FLOAT>(width_) - paddingX * 2.0f;

    // 布局区域（用于文本度量）
    D2D1_RECT_F layoutRect = D2D1::RectF(
        paddingX, 0.0f, static_cast<FLOAT>(width_) - paddingX, static_cast<FLOAT>(height_));

    // 获取文本度量
    Microsoft::WRL::ComPtr<IDWriteTextLayout> fullLayout;
    bool hasLayout = false;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
            text.c_str(), length, textFormat_.Get(),
            layoutRect.right - layoutRect.left, static_cast<FLOAT>(height_),
            fullLayout.GetAddressOf()))) {
        if (SUCCEEDED(fullLayout->GetMetrics(&metrics))) {
            hasLayout = true;
        }
    }

    const float textWidth = hasLayout ? metrics.width : 0.0f;

    // ── 判断是否需要跑马灯滚动 ──
    // 只要文本超宽且跑马灯状态机已激活（非Idle），就始终使用滚动模式绘制，
    // 避免 scrollOffset 从 0 跳变到 > 0 时产生 居中→左对齐 的视觉闪烁。
    const bool needsMarquee = (hasLayout && textWidth > availableWidth + 1.0f
                               && marqueeState_ != MarqueeState::Idle);

    if (needsMarquee) {
        // ═══════ 滚动模式：用 DrawTextLayout + 精确坐标 ═══════
        // 将布局改为左对齐，避免 CENTER 对齐导致的位置偏差
        fullLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // 滚动模式：文本起始位置紧贴左侧内边距（paddingX），
        // 随 scrollOffset 增大向左偏移，从而逐步露出右侧被截断的内容。
        // scrollOffset==0 时第一个字在最左侧，只有右侧被截断（符合从左到右的高亮方向）。
        const float textLeft = paddingX - scrollOffset;

        // 用 DrawTextLayout + Point2F 精确定位，完全绕过 DrawTextW 的自动对齐
        renderTarget_->DrawTextLayout(
            D2D1::Point2F(textLeft, 0.0f), fullLayout.Get(), normalBrush_.Get());

        if (enableKaraoke && progress > 0.0 && hasLayout) {
            const float highlightWidth = std::min(textWidth * static_cast<float>(progress), textWidth);
            if (highlightWidth > 0.0f) {
                // 高亮裁剪区域：从 textLeft 开始，宽度为 highlightWidth
                D2D1_RECT_F clipRect = D2D1::RectF(
                    textLeft, 0.0f,
                    textLeft + highlightWidth,
                    static_cast<FLOAT>(height_));
                renderTarget_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                renderTarget_->DrawTextLayout(
                    D2D1::Point2F(textLeft, 0.0f), fullLayout.Get(), highlightBrush_.Get());
                renderTarget_->PopAxisAlignedClip();
            }
        }
    } else {
        // ═══════ 非滚动模式：居中显示（原有逻辑）═══════
        renderTarget_->DrawTextW(
            text.c_str(), length, textFormat_.Get(), layoutRect, normalBrush_.Get());

        if (enableKaraoke && progress > 0.0 && hasLayout) {
            const float highlightWidth = std::min(textWidth * static_cast<float>(progress), textWidth);
            if (highlightWidth <= 0.0f) return;

            // 非滚动模式下，文本在 layoutRect 内居中
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

void TaskbarRenderer::DrawTranslatedText(const std::wstring& text, const float* overridePaddingX) {
    if (!translationFormat_ || !translationBrush_ || text.empty()) return;

    // 水平偏移量（像素）：支持垂直模式下的自定义内边距
    const float paddingX = overridePaddingX ? *overridePaddingX : constants::TEXT_PADDING_X;
    
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

    // ═════ 1. 绘制封面（左侧） ═════
    std::string fallback = state.songName.empty()
        ? "?"
        : state.songName.substr(0, 1);
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

    // ═════ 1. 封面（居中顶部） ═════
    std::string fallback = state.songName.empty() ? "?" : state.songName.substr(0, 1);
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

void TaskbarRenderer::DrawCoverArt(const std::string& url, const std::string& fallbackChar,
                                    float x, float y, float size) {
    if (!renderTarget_ || size <= 0.0f) return;

    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const float radius = constants::CARD_COVER_RADIUS_DP * dpiScale;

    // ═════ 异步下载封面图（仅下载，不做 D2D 操作） ═════
    // URL 变更时启动后台线程下载到临时文件（不阻塞渲染）
    // 后台线程只负责：URLDownloadToFile → 保存文件路径
    // 渲染线程负责：WIC 解码 → D2D 位图创建（必须在 renderTarget_ 同一线程）
    if (!url.empty() && url != cachedCoverUrl_ &&
        !coverLoadInProgress_.load(std::memory_order_relaxed)) {
        cachedCoverUrl_ = url;
        coverLoadInProgress_.store(true, std::memory_order_relaxed);
        d2dCoverBitmap_.Reset();       // 清除旧位图，避免显示过期封面
        delete pendingCoverFile_.exchange(nullptr);     // 清除旧的待消费文件

        std::string targetUrl = url;
        std::thread([this, targetUrl]() {
            wchar_t tempPath[MAX_PATH] = {0};
            ::GetTempPathW(MAX_PATH, tempPath);
            wchar_t tempFile[MAX_PATH] = {0};
            ::GetTempFileNameW(tempPath, L"mkl_", 0, tempFile);

            std::wstring wUrl(targetUrl.begin(), targetUrl.end());
            HRESULT hr = ::URLDownloadToFileW(nullptr, wUrl.c_str(), tempFile, 0, nullptr);

            if (SUCCEEDED(hr)) {
                // 下载成功：通过 atomic swap 将临时文件路径交给渲染线程，
                // 避免 std::string 本身的数据竞争。若存在未被消费的旧路径则一并清理。
                std::wstring ws(tempFile);
                auto* owned = new std::string(ws.begin(), ws.end());
                auto* old = pendingCoverFile_.exchange(owned, std::memory_order_release);
                delete old;
            } else {
                // 下载失败：删除空文件
                ::DeleteFileW(tempFile);
            }

            coverLoadInProgress_.store(false, std::memory_order_release);
            if (debugLog_) {
                std::wstring ws(tempFile);
                std::string tempPathUtf8(ws.begin(), ws.end());
                Log("[COVER] Download %s: %s, url='%s'\n",
                    SUCCEEDED(hr) ? "OK" : "FAIL",
                    SUCCEEDED(hr) ? tempPathUtf8.c_str() : "error",
                    targetUrl.substr(0, 60).c_str());
            }
        }).detach();
    }

    // ═════ 消费后台下载结果：在渲染线程创建 D2D 位图 ═════
    // 使用像素数据直接创建 D2D Bitmap（绕过 CreateBitmapFromWicBitmap，
    // 避免 D2DERR_WRONG_RESOURCE_DOMAIN 跨域问题）
    auto* pendingRaw = pendingCoverFile_.exchange(nullptr, std::memory_order_acquire);
    if (pendingRaw) {
        std::string pendingPath = std::move(*pendingRaw);
        delete pendingRaw;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFactory_->CreateDecoderFromFilename(
            std::wstring(pendingPath.begin(), pendingPath.end()).c_str(),
            nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
            decoder.GetAddressOf());

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
                                    if (debugLog_) Log("[COVER] D2D bitmap via pixels: hr=0x%08X, bitmap=%d size=%ux%u\n",
                                        hr, d2dCoverBitmap_ ? 1 : 0, w, h);
                                }
                            }
                        }
                    }
                }
            }
        }

        // 无论成功与否都清理临时文件（避免残留）
        if (!pendingPath.empty()) {
            ::DeleteFileW(std::wstring(pendingPath.begin(), pendingPath.end()).c_str());
        }
    }

    // ═════ 绘制 ═════
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

        // 创建圆角矩形裁剪几何
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        HRESULT geoHr = d2dFactory_->CreateRoundedRectangleGeometry(
            rr, clipGeo.GetAddressOf());
        if (SUCCEEDED(geoHr) && clipGeo) {
            Microsoft::WRL::ComPtr<ID2D1Layer> layer;
            D2D1_SIZE_F layerSize = D2D1::SizeF(size, size);
            HRESULT layerHr = renderTarget_->CreateLayer(&layerSize, layer.GetAddressOf());
            if (SUCCEEDED(layerHr) && layer) {
                D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
                    D2D1::InfiniteRect(), clipGeo.Get(),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::IdentityMatrix(),
                    1.0f, nullptr,
                    D2D1_LAYER_OPTIONS_NONE);
                renderTarget_->PushLayer(layerParams, layer.Get());
                renderTarget_->DrawBitmap(
                    d2dCoverBitmap_.Get(), destRect, 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
                renderTarget_->PopLayer();
            }
        }
    } else {
        // 无封面：显示灰底圆角矩形 + 音乐符号
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.45f, 0.45f, 0.5f, 0.85f), bgBrush.GetAddressOf());
        if (bgBrush) {
            renderTarget_->FillRoundedRectangle(rr, bgBrush.Get());
        }

        // 使用较大的字体绘制音乐符号，确保清晰可见
        if (textFormat_) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
            renderTarget_->CreateSolidColorBrush(
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), iconBrush.GetAddressOf());
            if (iconBrush) {
                D2D1_RECT_F textRect = D2D1::RectF(x, y, x + size, y + size);
                renderTarget_->DrawTextW(L"\u266A", 1, textFormat_.Get(),
                                          textRect, iconBrush.Get());
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

    bool stateChanged = (state.hasLyrics != lastState_.hasLyrics ||
                         state.currentLine != lastState_.currentLine ||
                         state.currentTranslated != lastState_.currentTranslated ||
                         state.isPlaying != lastState_.isPlaying ||
                         state.isHovering != lastState_.isHovering ||
                         state.isDragging != lastState_.isDragging ||
                         state.nextLine != lastState_.nextLine ||
                         state.coverArtUrl != lastState_.coverArtUrl ||
                         std::abs(state.progress - lastState_.progress) > 0.001);
    // 跑马灯滚动动画期间也需要重绘
    // 卡片模式无跑马灯：无封面图时每帧重绘（确保 fallback 始终可见），
    // 有封面图后按需重绘（state 变化时才更新）
    const bool needCardRedraw = (settings_.displayMode == "card" && !d2dCoverBitmap_);
    if (!stateChanged && !marqueeNeedsRedraw && !needCardRedraw && !cardScrollNeedsRedraw) {
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
            DrawCentered(L"...", normalBrush_.Get(), 0.0f);
        }
    } else {
        // ═════ 卡拉OK渲染路径 ═════
        // 垂直任务栏时减小内边距以适应窄窗口
        const float vertPaddingX = isVerticalTaskbar_
            ? constants::TEXT_PADDING_X * 0.4f
            : constants::TEXT_PADDING_X;

        if (state.hasLyrics && !state.currentLine.empty()) {
            const std::wstring lineW = Utf8ToWide(state.currentLine);
            DrawHighlightedTextPerCharacter(lineW, state.progress, settings_.enableKaraoke,
                                           scrollOffset,
                                           isVerticalTaskbar_ ? &vertPaddingX : nullptr);

            if (settings_.enableTranslation && !state.currentTranslated.empty()) {
                const std::wstring trW = Utf8ToWide(state.currentTranslated);
                DrawTranslatedText(trW, isVerticalTaskbar_ ? &vertPaddingX : nullptr);
            }
        } else {
            if (state.isPlaying) {
                DrawCentered(L"...", normalBrush_.Get(), 0.0f);
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

} // namespace moekoe
