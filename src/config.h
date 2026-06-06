// SPDX-License-Identifier: GPL-2.0
// config.h - 配置管理模块
//
// 职责:
//   - 加载/保存 JSON 配置文件
//   - 提供 enable / auto_start 等开关
//   - 通过注册表管理开机自启
//
#pragma once

#include <string>

namespace moekoe {

struct AppearanceConfig {
    std::string highlightColor{"#000000"};
    std::string normalColor{"#333333"};
    double      normalOpacity{0.85};
    std::string fontFamily{"Microsoft YaHei UI"};
    int         fontSize{14};
    bool        enableKaraoke{true};
    bool        enableTranslation{true};
};

struct AdvancedConfig {
    int  websocketPort{6520};
    int  refreshRateHz{30};
    bool debugLog{false};
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
    void SetAutoStart(bool v);

    // ---- 配置子结构 ----
    const AppearanceConfig& Appearance() const { return appearance_; }
    const AdvancedConfig&   Advanced()   const { return advanced_; }
    AppearanceConfig&       MutableAppearance() { return appearance_; }
    AdvancedConfig&         MutableAdvanced()   { return advanced_; }

    // ---- 路径 ----
    static std::string GetConfigPath();

private:
    bool SetAutoStartRegistry(bool enable);
    static std::string GetAutoStartRegistryKey();

    bool             enabled_{true};
    bool             autoStart_{true};
    AppearanceConfig appearance_;
    AdvancedConfig   advanced_;
};

} // namespace moekoe
