// SPDX-License-Identifier: GPL-2.0
// renderer.h - Direct2D + DirectWrite 渲染引擎
// 完全透明背景: WIC + UpdateLayeredWindow + 逐字高亮
#pragma once

#include "lyrics_data.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <atomic>

namespace moekoe {

struct RendererSettings {
    std::string highlightColor{"#4CC2FF"};
    std::string normalColor{"#333333"};
    float       normalOpacity{0.85f};
    std::string fontFamily{"华文细黑"};
    int         fontSize{20};
    bool        enableKaraoke{true};
    bool        enableTranslation{true};

    // 跑马灯（长歌词滚动）配置
    bool        enableMarquee{true};
    std::string marqueeMode{"bounce"};         // bounce / loop / off
    int         marqueeDelayMs{2000};
    int         marqueePauseMs{1000};
    float       marqueeSpeedPxPerSec{40.0f};

    // 显示模式: "karaoke" (默认) | "card" (卡片样式)
    std::string displayMode{"karaoke"};

    // 卡片模式专用字号（独立于 fontSize）
    float       cardCurrentFontSize{18.0f};    // 当前行字号
    float       cardNextFontSize{14.0f};       // 下一行字号

    // 卡片模式专用颜色（独立于 highlightColor / normalColor）
    std::string cardCurrentColor{"#FFFFFF"};   // 当前行文字颜色
    std::string cardNextColor{"#AAAAAA"};      // 下一行文字颜色

    // 卡片模式布局参数
    int         cardCoverSize{34};             // 封面尺寸 (dp, 会按 DPI 缩放)
    int         cardGap{8};                    // 封面与文字间距 (dp)
};

class TaskbarRenderer {
public:
    TaskbarRenderer();
    ~TaskbarRenderer();

    TaskbarRenderer(const TaskbarRenderer&) = delete;
    TaskbarRenderer& operator=(const TaskbarRenderer&) = delete;

    bool Initialize(HWND hwnd);
    void Shutdown();
    void ApplySettings(const RendererSettings& s);
    void Render(const RenderState& state);
    void Resize(UINT width, UINT height, UINT dpi);

    // 设置/查询垂直任务栏模式（LEFT / RIGHT 方位时启用）
    void SetVerticalTaskbar(bool vertical) { isVerticalTaskbar_ = vertical; }
    bool IsVerticalTaskbar() const { return isVerticalTaskbar_; }

private:
    void CreateRenderTarget();
    void DrawHighlightedTextPerCharacter(const std::wstring& text,
                                          double progress,
                                          bool enableKaraoke,
                                          float scrollOffset = 0.0f,
                                          const float* overridePaddingX = nullptr);
    void DrawTranslatedText(const std::wstring& text, const float* overridePaddingX = nullptr);
    void DrawCentered(const std::wstring& text, ID2D1Brush* brush, float yOffset);
    void DrawHoverControls(bool isPlaying);
    void PresentToLayeredWindow();

    // ═════ 卡片模式渲染（无卡拉OK效果） ═════
    void RenderCardStyle(const RenderState& state);
    /// 垂直任务栏专用：堆叠式布局（封面在上，歌词在下）
    void RenderCardStyleVertical(const RenderState& state);
    void DrawCoverArt(const std::string& url, const std::string& fallbackChar,
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

    RenderState lastState_;

    RendererSettings settings_;

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

    // 封面图缓存
    Microsoft::WRL::ComPtr<IWICBitmap> coverBitmap_;
    std::string cachedCoverUrl_;
    std::atomic<bool> coverLoadInProgress_{false};

    // 卡片模式专用颜色画刷
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardCurrentBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardNextBrush_;

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

    // ═══════════════════════════════
    // 缓动函数库
    // ═══════════════════════════════

    /// ease-out cubic: f(t) = 1 - (1-t)^3
    static float EaseOutCubic(float t);

    /// ease-in-out quad: f(t) = t<0.5 ? 2t^2 : 1-(2-2t)^2/2
    static float EaseInOutQuad(float t);

    /// ease-out back（带有轻微回弹，用于入场）
    static float EaseOutBack(float t);
};

} // namespace moekoe
