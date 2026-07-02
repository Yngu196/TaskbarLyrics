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

#include "constants.h"

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

    // 卡片模式布局参数（供渲染器使用）
    int         cardCoverSize{34};             // 封面尺寸 (dp, 会按 DPI 缩放)
    int         cardGap{8};                    // 封面与文字间距 (dp)

    // 卡片背景模式: "frosted" (默认,半透明毛玻璃) | "transparent" (纯透明)
    std::string cardBackgroundMode{"frosted"};
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
    bool             autoStart_{true};
    AppearanceConfig appearance_;
    AdvancedConfig   advanced_;
    PositionConfig   position_;
};

} // namespace moekoe
