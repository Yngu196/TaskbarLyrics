// SPDX-License-Identifier: Apache-2.0
// renderer.h - Direct2D + DirectWrite 渲染引擎
// 完全透明背景: WIC + UpdateLayeredWindow + 逐字高亮
#pragma once

#include "config/config.h"
#include "lyrics/lyrics_data.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>

#include "concurrentqueue/concurrentqueue.h"

namespace moekoe {

// 封面下载共享上下文：被渲染线程和后台下载线程共同访问。
// 使用 shared_ptr 管理，确保 detached 下载线程在 renderer 析构后
// 仍能安全访问队列与状态标志，避免 use-after-free。
struct CoverDownloadCtx {
    moodycamel::ConcurrentQueue<std::vector<uint8_t>> pendingCoverQueue;
    std::atomic<bool> coverLoadInProgress{false};
    std::atomic<bool> cancelRequested{false};
    std::atomic<int>  coverDownloadGen{0};  // 代际计数器：URL 变化时递增，下载线程完成后比对以丢弃过期结果
    std::mutex workerMutex;
    std::thread worker;

    ~CoverDownloadCtx();

    // 取消当前下载并回收 worker。URLMon 未响应取消时使用有界等待，
    // 下载线程持有 shared_ptr 上下文，超时脱离也不会访问已销毁的 renderer。
    void CancelAndJoin();
};

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

    // ═════ 媒体卡片（点击任务栏歌词窗口弹出） ═════
    // hoverButton: -1=无, 0=上一首, 1=播放/暂停, 2=下一首（与 MediaCardWindow 命中结果对应）
    // seekPreviewSeconds: >=0 表示进度条正在拖动，按该秒数预览；<0 表示使用 state.currentTime
    void RenderMediaCard(const RenderState& state, int hoverButton,
                         double seekPreviewSeconds, bool seekDragging);

    // 调试日志开关（由 config.debugLog 控制）
    void SetDebugLog(bool enabled) { debugLog_ = enabled; }

    // 设置/查询垂直任务栏模式（LEFT / RIGHT 方位时启用）
    void SetVerticalTaskbar(bool vertical) { isVerticalTaskbar_ = vertical; }
    bool IsVerticalTaskbar() const { return isVerticalTaskbar_; }

    // 测量卡片模式下当前歌词 + 下一行歌词所需窗口宽度（DIPs）
    // 仅在 displayMode == "card" 时有效，否则返回 0
    // 返回值包含封面尺寸、间距和文本内边距，可直接用于窗口宽度决策
    float MeasureCardLyricsWidth(const std::string& curLine, const std::string& nextLine) const;

private:
    void CreateRenderTarget();
    /// 重建所有绑定当前 renderTarget_ 的实心画刷（Initialize / Resize / 设备丢失恢复共用）
    void RecreateBrushes();
    void DrawHighlightedTextPerCharacter(const std::wstring& text,
                                          double progress,
                                          bool enableKaraoke,
                                          float scrollOffset = 0.0f,
                                          const float* overridePaddingLeft = nullptr,
                                          float opacity = 1.0f,
                                          const float* overridePaddingRight = nullptr,
                                          const D2D1_RECT_F* overrideLayout = nullptr,
                                          bool useHighlightColor = false,
                                          double charProgress = 0.0,
                                          double songPositionMs = 0.0,
                                          bool isPlaying = false,
                                          bool allowStardust = false);
    /// 跟唱流光：将流光渐变画刷的渐变轴对齐到卡拉OK进度前沿。
    /// vertical=false 时 front 为前沿 x 坐标（水平渐变），否则为 y 坐标（垂直渐变）。
    /// fontSizeOverride > 0 时用它计算带宽（卡片模式用 cardFontSizeCurrent），否则用 settings_.fontSize。
    /// brush 为空时使用单行模式的 singGlowBrush_，否则使用指定画刷（卡片模式传 cardSingGlowBrush_）。
    /// 返回 false 表示画刷不可用（跳过绘制）。
    bool SetupSingGlowBrush(float front, bool vertical,
                            float fontSizeOverride = 0.0f,
                            ID2D1LinearGradientBrush* brush = nullptr);
    /// 星尘跟唱（参考 taskbar-lyrics-displayer）：五角星沿字形顶边随逐字进度
    /// 跳跃旋转，身后每 24ms 一粒的流星拖尾（432ms 寿命，后拉+横向散开）。
    /// textLeft 为文本绘制原点 x（跑马灯时已含 -scrollOffset），progress 为
    /// 逐字进度 0~1，charProgress 为当前字内进度（跳跃相位）。
    void DrawStardust(const std::wstring& text, const D2D1_RECT_F& lineRect,
                      IDWriteTextLayout* layout, float textWidth, float textLeft,
                      double progress, double charProgress, double songPositionMs,
                      bool playing, float opacity);
    /// 创建跟唱流光渐变画刷（亮-暗-亮对称夹层，单行/卡片模式共用，颜色取基准歌词色）
    void CreateSingGlowGradient(const D2D1_COLOR_F& base,
                                ID2D1LinearGradientBrush** out);
    /// 当前帧是否应绘制跟唱流光（开关+卡拉OK启用+进度在行内+布局有效）
    bool SingGlowActive(bool enableKaraoke, bool useHighlightColor,
                        double progress, bool layoutValid) const;
    void DrawTranslatedText(const std::wstring& text,
                            const float* overridePaddingLeft = nullptr,
                            float opacity = 1.0f,
                            const float* overridePaddingRight = nullptr,
                            const D2D1_RECT_F* overrideLayout = nullptr);
    void DrawHoverControls(bool isPlaying);
    void PresentToLayeredWindow();
    void ReleasePresentResources();
    bool EnsurePresentResources();

    // 媒体卡片绘制辅助
    /// 绘制单行文本（自动省略号截断），alignment 控制左/右对齐
    void DrawMediaCardText(const std::wstring& text, IDWriteTextFormat* format,
                           const D2D1_RECT_F& rect, ID2D1Brush* brush,
                           DWRITE_TEXT_ALIGNMENT alignment,
                           bool ellipsis = true);
    /// 绘制图标字形（Segoe UI Symbol）
    void DrawMediaCardIcon(const wchar_t* icon, const D2D1_RECT_F& rect,
                           IDWriteTextFormat* format, ID2D1Brush* brush);

    // ═════ 卡片模式滚动信息（滚动偏移 + 缓存的无换行文本布局） ═════
    struct CardScrollInfo {
        float offset{0.0f};                 // 当前滚动偏移（px，正值=文本左移）
        IDWriteTextLayout* layout{nullptr}; // 该行的无换行布局（无滚动时为 nullptr）
    };

    // ═════ 卡片模式渲染（无卡拉OK效果） ═════
    void RenderCardStyle(const RenderState& state,
                         const CardScrollInfo& curScroll = CardScrollInfo{},
                         const CardScrollInfo& nextScroll = CardScrollInfo{});
    /// 垂直任务栏专用：堆叠式布局（封面在上，歌词在下）
    void RenderCardStyleVertical(const RenderState& state);
    /// 绘制卡片背景（毛玻璃封面 + 半透明主题色叠加），纯音乐模式共用
    void DrawCardBackground();
    void DrawCoverArt(const std::string& url, wchar_t fallbackChar,
                      float x, float y, float size);
    /// 绘制单行卡片模式歌词（isCurrent=true → 当前行大号亮色，false → 下一行小号灰色）
    /// brushOverride 非空时使用指定画刷（悬停歌曲信息使用高亮色）。
    /// scrollOffset > 0 时按偏移水平滚动绘制（scrollLayout 为对应行的无换行布局）。
    /// progress/karaokeEnabled 仅 isCurrent=true 时生效：卡拉OK高亮填充 + 跟唱流光。
    void DrawCardLyricsSingle(const std::wstring& line,
                              float x, float y, float availWidth,
                              float yOffset, float alpha, bool isCurrent,
                              ID2D1SolidColorBrush* brushOverride = nullptr,
                              float scrollOffset = 0.0f,
                              IDWriteTextLayout* scrollLayout = nullptr,
                              double progress = 0.0,
                              bool karaokeEnabled = false);
    void DrawCardLyrics(const std::wstring& currentLine,
                        const std::wstring& nextLine,
                        float x, float y, float availWidth,
                        float yOffset = 0.0f, float alpha = 1.0f,
                        float curFontSizeScale = 1.0f, float nextFontSizeScale = 1.0f,
                        ID2D1SolidColorBrush* brushOverride = nullptr,
                        const CardScrollInfo& curScroll = CardScrollInfo{},
                        const CardScrollInfo& nextScroll = CardScrollInfo{},
                        double progress = 0.0);

    static D2D1_COLOR_F ParseColor(const std::string& hex, float alpha = 1.0f);

    // ═══════════════════════════════
    // 跑马灯（长歌词滚动）状态机
    // ═══════════════════════════════

    /// 跑马灯滚动模式
    enum class MarqueeMode {
        Bounce,   // 历史命名：现语义为"跟随滚动"（旧往返行为已移除）
        Off,      // 关闭跑马灯，直接截断
    };

    /// 跑马灯内部状态
    enum class MarqueeState {
        Idle,        // 不需要滚动（短文本 / 跑马灯关闭）
        Delay,       // 延迟等待（卡片模式开始滚动前）
        ScrollLeft,  // 向左滚动中
        PauseRight,  // 终端态：滚动到末端后定住（复用自旧 bounce 命名）
    };

    /// 将字符串转换为 MarqueeMode
    static MarqueeMode ParseMarqueeMode(const std::string& mode);

    /// 更新跑马灯状态机，返回当前应使用的水平滚动偏移量（像素）
    /// needRedraw: 输出参数，表示是否因为滚动动画需要重绘
    /// progress: 当前歌词高亮进度 [0.0, 1.0]，用于控制回位时机
    /// availableWidth: 歌词实际可用宽度（已考虑封面偏移），用于计算正确的最大滚动偏移
    float UpdateMarquee(const std::string& lyricText, float progress, bool& needRedraw, float availableWidth = 0.0f);

    /// 垂直任务栏专用：按实际可视高度与歌词进度驱动上下滚动（单程，到末端定住）。
    float UpdateVerticalMarquee(const std::wstring& lyricText, float progress, float glyphHeight,
                                float availableHeight, bool& needRedraw);

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
    HDC     presentMemDC_{nullptr};
    HBITMAP presentBitmap_{nullptr};
    void*   presentBits_{nullptr};
    UINT    presentWidth_{0};
    UINT    presentHeight_{0};

    Microsoft::WRL::ComPtr<IDWriteFactory>       dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>    textFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>    translationFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>    btnFormat_;          // 控制按钮图标文字格式（缓存）

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> highlightBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> normalBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> translationBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> spectrumBrush_;

    // 跟唱流光渐变画刷（渐变轴每帧按进度前沿动态设置）
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> singGlowBrush_;

    // ═════ 星尘跟唱（单行逐字歌词） ═════
    // 星星/拖尾用实心画刷（颜色 = 高亮歌词色，换色随 RecreateBrushes 重建）
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stardustBrush_;
    // 单位五角星路径几何（首次绘制时创建，之后仅改绘制矩阵）
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> starGeometry_;
    // 流星拖尾：星心位置的环形采样缓冲（渲染线程每帧记录，粒子按 24ms 网格回溯）
    struct StardustSample { double time{0.0}; float x{0.0f}, y{0.0f}; };
    static constexpr int kStardustTrailSize = 32;
    StardustSample stardustTrail_[kStardustTrailSize]{};
    int         stardustTrailHead_{0};    // 下一写入槽位
    int         stardustTrailCount_{0};   // 有效样本数（最多 kStardustTrailSize）
    std::wstring stardustLine_;           // 当前生效的歌词文本（行变化时清空轨迹）
    double      stardustPauseStart_{0.0}; // 暂停时刻（暂停 300ms 内星星淡出）
    // 单行卡拉OK文本高度缓存（星尘"歌词下沉"量计算用）
    float       cachedTextHeight_{0.0f};

    // 卡片模式颜色画刷：普通歌词（未唱 + 下一行）与高亮歌词（已唱填充）
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardHighlightBrush_;
    // 卡片模式跟唱流光渐变画刷（颜色取卡片高亮色，渐变轴动态设置）
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> cardSingGlowBrush_;

    // 卡片模式逐字高亮：当前行文本宽度测量缓存（仅在文本/宽度变化时重建布局）
    Microsoft::WRL::ComPtr<IDWriteTextLayout> cardKaraokeLayout_;
    std::wstring cardKaraokeText_;
    float        cardKaraokeAvailWidth_{-1.0f};
    float        cardKaraokeTextWidth_{0.0f};

    RenderState lastState_;

    AppearanceConfig settings_;
    std::string cardTranslationMode_{"dual"};  // 卡片模式翻译显示模式

    // ═══════════════════════════════
    // 跑马灯状态机成员
    // ═══════════════════════════════

    MarqueeState   marqueeState_{MarqueeState::Idle};
    float          scrollOffset_{0.0f};           // 当前水平滚动偏移（像素，正值=文本左移）
    double         stateStartTime_{0.0};          // 当前状态开始时间（QueryPerformanceCounter 秒）
    double         marqueeLastUpdateTime_{0.0};   // 上次更新跑马灯的时间（用于帧间增量）
    std::string    marqueeLastText_;              // 上一次的歌词文本（用于检测歌词切换）
    float          marqueeTextWidth_{0.0f};       // 当前歌词文本的像素宽度（缓存）
    float          marqueeMaxOffset_{0.0f};       // 最大可滚动偏移量 = textWidth - availableWidth
    float          marqueeProgress_{0.0f};         // 当前歌词高亮进度 [0.0, 1.0]，用于控制回位时机

    // 垂直任务栏的逐字歌词滚动状态（与水平跑马灯分开，避免切换方向时串状态）
    MarqueeState   verticalMarqueeState_{MarqueeState::Idle};
    float          verticalScrollOffset_{0.0f};
    float          verticalMaxOffset_{0.0f};
    std::wstring   verticalMarqueeText_;
    double         verticalStateStartTime_{0.0};
    double         verticalLastUpdateTime_{0.0};

    // ═══════════════════════════════
    // 卡片模式跑马灯（长歌词滚动，仅当前行、单程滚到末端定住）
    // 仅在 displayMode=="card" 且关闭"歌词显示扩展"时启用；
    // 纯时间驱动（LRC 无逐字进度也能滚动）。
    // ═══════════════════════════════
    struct CardMarqueeState {
        MarqueeState state{MarqueeState::Idle};
        float        offset{0.0f};      // 当前滚动偏移（px）
        float        maxOffset{0.0f};   // 最大滚动距离 = textWidth - availWidth
        float        textWidth{0.0f};   // 文本像素宽度（缓存）
        std::wstring text;              // 上一次文本（变化检测）
        double       stateStart{0.0};   // 当前状态开始时间（秒）
        double       lastUpdate{0.0};   // 上次步进时间（秒）
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;  // 无换行布局缓存
    };
    CardMarqueeState cardMarqueeCur_;

    /// 卡片模式跑马灯步进（进度驱动单程滚动：偏移目标 = maxOffset × progress，
    /// 以设定速度向目标逼近——高亮从行首推进时同步开始滚动，唱完必然滚完）。
    /// enabled=false 时重置状态并返回 0。needRedraw 表示本帧因滚动需要重绘。
    float UpdateCardMarquee(CardMarqueeState& m, const std::wstring& text,
                            IDWriteTextFormat* format, float availWidth, float boxHeight,
                            bool enabled, double progress, bool& needRedraw);

    // ═══════════════════════════════
    // 卡片模式成员
    // ═══════════════════════════════

    // 卡片模式专用的 DirectWrite 文本格式（两行不同字号）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> cardCurrentFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> cardNextFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> cardFallbackFormat_;  // 无封面 fallback 字符

    // 媒体卡片文本格式（固定字号，按 DPI 缩放）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> mediaTitleFormat_;   // 歌曲名（粗体）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> mediaArtistFormat_;  // 歌手（灰色）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> mediaMetaFormat_;    // 来源（小号灰色）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> mediaTimeFormat_;    // 时间标签
    Microsoft::WRL::ComPtr<IDWriteTextFormat> mediaIconFormat_;    // 控制按钮图标

    // 封面图缓存：后台线程下载到内存，通过 atomic swap 传递给渲染线程
    // 渲染线程通过 IWICStream::InitializeFromMemory 直接从内存解码，消除磁盘 I/O
    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dCoverBitmap_;  // 渲染线程创建的 D2D 位图（与 renderTarget_ 同域）
    std::string cachedCoverUrl_;
    // 封面数据缓冲：后台线程写入，渲染线程消费。
    // 使用 shared_ptr<CoverDownloadCtx> 持有队列与状态标志；下载 worker
    // 在 Shutdown 中取消并 join，避免 renderer 析构后仍有后台访问。
    std::shared_ptr<CoverDownloadCtx> coverCtx_;

    // 封面裁剪 Layer 缓存（避免每帧 CreateLayer 导致 D2D 资源耗尽）
    Microsoft::WRL::ComPtr<ID2D1Layer>                    coverLayer_;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> coverClipGeo_;
    float                                                 cachedCoverSize_{-1.0f};

    // 卡片模式专用颜色画刷
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardCurrentBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cardBackgroundBrush_;  // 卡片背景画刷（随封面主色调变化）

    // 媒体卡片画刷（深色玻璃卡片）
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaPanelBrush_;     // 卡片底色
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaBorderBrush_;    // 卡片描边
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaTextBrush_;      // 主文本
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaSubTextBrush_;   // 次级文本
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaTrackBrush_;     // 进度条轨道
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaAccentBrush_;    // 进度条已播部分 / 强调色
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaKnobBrush_;      // 进度条拖钮
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaIconBrush_;      // 播放按钮上的深色图标
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mediaHoverBrush_;     // 按钮悬停底色

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
    float                cachedLayoutWidth_{0.0f};
    float                cachedLayoutHeight_{0.0f};

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
    int             cardLastLineIndex_{-1};   // 当前行索引

    /// 更新卡片模式歌词切换动画
    /// 返回是否处于动画中（需要持续重绘）
    bool UpdateCardAnim(const std::string& currentLine, const std::string& nextLine,
                        int currentLineIndex);

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

    /// ease-out back（带有轻微回弹，用于入场）
    static float EaseOutBack(float t);

    // 频谱渲染
    void DrawSpectrumBars(const std::vector<float>& bands, float x, float width, float y, float height, float alpha = 1.0f);
};

} // namespace moekoe
