// SPDX-License-Identifier: GPL-2.0
// renderer.h - Direct2D + DirectWrite 渲染引擎
// 完全透明背景: WIC + UpdateLayeredWindow + 逐字高亮
#pragma once

#include "config.h"
#include "lyrics_data.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <atomic>

namespace moekoe {

class TaskbarRenderer {
public:
    TaskbarRenderer();
    ~TaskbarRenderer();

    TaskbarRenderer(const TaskbarRenderer&) = delete;
    TaskbarRenderer& operator=(const TaskbarRenderer&) = delete;

    bool Initialize(HWND hwnd);
    void Shutdown();
    void ApplySettings(const AppearanceConfig& s);
    void Render(const RenderState& state);
    void Resize(UINT width, UINT height, UINT dpi);

    // 调试日志开关（由 config.debugLog 控制）
    void SetDebugLog(bool enabled) { debugLog_ = enabled; }

    // 设置/查询垂直任务栏模式（LEFT / RIGHT 方位时启用）
    void SetVerticalTaskbar(bool vertical) { isVerticalTaskbar_ = vertical; }
    bool IsVerticalTaskbar() const { return isVerticalTaskbar_; }

private:
    void CreateRenderTarget();
    void DrawHighlightedTextPerCharacter(const std::wstring& text,
                                          double progress,
                                          bool enableKaraoke,
                                          float scrollOffset = 0.0f,
                                          const float* overridePaddingX = nullptr,
                                          float opacity = 1.0f);
    void DrawTranslatedText(const std::wstring& text, const float* overridePaddingX = nullptr, float opacity = 1.0f);
    void DrawCentered(const std::wstring& text, ID2D1Brush* brush, float yOffset);
    void DrawHoverControls(bool isPlaying);
    void PresentToLayeredWindow();

    // ═════ 卡片模式渲染（无卡拉OK效果） ═════
    void RenderCardStyle(const RenderState& state);
    /// 垂直任务栏专用：堆叠式布局（封面在上，歌词在下）
    void RenderCardStyleVertical(const RenderState& state);
    void DrawCoverArt(const std::string& url, wchar_t fallbackChar,
                      float x, float y, float size);
    /// 绘制单行卡片模式歌词（isCurrent=true → 当前行大号亮色，false → 下一行小号灰色）
    void DrawCardLyricsSingle(const std::wstring& line,
                              float x, float y, float availWidth,
                              float yOffset, float alpha, bool isCurrent);
    void DrawCardLyrics(const std::wstring& currentLine,
                        const std::wstring& nextLine,
                        float x, float y, float availWidth,
                        float yOffset = 0.0f, float alpha = 1.0f,
                        float curFontSizeScale = 1.0f, float nextFontSizeScale = 1.0f);

    static D2D1_COLOR_F ParseColor(const std::string& hex, float alpha = 1.0f);

    // ═══════════════════════════════
    // 跑马灯（长歌词滚动）状态机
    // ═══════════════════════════════

    /// 跑马灯滚动模式
    enum class MarqueeMode {
        Bounce,   // 左右往返滚动（推荐）
        Loop,     // 传统跑马灯循环
        Off,      // 关闭跑马灯，直接截断
    };

    /// 跑马灯内部状态
    enum class MarqueeState {
        Idle,        // 不需要滚动（短文本 / 跑马灯关闭）
        Delay,       // 延迟等待（歌词刚显示）
        ScrollLeft,  // 向左滚动中
        PauseRight,  // 右端点暂停（仅 bounce 模式）
        ScrollRight, // 向右滚回（仅 bounce 模式）
        PauseLeft,   // 左端点暂停 → 回到 Delay（仅 bounce 模式）
    };

    /// 将字符串转换为 MarqueeMode
    static MarqueeMode ParseMarqueeMode(const std::string& mode);

    /// 更新跑马灯状态机，返回当前应使用的水平滚动偏移量（像素）
    /// needRedraw: 输出参数，表示是否因为滚动动画需要重绘
    /// progress: 当前歌词高亮进度 [0.0, 1.0]，用于控制回位时机
    float UpdateMarquee(const std::string& lyricText, float progress, bool& needRedraw);

    HWND hwnd_{nullptr};
    UINT width_{0};
    UINT height_{0};
    UINT dpi_{96};
    bool initialized_{false};
    bool isVerticalTaskbar_{false};  // 垂直任务栏模式（LEFT/RIGHT）
    bool debugLog_{false};           // 调试日志开关（由 config.debugLog 控制）

    Microsoft::WRL::ComPtr<ID2D1Factory>              d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget>        renderTarget_;
    Microsoft::WRL::ComPtr<IWICImagingFactory>        wicFactory_;
    Microsoft::WRL::ComPtr<IWICBitmap>                wicBitmap_;

    Microsoft::WRL::ComPtr<IDWriteFactory>       dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>    textFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>    translationFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>    btnFormat_;          // 控制按钮图标文字格式（缓存）

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> highlightBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> normalBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> translationBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> spectrumBrush_;

    RenderState lastState_;

    AppearanceConfig settings_;

    // ═══════════════════════════════
    // 跑马灯状态机成员
    // ═══════════════════════════════

    MarqueeState   marqueeState_{MarqueeState::Idle};
    float          scrollOffset_{0.0f};           // 当前水平滚动偏移（像素，正值=文本左移）
    double         stateStartTime_{0.0};          // 当前状态开始时间（QueryPerformanceCounter 秒）
    std::string    marqueeLastText_;              // 上一次的歌词文本（用于检测歌词切换）
    float          marqueeTextWidth_{0.0f};       // 当前歌词文本的像素宽度（缓存）
    float          marqueeMaxOffset_{0.0f};       // 最大可滚动偏移量 = textWidth - availableWidth
    float          marqueeProgress_{0.0f};         // 当前歌词高亮进度 [0.0, 1.0]，用于控制回位时机

    // ═══════════════════════════════
    // 卡片模式成员
    // ═══════════════════════════════

    // 卡片模式专用的 DirectWrite 文本格式（两行不同字号）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> cardCurrentFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> cardNextFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> cardFallbackFormat_;  // 无封面 fallback 字符

    // 封面图缓存：后台线程下载到内存，通过 atomic swap 传递给渲染线程
    // 渲染线程通过 IWICStream::InitializeFromMemory 直接从内存解码，消除磁盘 I/O
    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dCoverBitmap_;  // 渲染线程创建的 D2D 位图（与 renderTarget_ 同域）
    std::string cachedCoverUrl_;
    std::atomic<std::vector<uint8_t>*> pendingCoverData_{nullptr};  // 后台线程 new，原子 swap 给渲染线程。渲染线程 delete 后置 nullptr
    std::atomic<bool> coverLoadInProgress_{false};
    std::atomic<int> coverDownloadGen_{0};  // 代际计数器：URL 变化时递增，下载线程完成后比对以丢弃过期结果

    // 封面裁剪 Layer 缓存（避免每帧 CreateLayer 导致 D2D 资源耗尽）
    Microsoft::WRL::ComPtr<ID2D1Layer>                    coverLayer_;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> coverClipGeo_;
    float                                                 cachedCoverSize_{-1.0f};

    // 卡片模式专用颜色画刷
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardCurrentBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardNextBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardBackgroundBrush_;  // 卡片背景画刷（随封面主色调变化）

    // 动态主题色提取（P1：封面渲染增强）
    /// 从封面像素中提取的主色调（RGBA）。颜色被"欠饱和"处理以避免背景过于鲜艳。
    /// 当封面位图为空时恢复为默认灰蓝色 #737380（即无封面 fallback 背景色）。
    D2D1_COLOR_F coverThemeColor_{0.45f, 0.45f, 0.50f, 1.0f};

    // ═════ P1-④: 卡片背景柔焦（小尺寸 WIC 重采样 + 位图画刷拉伸） ═════
    /// 将封面缩放到 COVER_BLUR_SAMPLE_SIZE 后拉伸铺满卡片，
    /// 利用 D2D 双线性插值产生柔焦效果，代替纯色背景。
    Microsoft::WRL::ComPtr<ID2D1Bitmap>      blurredCoverBg_;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> blurredBgBrush_;
    float blurredBgBitmapW_{0.0f};  // 位图宽度，用于 Stretch 计算

    // ═════ P1-②: 封面 fade-in 过渡动画 ═════
    /// 封面位图从无到有时触发 fade-in，持续 COVER_FADE_DURATION_MS 毫秒。
    /// coverFadeAlpha_ 从 0.0f 渐变至 1.0f，作为 DrawBitmap 的不透明度参数。
    bool    coverFadingIn_{false};
    double  coverFadeStartTime_{0.0};
    float   coverFadeAlpha_{1.0f};     // 默认 1.0（已淡入完成），Initialize 后首帧即可见

    /// 驱动封面 fade-in 进度，返回是否处于动画中（需要持续重绘）
    bool UpdateCoverFade();

    // ═══════════════════════════════
    // 逐字高亮渲染缓存（P1: 缓存 glyph layout，仅在歌词变化时重建）
    // ═══════════════════════════════
    std::wstring         cachedKaraokeText_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> cachedLayout_;
    float                cachedTextWidth_{0.0f};

    // ═══════════════════════════════
    // 卡片模式歌词切换动画（淡入淡出 + 位移）
    // ═══════════════════════════════

    /// 歌词切换动画状态
    enum class CardAnimState {
        Idle,       // 无动画，正常显示
        Animating,  // 动画进行中
    };

    CardAnimState    cardAnimState_{CardAnimState::Idle};
    double          cardAnimStartTime_{0.0};     // 动画开始时间（QPC 秒）
    float           cardAnimProgress_{0.0f};    // 当前动画进度 [0, 1]

    /// 动画期间缓存的旧歌词（用于绘制淡出的旧内容）
    std::string     cardPrevCurrentLine_;        // 旧的当前行
    std::string     cardPrevNextLine_;           // 旧的下一行

    std::string     cardLastCurrentLine_;         // 上一次的当前行（用于检测切换）
    std::string     cardLastNextLine_;         // 上一次的下一行

    /// 更新卡片模式歌词切换动画
    /// 返回是否处于动画中（需要持续重绘）
    bool UpdateCardAnim(const std::string& currentLine, const std::string& nextLine);

    // ═════ P3: 歌词切换动画 + 进度弹簧（普通/卡拉OK模式）═════
    // 解决歌词行切换时"硬切"和逐字高亮进度跳变的问题。

    /// P3-①: 卡拉OK模式歌词行切换 fade 过渡状态
    bool     lyricFadeActive_{false};
    double   lyricFadeStartTime_{0.0};
    std::wstring lyricFadeOldText_;   // 正在淡出的旧行文本（wstring 缓存，避免 UTF-8→wchar 每帧转换）
    std::wstring lyricFadeOldTrans_;  // 旧行的翻译文本（仅在 fade 期间绘制）

    /// P3-②: 卡拉OK进度弹簧物理状态（缓和高亮进度的抖动与跳变）
    double   springProgress_{0.0};    // 当前显示的平滑进度值 [0, 1]
    double   springVelocity_{0.0};    // 进度变化速率（1/s）
    double   springLastTime_{0.0};    // 上一次物理步进的时间戳（QPC 秒）

    /// P3-①: 更新歌词行切换 fade 动画。返回是否处于动画中（需持续重绘）。
    bool UpdateLyricFade(const std::wstring& newText);

    /// P3-②: 弹簧物理步进，将显示进度向目标收敛。返回是否处于运动中（需持续重绘）。
    bool UpdateProgressSpring(double target, double now);

    // ═══════════════════════════════
    // 缓动函数库
    // ═══════════════════════════════

    /// ease-out cubic: f(t) = 1 - (1-t)^3
    static float EaseOutCubic(float t);

    /// ease-in-out quad: f(t) = t<0.5 ? 2t^2 : 1-(2-2t)^2/2
    static float EaseInOutQuad(float t);

    /// ease-out back（带有轻微回弹，用于入场）
    static float EaseOutBack(float t);

    // 频谱渲染
    void DrawSpectrumBars(const std::vector<float>& bands, float x, float width, float y, float height, float alpha = 1.0f);
};

} // namespace moekoe
