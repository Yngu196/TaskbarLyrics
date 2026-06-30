// SPDX-License-Identifier: GPL-3.0
// card_renderer.cpp - 卡片模式渲染（专辑封面 + 双行歌词布局）
#include "renderer.h"
#include "renderer_utils.h"
#include "constants.h"
#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <thread>

namespace moekoe {
using renderer_utils::Utf8ToWide;
using renderer_utils::FirstUtf8CharAsWide;
using renderer_utils::GetCurrentTimeSeconds;

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
            // 排空无锁队列中可能残留的旧封面数据
            std::vector<uint8_t> stale;
            while (pendingCoverQueue_.try_dequeue(stale)) { }
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
                        std::vector<uint8_t> data(fileSize);
                        DWORD bytesRead = 0;
                        if (::ReadFile(hFile, data.data(), fileSize, &bytesRead, nullptr) && bytesRead == fileSize) {
                            // 通过无锁队列将封面数据发送给渲染线程（move 语义，零拷贝）
                            pendingCoverQueue_.enqueue(std::move(data));
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
    std::vector<uint8_t> data;
    pendingCoverQueue_.try_dequeue(data);
    if (!data.empty()) {

        // 通过 IWICStream::InitializeFromMemory 直接从内存解码，消除磁盘 I/O
        Microsoft::WRL::ComPtr<IWICStream> stream;
        HRESULT hr = wicFactory_->CreateStream(stream.GetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = stream->InitializeFromMemory(data.data(), static_cast<DWORD>(data.size()));
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
