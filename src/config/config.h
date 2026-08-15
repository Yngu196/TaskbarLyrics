// SPDX-License-Identifier: GPL-3.0
// config.h - 配置管理模块
//
// 职责:
//   - 加载/保存 JSON 配置文件
//   - 提供 enable / auto_start 等开关
//   - 通过注册表管理开机自启
//
#pragma once

#include <string>

#include "core/constants.h"

namespace moekoe {

struct AppearanceConfig {
    std::string highlightColor{"#4CC2FF"};
    std::string normalColor{"#333333"};
    double      normalOpacity{0.85};
    std::string fontFamily{"华文细黑"};
    int         fontSize{20};
    bool        enableKaraoke{true};
    bool        enableTranslation{true};
    // 翻译显示模式: "off"=不显示翻译 | "below"=原文下方显示(默认) | "replace"=替换原文
    std::string translationMode{"below"};

    // 卡片模式翻译显示: "off"=仅原文(不翻译) | "replace"=仅译文 | "dual"=双行显示(原-译)
    std::string cardTranslationMode{"off"};

    // 显示模式: "karaoke" (默认,现有单行卡拉OK) | "card" (卡片样式)
    std::string displayMode{"karaoke"};

    // 卡片模式专用字号（与 font_size 独立）
    int         cardFontSizeCurrent{18};   // 当前行字号（卡片模式）
    int         cardFontSizeNext{14};      // 下一行字号（卡片模式）

    // 卡片模式专用字体（空串时回落 fontFamily）
    std::string cardFontFamily{};

    // 卡片模式专用颜色（独立于 highlightColor / normalColor）
    std::string cardCurrentColor{"#FFFFFF"};  // 当前行文字颜色
    std::string cardNextColor{"#AAAAAA"};     // 下一行文字颜色

    // 跑马灯（长歌词滚动）配置
    bool        enableMarquee{true};           // 是否启用跑马灯
    std::string marqueeMode{"bounce"};        // bounce=往返 / loop=循环 / off=关闭
    int         marqueeDelayMs{2000};          // 歌词显示后延迟多久开始滚动（毫秒）
    int         marqueePauseMs{1000};          // 滚动到端点后暂停时间（毫秒）
    float       marqueeSpeedPxPerSec{40.0f};   // 滚动速度（像素/秒）

    // 封面与布局参数（供渲染器使用）
    int         coverSize{34};                 // 封面尺寸 (dp, 会按 DPI 缩放)
    int         cardGap{8};                    // 封面与文字间距 (dp)

    // 封面开关（独立于显示模式，两种模式都可选）
    bool        enableCover{true};             // 是否显示专辑封面

    // 封面左右偏移量（dp, 负值左移/正值右移, 默认 0）
    int         coverOffsetX{0};

    // 封面圆角百分比（0=正方形, 100=圆形, 默认 17 约 6dp/34dp）
    int         coverCornerRadius{17};

    // 设置界面主题颜色: "blue"(默认,蓝) | "dark"(深色) | "light"(浅色) | "white"(白色)
    std::string settingsTheme{"blue"};

    // 卡片背景模式: "frosted" (默认,半透明毛玻璃) | "transparent" (纯透明)
    std::string cardBackgroundMode{"frosted"};

    // 单行歌词背景模式: "frosted" (默认,半透明毛玻璃) | "transparent" (纯透明)
    std::string singleLineBackgroundMode{"frosted"};

    // 卡片模式歌词显示扩展：长歌词自动加宽显示区域
    bool        cardDynamicWidth{true};

    // 卡片最大扩展比例: 0=默认(1/3), 1=50%, 2=100%
    int         cardMaxExpandRatio{0};

    // 手动窗口宽度覆盖（dp, 0=自动计算；>0 时强制窗口至少为该宽度，
    // 用于任务栏空闲区域检测异常导致窗口被压缩到过窄的场景）
    int         windowWidthOverride{0};

    // 歌词垂直偏移（单行/卡拉OK模式，dp，负值=上移，正值=下移）
    int         lyricOffsetY{0};

    // 双行歌词模式各行垂直偏移（dp，负值=上移，正值=下移）
    int         cardLine1OffsetY{0};  // 首行（当前行）偏移
    int         cardLine2OffsetY{0};  // 第二行（下一行）偏移

    // 纯音乐时频谱显示: "spectrum"=频谱条（默认） | "text"=显示"纯音乐，请欣赏"
    std::string spectrumMode{"spectrum"};
    // 频谱条颜色（hex，默认奶白色）
    std::string spectrumColor{"#F0EFEA"};
    // 频谱 dB 映射上限（dBFS，越大柱子越容易到顶）
    float       spectrumDbCeil{-10.0f};
    // 频谱 dB 映射下限（dBFS，越小底部越安静也能显示）
    float       spectrumDbFloor{-62.0f};
    // 频谱条透明度 [0~1]
    float       spectrumOpacity{1.0f};
    // 频谱柱数 [8~64]
    int         spectrumNumBands{32};
    // 频谱柱宽（dp，0=自动）
    float       spectrumBarWidth{0.0f};
};

struct AdvancedConfig {
    int  websocketPort{6520};
    int  httpServerPort{6523};
    int  refreshRateHz{constants::DEFAULT_REFRESH_RATE_HZ};
    bool debugLog{false};
    bool enableFullscreenHide{true};  // 全屏时自动隐藏歌词
};

// 歌词窗口位置偏移（用户可拖动调整）
struct PositionConfig {
    int  offsetX{0};   // 水平偏移像素
    int  offsetY{0};   // 垂直偏移像素
    bool lockPosition{false};   // 锁定位置（禁止拖动）
    bool lockFully{false};      // 完全锁定（禁止拖动+按钮交互）
};

class Config {
public:
    Config();
    ~Config() = default;

    // 配置文件 schema 版本：当字段重命名/删除时递增，
    // Load 时据此判断是否需要执行迁移逻辑。
    static constexpr int kSchemaVersion = 1;

    // 加载配置文件（不存在时使用默认值并写盘）
    bool Load();

    // 保存到磁盘
    bool Save() const;

    // ---- 开关 ----
    bool IsEnabled()    const { return enabled_; }
    bool IsAutoStart()  const { return autoStart_; }
    void SetEnabled(bool v)   { enabled_ = v; }
    // 设置并立即写注册表；返回注册表操作是否成功
    bool SetAutoStart(bool v);

    // ---- 配置子结构 ----
    const AppearanceConfig& Appearance() const { return appearance_; }
    const AdvancedConfig&   Advanced()   const { return advanced_; }
    const PositionConfig&   Position()   const { return position_; }
    AppearanceConfig&       MutableAppearance() { return appearance_; }
    AdvancedConfig&         MutableAdvanced()   { return advanced_; }
    PositionConfig&         MutablePosition()   { return position_; }

    // ---- 路径 ----
    static std::string GetConfigPath();

    // ---- 导入导出 ----
    // 导出当前配置到指定文件
    bool ExportToFile(const std::string& path) const;
    // 从指定文件导入配置（覆盖当前值并保存）
    bool ImportFromFile(const std::string& path);
    // 重置为默认值并保存
    bool ResetToDefaults();

    // ---- 鉴权 Token ----
    // 从注册表 HKCU\Software\MoeKoeMusic\TaskbarLyrics\authToken 读取。
    // 首次调用时自动生成 UUID 写入注册表，回退使用 MachineGuid 哈希。
    static std::string GetAuthToken();
    // 当所有 Token 来源均不可用时，GetAuthToken 会回退到硬编码 fallback。
    // 调用方（如 HTTP Server）应检查此状态并决定是否拒绝启动。
    static bool IsUsingFallbackToken();

private:
    // 注册表 Run 键方案
    bool SetAutoStartRegistry(bool enable);
    static std::string GetAutoStartRegistryKey();
    // 任务计划程序方案（自启的备选/主推方式，避开杀毒软件对 Run 键的拦截）
    bool SetAutoStartTaskScheduler(bool enable);
    // 启动文件夹快捷方式方案
    bool SetAutoStartStartupFolder(bool enable);

    bool             enabled_{true};
    bool             autoStart_{false};
    AppearanceConfig appearance_;
    AdvancedConfig   advanced_;
    PositionConfig   position_;
};

} // namespace moekoe
