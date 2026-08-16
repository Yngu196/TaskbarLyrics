// SPDX-License-Identifier: GPL-3.0
// settings_page.h - 设置页面基类 + 6 个页面
//
// 职责:
//   - SettingsPage 基类：定义页面接口（Title/Subtitle/BuildContent/CollectChanges）
//   - 6 个页面：歌词 / 外观 / 窗口 / 行为 / 高级 / 关于
//
#pragma once

#include "ui/ui_elements.h"
#include "config/config.h"

namespace moekoe::ui {

// ── 页面基类 ──

class SettingsPage : public UIElement {
public:
    virtual ~SettingsPage() = default;

    // 页面元信息
    virtual std::string Title() const = 0;
    virtual std::string Subtitle() const = 0;

    // 构建页面内容（Card + 控件）
    virtual void BuildContent(const moekoe::Config& cfg) = 0;

    // 从控件收集配置变更
    virtual void CollectChanges(moekoe::Config& cfg) = 0;

    // 用指定配置重建页面（清除旧控件并重新 BuildContent）
    void Rebuild(const moekoe::Config& cfg) {
        children_.clear();
        BuildContent(cfg);
    }

    // UIElement 接口
    float MeasureWidth(float availWidth) override;
    float MeasureHeight(float availWidth) override;
    void Arrange(float x, float y, float w, float h) override;
    void Draw(ID2D1RenderTarget* rt) override;
    void Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) override;
};

// ── 6 个页面 ──

class LyricsPage : public SettingsPage {
public:
    std::string Title() const override { return "歌词"; }
    std::string Subtitle() const override { return "自定义歌词内容和显示方式"; }
    void BuildContent(const moekoe::Config& cfg) override;
    void CollectChanges(moekoe::Config& cfg) override;

    // 根据 displayMode 更新"卡拉OK效果"可见性和翻译模式选项
    void UpdateForDisplayMode(const std::string& displayMode, const moekoe::Config& cfg);
};

class AppearancePage : public SettingsPage {
public:
    std::string Title() const override { return "外观"; }
    std::string Subtitle() const override { return "调整歌词视觉效果"; }
    void BuildContent(const moekoe::Config& cfg) override;
    void CollectChanges(moekoe::Config& cfg) override;

    // 根据 displayMode 更新卡拉OK/卡片 Card 的可见性
    void UpdateVisibility(const std::string& displayMode);
};

class SpectrumPage : public SettingsPage {
public:
    std::string Title() const override { return "频谱"; }
    std::string Subtitle() const override { return "纯音乐时的频谱显示设置"; }
    void BuildContent(const moekoe::Config& cfg) override;
    void CollectChanges(moekoe::Config& cfg) override;
};

class WindowPage : public SettingsPage {
public:
    std::string Title() const override { return "窗口"; }
    std::string Subtitle() const override { return "调整歌词窗口位置和尺寸"; }
    void BuildContent(const moekoe::Config& cfg) override;
    void CollectChanges(moekoe::Config& cfg) override;
};

class BehaviorPage : public SettingsPage {
public:
    std::string Title() const override { return "行为"; }
    std::string Subtitle() const override { return "插件启动和窗口行为"; }
    void BuildContent(const moekoe::Config& cfg) override;
    void CollectChanges(moekoe::Config& cfg) override;
};

class AdvancedPage : public SettingsPage {
public:
    std::string Title() const override { return "高级"; }
    std::string Subtitle() const override { return "高级设置（修改端口可能影响使用）"; }
    void BuildContent(const moekoe::Config& cfg) override;
    void CollectChanges(moekoe::Config& cfg) override;

    // 根据 displayMode 更新仅双行模式可见的 Card 的可见性
    void UpdateVisibility(const std::string& displayMode);
};

class AboutPage : public SettingsPage {
public:
    std::string Title() const override { return "关于"; }
    std::string Subtitle() const override { return "插件信息"; }
    void BuildContent(const moekoe::Config& cfg) override;
    void CollectChanges(moekoe::Config& cfg) override;
};

} // namespace moekoe::ui
