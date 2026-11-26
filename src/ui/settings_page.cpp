// SPDX-License-Identifier: Apache-2.0
// settings_page.cpp - 设置页面实现（Win11 设置风格：分组标题 + 分组卡片内多行 + 行间分隔线）
#include "ui/settings_page.h"
#include "ui/color_utils.h"
#include "core/constants.h"
#include "taskbar/taskbar_selector.h"

using moekoe::Utf8ToWide;
using moekoe::HexToColorF;
using moekoe::ColorFToHex;

namespace moekoe::ui {

// ═══════════════════════════════════════
// SettingsPage 基类
// ═══════════════════════════════════════

float SettingsPage::MeasureWidth(float availWidth) {
    return availWidth;
}

float SettingsPage::MeasureHeight(float availWidth) {
    float h = 60;  // 标题 + 副标题区域

    for (auto& child : children_) {
        if (child->IsVisible()) {
            h += child->MeasureHeight(availWidth - 40);  // 内容区左右各 20 边距
            h += 12;  // 分组间距
        }
    }
    return h;
}

void SettingsPage::Arrange(float x, float y, float w, float h) {
    UIElement::Arrange(x, y, w, h);

    float contentX = x + 20;
    float contentW = w - 40;
    float curY = y + 60;  // 标题区域之后

    for (auto& child : children_) {
        if (!child->IsVisible()) continue;
        float childH = child->MeasureHeight(contentW);
        child->Arrange(contentX, curY, contentW, childH);
        curY += childH + 12;
    }
}

void SettingsPage::Draw(ID2D1RenderTarget* rt) {
    Draw(rt, DrawContext{});
}

void SettingsPage::Draw(ID2D1RenderTarget* rt, const DrawContext& ctx) {
    if (!visible_) return;

    // 辅助函数
    static auto makeBrush = [](ID2D1RenderTarget* rt, const D2D1_COLOR_F& c) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(c, b.GetAddressOf());
        return b;
    };
    static auto getFactory = []() -> Microsoft::WRL::ComPtr<IDWriteFactory>& {
        return moekoe::ui::GetSharedDWriteFactory();
    };

    // 页面标题（Segoe UI Variable 28px SemiBold #E0E0E0）
    auto titleFmt = [&]() {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        getFactory()->CreateTextFormat(L"Segoe UI Variable", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            28, L"zh-Hans", fmt.GetAddressOf());
        return fmt;
    }();

    if (titleFmt) {
        auto titleBrush = makeBrush(rt, ctx.Text());
        std::wstring wtitle = Utf8ToWide(Title());
        rt->DrawTextW(wtitle.c_str(), static_cast<UINT32>(wtitle.size()),
                      titleFmt.Get(), D2D1::RectF(x_ + 20, y_ + 8, x_ + w_ - 20, y_ + 40),
                      titleBrush.Get());
    }

    // 副标题（Segoe UI Variable 13px #B0B0B0）
    auto subFmt = [&]() {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        getFactory()->CreateTextFormat(L"Segoe UI Variable", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13, L"zh-Hans", fmt.GetAddressOf());
        return fmt;
    }();

    if (subFmt) {
        auto subBrush = makeBrush(rt, ctx.TextSecondary());
        std::wstring wsub = Utf8ToWide(Subtitle());
        rt->DrawTextW(wsub.c_str(), static_cast<UINT32>(wsub.size()),
                      subFmt.Get(), D2D1::RectF(x_ + 20, y_ + 40, x_ + w_ - 20, y_ + 58),
                      subBrush.Get());
    }

    // 子元素（分组标题 / 分组卡片），传递 DrawContext
    for (auto& child : children_) {
        if (child->IsVisible()) child->Draw(rt, ctx);
    }
}

// ═══════════════════════════════════════
// 辅助：分组标题 + 分组卡片 + 设置行
// ═══════════════════════════════════════

// 分组标题：纯文字（无蓝竖条），参考 Win11 设置的组头
static TextBlock* AddSection(SettingsPage& p, const std::string& title,
                             const std::string& id = "") {
    auto t = std::make_unique<TextBlock>();
    t->text = title;
    t->style = TextBlock::Style::SectionHeader;
    t->id = id;
    TextBlock* raw = t.get();
    p.AddChild(std::move(t));
    return raw;
}

// 分组卡片：一张圆角卡片容纳多行设置行（行间分隔线由 Card 自动绘制）
static Card* AddGroup(SettingsPage& p, const std::string& id = "") {
    auto card = std::make_unique<Card>();
    card->title = "";
    card->cornerRadius = 12;
    card->padding = {0, 0, 0, 0};
    card->gap = 0;
    card->id = id;
    Card* raw = card.get();
    p.AddChild(std::move(card));
    return raw;
}

// 分组内设置行：左「图标+标题+说明」右控件；返回行指针便于设置可见性
static SettingRow* AddGroupRow(Card& g, const std::string& title, const std::string& desc,
                               const wchar_t* icon, std::unique_ptr<UIElement> ctrl) {
    auto row = std::make_unique<SettingRow>();
    row->title = title;
    row->description = desc;
    if (icon && *icon) row->icon = icon;
    row->grouped = true;
    if (ctrl) row->AddChild(std::move(ctrl));
    SettingRow* raw = row.get();
    g.AddChild(std::move(row));
    return raw;
}

// 仅在可见性由控件联动控制时使用：把可见性同步到行与控件两级
static void SetRowVisible(UIElement* ctrl, bool v) {
    if (ctrl) ctrl->SetVisible(v);
    if (ctrl && ctrl->Parent()) ctrl->Parent()->SetVisible(v);
}

// 辅助：创建整宽内容 Card（用于 ThemePresets / 关于页）
static std::unique_ptr<Card> MakeCard(const std::string& title) {
    auto card = std::make_unique<Card>();
    card->title = title;
    card->cornerRadius = 12;
    card->padding = {20, 20, 20, 20};
    card->gap = 8;
    return card;
}

// ═══════════════════════════════════════
// 页面 1：歌词
// ═══════════════════════════════════════

void LyricsPage::BuildContent(const moekoe::Config& cfg) {
    const auto& a = cfg.Appearance();
    bool isCard = (a.displayMode == "card");

    AddSection(*this, "显示内容");
    {
        Card* g = AddGroup(*this);
        auto dm = std::make_unique<ComboBox>();
        dm->id = "displayMode";
        dm->items = {"单行歌词", "双行歌词"};
        dm->selectedIndex = isCard ? 1 : 0;
        AddGroupRow(*g, "显示模式",
                    "左侧或右侧任务栏将自动使用单行歌词；双行歌词仅适用于顶部和底部任务栏",
                    L"\uE8A1", std::move(dm));
        auto t = std::make_unique<Toggle>();
        t->id = "karaoke";
        t->value = a.enableKaraoke;
        AddGroupRow(*g, "逐字高亮", "卡拉OK 逐字填充效果", L"\uE734", std::move(t));
        auto glow = std::make_unique<Toggle>();
        glow->id = "singGlow";
        glow->value = a.enableSingAlongGlow;
        AddGroupRow(*g, "跟唱流光",
                    "需要开启逐字高亮", L"\uE706", std::move(glow));
        auto stardust = std::make_unique<Toggle>();
        stardust->id = "stardust";
        stardust->value = a.enableStardust;
        auto* stardustRow = AddGroupRow(*g, "星尘跟唱",
                    "仅单行逐字歌词生效，歌词下沉、星星跳动旋转并带流星粒子拖尾；与翻译显示互斥",
                    L"\uE734", std::move(stardust));
        stardustRow->SetVisible(!isCard);
    }

    AddSection(*this, "翻译模式");
    {
        Card* g = AddGroup(*this);
        auto dm = std::make_unique<ComboBox>();
        dm->id = "translationMode";
        if (isCard) {
            // 卡片模式：仅原文、仅翻译、原文+翻译
            dm->items = {"仅原文", "仅翻译", "原文+翻译"};
            dm->selectedIndex = (a.cardTranslationMode == "replace") ? 1 :
                               (a.cardTranslationMode == "dual")   ? 2 : 0;
        } else {
            // 卡拉OK模式：仅原文、仅翻译
            dm->items = {"仅原文", "仅翻译"};
            dm->selectedIndex = (a.translationMode == "replace") ? 1 : 0;
        }
        AddGroupRow(*g, "翻译模式", "", L"\uE713", std::move(dm));
    }
}

void LyricsPage::CollectChanges(moekoe::Config& cfg) {
    auto& ap = cfg.MutableAppearance();
    for (auto& group : children_) {
        for (auto& row : group->Children()) {
            for (auto& child : row->Children()) {
                if (auto* cb = dynamic_cast<ComboBox*>(child.get())) {
                    if (cb->id == "displayMode") {
                        ap.displayMode = (cb->selectedIndex == 1) ? "card" : "karaoke";
                    } else if (cb->id == "translationMode") {
                        bool isCard = (ap.displayMode == "card");
                        if (isCard) {
                            ap.cardTranslationMode = (cb->selectedIndex == 1) ? "replace" :
                                                     (cb->selectedIndex == 2) ? "dual" : "off";
                        } else {
                            ap.translationMode = (cb->selectedIndex == 1) ? "replace" : "off";
                        }
                    }
                } else if (auto* t = dynamic_cast<Toggle*>(child.get())) {
                    if (t->id == "karaoke") ap.enableKaraoke = t->value;
                    else if (t->id == "singGlow") ap.enableSingAlongGlow = t->value;
                    else if (t->id == "stardust") ap.enableStardust = t->value;
                }
            }
        }
    }
}

void LyricsPage::UpdateForDisplayMode(const std::string& displayMode, const moekoe::Config& cfg) {
    bool isCard = (displayMode == "card");
    const auto& a = cfg.Appearance();

    for (auto& group : children_) {
        for (auto& row : group->Children()) {
            for (auto& child : row->Children()) {
                // 星尘跟唱：仅单行歌词模式生效，双行模式下隐藏该行
                if (auto* t = dynamic_cast<Toggle*>(child.get())) {
                    if (t->id == "stardust") {
                        SetRowVisible(t, !isCard);
                    }
                }
                // 更新翻译模式选项
                if (auto* cb = dynamic_cast<ComboBox*>(child.get())) {
                    if (cb->id == "translationMode") {
                        if (isCard) {
                            cb->items = {"仅原文", "仅翻译", "原文+翻译"};
                            cb->selectedIndex = (a.cardTranslationMode == "replace") ? 1 :
                                               (a.cardTranslationMode == "dual")   ? 2 : 0;
                        } else {
                            cb->items = {"仅原文", "仅翻译"};
                            cb->selectedIndex = (a.translationMode == "replace") ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
}

// ═══════════════════════════════════════
// 页面 2：外观
// ═══════════════════════════════════════

void AppearancePage::BuildContent(const moekoe::Config& cfg) {
    const auto& a = cfg.Appearance();
    bool isCard = (a.displayMode == "card");

    // ── 通用选项（两种模式都可见）──

    AddSection(*this, "封面");
    {
        Card* g = AddGroup(*this);
        auto t = std::make_unique<Toggle>();
        t->id = "enableCover";
        t->value = a.enableCover;
        AddGroupRow(*g, "显示专辑封面", "", L"\uE790", std::move(t));
        auto s = std::make_unique<Slider>();
        s->id = "coverSize";
        s->minValue = 16; s->maxValue = 80;
        s->value = static_cast<float>(a.coverSize);
        s->suffix = " dp";
        AddGroupRow(*g, "封面尺寸", "", L"\uE70F", std::move(s));
        s = std::make_unique<Slider>();
        s->id = "coverOffsetX";
        s->minValue = -50; s->maxValue = 50;
        s->value = static_cast<float>(a.coverOffsetX);
        s->suffix = " dp";
        AddGroupRow(*g, "左右偏移", "", L"\uE70F", std::move(s));
        s = std::make_unique<Slider>();
        s->id = "coverCornerRadius";
        s->minValue = 0; s->maxValue = 100;
        s->value = static_cast<float>(a.coverCornerRadius);
        s->suffix = "%";
        AddGroupRow(*g, "封面圆角", "", L"\uE70F", std::move(s));
    }

    AddSection(*this, "主题颜色");
    {
        Card* g = AddGroup(*this);
        auto dm = std::make_unique<ComboBox>();
        dm->id = "settingsTheme";
        dm->items = {"浅色", "深色"};
        dm->selectedIndex = (a.settingsTheme == "light") ? 0 : 1;
        AddGroupRow(*g, "设置界面主题", "", L"\uE790", std::move(dm));
    }

    // ── 单行歌词区域（仅卡拉OK模式可见）──

    AddSection(*this, "字体", "karaokeFontH");
    {
        Card* g = AddGroup(*this, "karaokeFont");
        g->SetVisible(!isCard);
        auto s = std::make_unique<Slider>();
        s->id = "fontSize";
        s->minValue = 10; s->maxValue = 28;
        s->value = static_cast<float>(a.fontSize);
        AddGroupRow(*g, "字号", "", L"\uE70F", std::move(s));
        auto lr = std::make_unique<LabelRow>();
        lr->id = "fontFamily";
        lr->textValue = a.fontFamily;
        AddGroupRow(*g, "字体", "", L"\uE70F", std::move(lr));
    }

    AddSection(*this, "颜色", "karaokeColorH");
    {
        Card* g = AddGroup(*this, "karaokeColor");
        g->SetVisible(!isCard);
        auto cr = std::make_unique<ColorRow>();
        cr->id = "normalColor";
        cr->textValue = a.normalColor;
        cr->colorValue = HexToColorF(a.normalColor);
        AddGroupRow(*g, "普通歌词颜色", "", L"\uE790", std::move(cr));
        cr = std::make_unique<ColorRow>();
        cr->id = "highlightColor";
        cr->textValue = a.highlightColor;
        cr->colorValue = HexToColorF(a.highlightColor);
        AddGroupRow(*g, "高亮歌词颜色", "", L"\uE790", std::move(cr));
        auto s = std::make_unique<Slider>();
        s->id = "opacity";
        s->minValue = 20; s->maxValue = 100;
        s->value = static_cast<float>(a.normalOpacity * 100.0);
        s->suffix = "%";
        AddGroupRow(*g, "不透明度", "", L"\uE70F", std::move(s));
    }

    AddSection(*this, "预设主题", "themePresetsH");
    {
        // 整宽色板（与上方「颜色」行联动）
        auto card = MakeCard("");
        card->id = "themePresets";
        auto tp = std::make_unique<ThemePresets>();
        tp->id = "themePresets";
        tp->presets = {
            {HexToColorF("#4CC2FF"), HexToColorF("#FFFFFF"), "默认"},
            {HexToColorF("#EC4141"), HexToColorF("#FFFFFF"), "网易云红"},
            {HexToColorF("#31C27C"), HexToColorF("#FFFFFF"), "QQ音乐绿"},
            {HexToColorF("#FF9800"), HexToColorF("#FFFFFF"), "暖橙"},
            {HexToColorF("#E040FB"), HexToColorF("#E0E0E0"), "紫罗兰"},
            {HexToColorF("#00E676"), HexToColorF("#B0BEC5"), "薄荷"},
        };
        // 检查当前是否匹配某个预设
        for (int i = 0; i < static_cast<int>(tp->presets.size()); ++i) {
            if (ColorFToHex(tp->presets[i].hlColor) == a.highlightColor &&
                ColorFToHex(tp->presets[i].nlColor) == a.normalColor) {
                tp->selectedIndex = i;
                break;
            }
        }
        card->AddChild(std::move(tp));
        card->SetVisible(!isCard);
        AddChild(std::move(card));
    }

    AddSection(*this, "长歌词滚动", "marqueeH");
    {
        // 卡片模式关闭"歌词显示扩展"后同样使用本组参数（仅当前行，跟随高亮进度滚动）
        Card* g = AddGroup(*this, "marqueeCard");
        auto t = std::make_unique<Toggle>();
        t->id = "marquee";
        t->value = a.enableMarquee;
        AddGroupRow(*g, "启用长歌词滚动效果", "", L"\uE72C", std::move(t));
        auto dm = std::make_unique<ComboBox>();
        dm->id = "marqueeMode";
        dm->items = {"跟随滚动（推荐）", "关闭（截断显示）"};
        dm->selectedIndex = (a.marqueeMode == "off") ? 1 : 0;
        AddGroupRow(*g, "滚动模式", "跟随高亮进度滚动，唱完停在末端（单行与双行模式一致）", L"\uE72C", std::move(dm));
        auto s = std::make_unique<Slider>();
        s->id = "marqueeDelay";
        s->minValue = 0; s->maxValue = 5000;
        s->value = static_cast<float>(a.marqueeDelayMs);
        s->suffix = " ms";
        AddGroupRow(*g, "开始延迟", "滚动开始前的等待时间", L"\uE70F", std::move(s));
        s = std::make_unique<Slider>();
        s->id = "marqueeSpeed";
        s->minValue = 10; s->maxValue = 200;
        s->value = a.marqueeSpeedPxPerSec;
        s->suffix = " px/s";
        AddGroupRow(*g, "滚动速度", "", L"\uE70F", std::move(s));
    }

    // 开关关闭或选择"关闭（截断显示）"时，隐藏无效的滚动参数行。
    UpdateMarqueeVisibility(a.enableMarquee, a.marqueeMode);

    AddSection(*this, "歌词位置", "karaokeOffsetH");
    {
        Card* g = AddGroup(*this, "karaokeOffset");
        g->SetVisible(!isCard);
        auto s = std::make_unique<Slider>();
        s->id = "lyricOffsetY";
        s->minValue = -100; s->maxValue = 100;
        s->value = static_cast<float>(a.lyricOffsetY);
        s->suffix = " px";
        AddGroupRow(*g, "上下偏移", "", L"\uE70F", std::move(s));
    }

    AddSection(*this, "单行背景", "singleLineBgH");
    {
        Card* g = AddGroup(*this, "singleLineBg");
        g->SetVisible(!isCard);
        auto dm = std::make_unique<ComboBox>();
        dm->id = "singleLineBackgroundMode";
        dm->items = {"半透明毛玻璃", "纯透明"};
        dm->selectedIndex = (a.singleLineBackgroundMode == "transparent") ? 1 : 0;
        AddGroupRow(*g, "背景样式", "", L"\uE73F", std::move(dm));
    }

    // ── 双行歌词区域（仅卡片模式可见）──

    AddSection(*this, "卡片字体", "cardFontH");
    {
        Card* g = AddGroup(*this, "cardFont");
        g->SetVisible(isCard);
        auto s = std::make_unique<Slider>();
        s->id = "cardFontSizeCurrent";
        s->minValue = 10; s->maxValue = 20;
        s->value = static_cast<float>(a.cardFontSizeCurrent);
        AddGroupRow(*g, "当前行字号", "", L"\uE70F", std::move(s));
        s = std::make_unique<Slider>();
        s->id = "cardFontSizeNext";
        s->minValue = 8; s->maxValue = 18;
        s->value = static_cast<float>(a.cardFontSizeNext);
        AddGroupRow(*g, "下一行字号", "", L"\uE70F", std::move(s));
        auto lr = std::make_unique<LabelRow>();
        lr->id = "cardFontFamily";
        lr->textValue = a.cardFontFamily.empty() ? "(与主模式相同)" : a.cardFontFamily;
        AddGroupRow(*g, "字体", "", L"\uE70F", std::move(lr));
    }

    AddSection(*this, "卡片颜色", "cardColorH");
    {
        Card* g = AddGroup(*this, "cardColor");
        g->SetVisible(isCard);
        auto cr = std::make_unique<ColorRow>();
        cr->id = "cardCurrentColor";
        cr->textValue = a.cardCurrentColor;
        cr->colorValue = HexToColorF(a.cardCurrentColor);
        AddGroupRow(*g, "普通歌词颜色", "当前行未唱部分与下一行", L"\uE790", std::move(cr));
        cr = std::make_unique<ColorRow>();
        cr->id = "cardHighlightColor";
        const std::string cardHiResolved = a.cardHighlightColor.empty()
            ? a.highlightColor : a.cardHighlightColor;
        cr->textValue = cardHiResolved;
        cr->colorValue = HexToColorF(cardHiResolved);
        AddGroupRow(*g, "高亮歌词颜色", "当前行已唱部分（逐字填充）", L"\uE790", std::move(cr));
    }

    AddSection(*this, "卡片背景", "cardBgH");
    {
        Card* g = AddGroup(*this, "cardBg");
        g->SetVisible(isCard);
        auto dm = std::make_unique<ComboBox>();
        dm->id = "cardBackgroundMode";
        dm->items = {"半透明毛玻璃", "纯透明"};
        dm->selectedIndex = (a.cardBackgroundMode == "transparent") ? 1 : 0;
        AddGroupRow(*g, "背景样式", "", L"\uE73F", std::move(dm));
        auto t = std::make_unique<Toggle>();
        t->id = "cardDynamicWidth";
        t->value = a.cardDynamicWidth;
        AddGroupRow(*g, "歌词显示扩展",
                    "窗口随长歌词自动加宽；关闭后当前行长歌词将滚动显示", L"\uE73E", std::move(t));
    }

    AddSection(*this, "歌词位置", "cardLineOffsetH");
    {
        Card* g = AddGroup(*this, "cardLineOffset");
        g->SetVisible(isCard);
        auto s = std::make_unique<Slider>();
        s->id = "cardLine1OffsetY";
        s->minValue = -100; s->maxValue = 100;
        s->value = static_cast<float>(a.cardLine1OffsetY);
        s->suffix = " px";
        AddGroupRow(*g, "首行歌词偏移", "", L"\uE70F", std::move(s));
        s = std::make_unique<Slider>();
        s->id = "cardLine2OffsetY";
        s->minValue = -100; s->maxValue = 100;
        s->value = static_cast<float>(a.cardLine2OffsetY);
        s->suffix = " px";
        AddGroupRow(*g, "第二行偏移", "", L"\uE70F", std::move(s));
    }

    // ── 恢复默认 ──
    AddSection(*this, "默认设置");
    {
        Card* g = AddGroup(*this);
        auto btn = std::make_unique<Button>();
        btn->id = "resetAppearancePage";
        btn->text = "重置";
        btn->isDanger = true;
        AddGroupRow(*g, "恢复本页默认设置", "仅重置外观参数，保留显示模式", L"\uE72C", std::move(btn));
    }

    // 构建完成时按当前显示模式统一同步组头/分组的可见性（含初次构建）
    UpdateVisibility(a.displayMode);
}

void AppearancePage::UpdateVisibility(const std::string& displayMode) {
    bool isCard = (displayMode == "card");
    // 单行（卡拉OK）模式专属：分组标题 H 后缀 + 分组卡片
    // 注：长歌词滚动（marqueeH/marqueeCard）两种模式通用——卡片模式
    // 关闭"歌词显示扩展"后长歌词也会滚动，因此始终可见
    static const char* kKaraokeIds[] = {
        "karaokeFontH", "karaokeColorH", "themePresetsH",
        "karaokeOffsetH", "singleLineBgH",
        "karaokeFont", "karaokeColor", "themePresets",
        "karaokeOffset", "singleLineBg"};
    // 双行（卡片）模式专属
    static const char* kCardIds[] = {
        "cardFontH", "cardColorH", "cardBgH", "cardLineOffsetH",
        "cardFont", "cardColor", "cardBg", "cardLineOffset"};
    for (auto& child : children_) {
        for (const char* kid : kKaraokeIds) {
            if (child->id == kid) { child->SetVisible(!isCard); break; }
        }
        for (const char* kid : kCardIds) {
            if (child->id == kid) { child->SetVisible(isCard); break; }
        }
        // 封面 / 主题颜色分组始终可见
    }
}

void AppearancePage::UpdateMarqueeVisibility(bool enabled, const std::string& marqueeMode) {
    // 滚动模式下拉始终保留，便于用户在关闭后快速重新开启；
    // 只有实际启用滚动时，延迟/速度参数行才有意义。
    const bool showParameters = enabled && marqueeMode != "off";
    for (auto& group : children_) {
        if (group->id != "marqueeCard") continue;
        for (auto& row : group->Children()) {
            for (auto& c : row->Children()) {
                if (c->id == "marqueeDelay" ||
                    c->id == "marqueeSpeed") {
                    SetRowVisible(c.get(), showParameters);
                }
            }
        }
    }
}

void AppearancePage::CollectChanges(moekoe::Config& cfg) {
    auto& ap = cfg.MutableAppearance();
    for (auto& group : children_) {
        for (auto& row : group->Children()) {
            for (auto& child : row->Children()) {
                if (auto* s = dynamic_cast<Slider*>(child.get())) {
                    if (s->id == "fontSize")           ap.fontSize = static_cast<int>(s->value);
                    else if (s->id == "opacity")       ap.normalOpacity = s->value / 100.f;
                    else if (s->id == "marqueeDelay")  ap.marqueeDelayMs = static_cast<int>(s->value);
                    else if (s->id == "marqueeSpeed")  ap.marqueeSpeedPxPerSec = s->value;
                    else if (s->id == "cardFontSizeCurrent") ap.cardFontSizeCurrent = static_cast<int>(s->value);
                    else if (s->id == "cardFontSizeNext")    ap.cardFontSizeNext = static_cast<int>(s->value);
                    else if (s->id == "coverSize")            ap.coverSize = static_cast<int>(s->value);
                    else if (s->id == "coverOffsetX")       ap.coverOffsetX = static_cast<int>(s->value);
                    else if (s->id == "coverCornerRadius")  ap.coverCornerRadius = static_cast<int>(s->value);
                    else if (s->id == "lyricOffsetY")       ap.lyricOffsetY = static_cast<int>(s->value);
                    else if (s->id == "cardLine1OffsetY")   ap.cardLine1OffsetY = static_cast<int>(s->value);
                    else if (s->id == "cardLine2OffsetY")   ap.cardLine2OffsetY = static_cast<int>(s->value);
                } else if (auto* t = dynamic_cast<Toggle*>(child.get())) {
                    if (t->id == "karaoke")            ap.enableKaraoke = t->value;
                    else if (t->id == "translation")   ap.enableTranslation = t->value;
                    else if (t->id == "marquee")       ap.enableMarquee = t->value;
                    else if (t->id == "cardDynamicWidth") ap.cardDynamicWidth = t->value;
                    else if (t->id == "enableCover")   ap.enableCover = t->value;
                } else if (auto* cb = dynamic_cast<ComboBox*>(child.get())) {
                    if (cb->id == "marqueeMode")
                        ap.marqueeMode = (cb->selectedIndex == 1) ? "off" : "bounce";
                    else if (cb->id == "cardBackgroundMode")
                        ap.cardBackgroundMode = (cb->selectedIndex == 1) ? "transparent" : "frosted";
                    else if (cb->id == "singleLineBackgroundMode")
                        ap.singleLineBackgroundMode = (cb->selectedIndex == 1) ? "transparent" : "frosted";
                    else if (cb->id == "settingsTheme")
                        ap.settingsTheme = (cb->selectedIndex == 0) ? "light" : "dark";
                } else if (auto* cr = dynamic_cast<ColorRow*>(child.get())) {
                    if (cr->id == "normalColor")       ap.normalColor = cr->textValue;
                    else if (cr->id == "highlightColor") ap.highlightColor = cr->textValue;
                    else if (cr->id == "cardCurrentColor") ap.cardCurrentColor = cr->textValue;
                    else if (cr->id == "cardHighlightColor") ap.cardHighlightColor = cr->textValue;
                } else if (auto* lr = dynamic_cast<LabelRow*>(child.get())) {
                    if (lr->id == "fontFamily")        ap.fontFamily = lr->textValue;
                    else if (lr->id == "cardFontFamily") ap.cardFontFamily = lr->textValue;
                } else if (auto* tp = dynamic_cast<ThemePresets*>(child.get())) {
                    if (tp->selectedIndex >= 0 && tp->selectedIndex < static_cast<int>(tp->presets.size())) {
                        ap.highlightColor = ColorFToHex(tp->presets[tp->selectedIndex].hlColor);
                        ap.normalColor = ColorFToHex(tp->presets[tp->selectedIndex].nlColor);
                    }
                }
            }
        }
    }
}

// ═══════════════════════════════════════
// 页面 3：频谱
// ═══════════════════════════════════════

void SpectrumPage::BuildContent(const moekoe::Config& cfg) {
    const auto& a = cfg.Appearance();

    AddSection(*this, "纯音乐显示");
    {
        Card* g = AddGroup(*this);
        auto cb = std::make_unique<ComboBox>();
        cb->id = "spectrumMode";
        cb->items = {"动态频谱条", "\"纯音乐，请欣赏\"文字"};
        cb->selectedIndex = (a.spectrumMode == "text") ? 1 : 0;
        AddGroupRow(*g, "纯音乐时显示",
                    "频谱仅在纯音乐（无歌词）时显示，有歌词时不受影响",
                    L"\uE950", std::move(cb));
    }

    AddSection(*this, "频谱外观");
    {
        Card* g = AddGroup(*this);
        auto cr = std::make_unique<ColorRow>();
        cr->id = "spectrumColor";
        cr->textValue = a.spectrumColor;
        cr->colorValue = HexToColorF(a.spectrumColor);
        AddGroupRow(*g, "频谱条颜色", "", L"\uE790", std::move(cr));
        auto s = std::make_unique<Slider>();
        s->id = "spectrumOpacity";
        s->minValue = 10; s->maxValue = 100;
        s->value = a.spectrumOpacity * 100.0f;
        s->suffix = "%";
        AddGroupRow(*g, "柱透明度", "", L"\uE70F", std::move(s));
    }

    AddSection(*this, "频谱参数");
    {
        Card* g = AddGroup(*this);
        auto s = std::make_unique<Slider>();
        s->id = "spectrumDbCeil";
        s->minValue = -40; s->maxValue = 0;
        s->value = a.spectrumDbCeil;
        s->suffix = " dB";
        AddGroupRow(*g, "柱上限", "", L"\uE70F", std::move(s));
        s = std::make_unique<Slider>();
        s->id = "spectrumDbFloor";
        s->minValue = -90; s->maxValue = -20;
        s->value = a.spectrumDbFloor;
        s->suffix = " dB";
        AddGroupRow(*g, "柱下限", "", L"\uE70F", std::move(s));
        s = std::make_unique<Slider>();
        s->id = "spectrumNumBands";
        s->minValue = 8; s->maxValue = 64;
        s->value = static_cast<float>(a.spectrumNumBands);
        s->suffix = " 根";
        AddGroupRow(*g, "柱数", "", L"\uE70F", std::move(s));
        s = std::make_unique<Slider>();
        s->id = "spectrumBarWidth";
        s->minValue = 0; s->maxValue = 12;
        s->value = a.spectrumBarWidth;
        s->suffix = " dp";
        AddGroupRow(*g, "柱宽", "", L"\uE70F", std::move(s));
        auto btn = std::make_unique<Button>();
        btn->id = "resetSpectrum";
        btn->text = "重置";
        AddGroupRow(*g, "恢复默认频谱参数", "", L"\uE72C", std::move(btn));
    }
    {
        auto hint = std::make_unique<TextBlock>();
        hint->text = "上限越高柱子越容易到顶，下限越低安静段也能显示，柱宽 0 为自动";
        hint->style = TextBlock::Style::Caption;
        AddChild(std::move(hint));
    }
}

void SpectrumPage::CollectChanges(moekoe::Config& cfg) {
    auto& ap = cfg.MutableAppearance();
    for (auto& group : children_) {
        for (auto& row : group->Children()) {
            for (auto& child : row->Children()) {
                if (auto* cb = dynamic_cast<ComboBox*>(child.get())) {
                    if (cb->id == "spectrumMode")
                        ap.spectrumMode = (cb->selectedIndex == 1) ? "text" : "spectrum";
                } else if (auto* cr = dynamic_cast<ColorRow*>(child.get())) {
                    if (cr->id == "spectrumColor")
                        ap.spectrumColor = cr->textValue;
                } else if (auto* s = dynamic_cast<Slider*>(child.get())) {
                    if (s->id == "spectrumOpacity")      ap.spectrumOpacity = s->value / 100.0f;
                    else if (s->id == "spectrumDbCeil")  ap.spectrumDbCeil = s->value;
                    else if (s->id == "spectrumDbFloor") ap.spectrumDbFloor = s->value;
                    else if (s->id == "spectrumNumBands") ap.spectrumNumBands = static_cast<int>(s->value);
                    else if (s->id == "spectrumBarWidth") ap.spectrumBarWidth = s->value;
                }
            }
        }
    }
}

// ═══════════════════════════════════════
// 页面 4：窗口
// ═══════════════════════════════════════

void WindowPage::BuildContent(const moekoe::Config& cfg) {
    // 显示位置（多任务栏选择，仅多显示器时出现）
    if (TaskbarSelector::MonitorCount() > 1) {
        AddSection(*this, "显示位置", "displayPositionCard");
        Card* g = AddGroup(*this, "displayPositionCard");
        {
            auto cb = std::make_unique<ComboBox>();
            cb->id = "displaySelectionMode";
            cb->items = {"自动跟随", "手动指定"};
            cb->selectedIndex = (cfg.Appearance().displaySelectionMode == "manual") ? 1 : 0;
            AddGroupRow(*g, "跟随方式", "多显示器时选择歌词任务栏的跟随方式",
                        L"\uE73F", std::move(cb));
        }
        {
            auto cb = std::make_unique<ComboBox>();
            cb->id = "displayMonitor";
            cb->items = TaskbarSelector::MonitorLabels();
            if (!cb->items.empty()) {
                cb->selectedIndex = std::clamp(cfg.Appearance().manualDisplayIndex,
                                               0, static_cast<int>(cb->items.size()) - 1);
            }
            // 仅"手动指定"模式下显示
            auto* row = AddGroupRow(*g, "指定显示器", "", L"\uE73F", std::move(cb));
            row->SetVisible(cfg.Appearance().displaySelectionMode == "manual");
        }
    }

    AddSection(*this, "位置");
    {
        Card* g = AddGroup(*this);
        auto btn = std::make_unique<Button>();
        btn->id = "resetPos";
        btn->text = "重置位置";
        btn->isDanger = true;
        AddGroupRow(*g, "歌词窗口位置", "拖动歌词窗口可直接调整位置", L"\uE72C", std::move(btn));
    }

    // 窗口尺寸（手动宽度覆盖，解决任务栏空闲区域检测异常导致窗口过窄的问题）
    AddSection(*this, "窗口尺寸");
    {
        Card* g = AddGroup(*this);
        auto s = std::make_unique<Slider>();
        s->id = "windowWidthOverride";
        s->minValue = 0; s->maxValue = 600;
        s->value = static_cast<float>(cfg.Appearance().windowWidthOverride);
        s->suffix = " dp";
        AddGroupRow(*g, "窗口宽度",
                    "自动检测宽度异常（歌词只显示封面和几个字）时可手动指定，0=自动",
                    L"\uE70F", std::move(s));
    }
}

void WindowPage::CollectChanges(moekoe::Config& cfg) {
    auto& ap = cfg.MutableAppearance();
    for (auto& group : children_) {
        for (auto& row : group->Children()) {
            for (auto& child : row->Children()) {
                if (auto* s = dynamic_cast<Slider*>(child.get())) {
                    if (s->id == "windowWidthOverride") ap.windowWidthOverride = static_cast<int>(s->value);
                } else if (auto* cb = dynamic_cast<ComboBox*>(child.get())) {
                    if (cb->id == "displaySelectionMode")
                        ap.displaySelectionMode = (cb->selectedIndex == 1) ? "manual" : "auto";
                    else if (cb->id == "displayMonitor")
                        ap.manualDisplayIndex = cb->selectedIndex;
                }
            }
        }
    }
    // 重置位置通过回调处理，不修改配置字段
}

void WindowPage::UpdateVisibility(const std::string& selectionMode) {
    const bool isManual = (selectionMode == "manual");
    for (auto& group : children_) {
        for (auto& row : group->Children()) {
            for (auto& child : row->Children()) {
                if (auto* cb = dynamic_cast<ComboBox*>(child.get())) {
                    if (cb->id == "displayMonitor") {
                        SetRowVisible(cb, isManual);
                    }
                }
            }
        }
    }
}

// ═══════════════════════════════════════
// 页面 5：行为
// ═══════════════════════════════════════

void BehaviorPage::BuildContent(const moekoe::Config& cfg) {
    const auto& adv = cfg.Advanced();

    AddSection(*this, "启动");
    {
        Card* g = AddGroup(*this);
        auto t = std::make_unique<Toggle>();
        t->id = "autoStart";
        t->value = cfg.IsAutoStart();
        AddGroupRow(*g, "开机自动启动", "", L"\uE73E", std::move(t));
    }

    AddSection(*this, "窗口行为");
    {
        Card* g = AddGroup(*this);
        // 二选一（单个配置位 alwaysShowLyrics：false=绑定 / true=置顶）
        auto cb = std::make_unique<ComboBox>();
        cb->id = "displayBinding";
        cb->items = {"与任务栏绑定", "始终置顶"};
        cb->selectedIndex = adv.alwaysShowLyrics ? 1 : 0;
        AddGroupRow(*g, "歌词显示模式",
                    "与任务栏绑定：全屏或任务栏自动隐藏时歌词跟随隐藏；始终置顶：歌词保持最后可见位置继续显示",
                    L"\uE73F", std::move(cb));
    }
}

void BehaviorPage::CollectChanges(moekoe::Config& cfg) {
    auto& adv = cfg.MutableAdvanced();
    for (auto& group : children_) {
        for (auto& row : group->Children()) {
            for (auto& child : row->Children()) {
                if (auto* t = dynamic_cast<Toggle*>(child.get())) {
                    if (t->id == "autoStart") cfg.SetAutoStart(t->value);
                } else if (auto* cb = dynamic_cast<ComboBox*>(child.get())) {
                    if (cb->id == "displayBinding") adv.alwaysShowLyrics = (cb->selectedIndex == 1);
                }
            }
        }
    }
}

// ═══════════════════════════════════════
// 页面 6：高级
// ═══════════════════════════════════════

void AdvancedPage::BuildContent(const moekoe::Config& cfg) {
    const auto& adv = cfg.Advanced();
    const auto& app = cfg.Appearance();

    AddSection(*this, "高级设置");
    {
        Card* g = AddGroup(*this);
        auto s = std::make_unique<Slider>();
        s->id = "refreshRate";
        s->minValue = 15; s->maxValue = 120;
        s->value = static_cast<float>(adv.refreshRateHz);
        s->suffix = " FPS";
        AddGroupRow(*g, "刷新率", "", L"\uE72C", std::move(s));
        auto t = std::make_unique<Toggle>();
        t->id = "debugLog";
        t->value = adv.debugLog;
        AddGroupRow(*g, "调试日志", "", L"\uE946", std::move(t));
    }

    // 双行歌词扩展显示（仅双行模式可见）
    AddSection(*this, "双行歌词扩展显示", "cardExpandH");
    {
        Card* g = AddGroup(*this, "cardExpand");
        g->SetVisible(app.displayMode == "card");
        auto cb = std::make_unique<ComboBox>();
        cb->id = "cardMaxExpandRatio";
        cb->items = {"默认（1/3）", "50%", "100%"};
        cb->selectedIndex = app.cardMaxExpandRatio;
        AddGroupRow(*g, "最大扩展比例", "", L"\uE756", std::move(cb));
    }

    AddSection(*this, "配置管理");
    {
        Card* g = AddGroup(*this);
        auto btn = std::make_unique<Button>();
        btn->id = "exportConfig";
        btn->text = "导出";
        AddGroupRow(*g, "导出当前配置", "", L"\uE74E", std::move(btn));
        btn = std::make_unique<Button>();
        btn->id = "importConfig";
        btn->text = "导入";
        AddGroupRow(*g, "导入配置", "", L"\uE74E", std::move(btn));
        btn = std::make_unique<Button>();
        btn->id = "resetConfig";
        btn->text = "重置";
        btn->isDanger = true;
        AddGroupRow(*g, "恢复默认", "将所有设置恢复为默认值", L"\uE72C", std::move(btn));
    }

    AddSection(*this, "导出");
    {
        Card* g = AddGroup(*this);
        auto btn = std::make_unique<Button>();
        btn->id = "exportLog";
        btn->text = "导出";
        AddGroupRow(*g, "导出日志", "", L"\uE74E", std::move(btn));
        btn = std::make_unique<Button>();
        btn->id = "exportDiagnostic";
        btn->text = "导出";
        AddGroupRow(*g, "导出诊断信息", "", L"\uE946", std::move(btn));
    }

    // 构建完成时按当前显示模式同步"双行歌词扩展显示"的可见性（含初次构建）
    UpdateVisibility(app.displayMode);
}

void AdvancedPage::UpdateVisibility(const std::string& displayMode) {
    const bool isCard = (displayMode == "card");
    for (auto& child : children_) {
        if (child->id == "cardExpand" || child->id == "cardExpandH") {
            child->SetVisible(isCard);
        }
    }
}

void AdvancedPage::CollectChanges(moekoe::Config& cfg) {
    auto& adv = cfg.MutableAdvanced();
    auto& app = cfg.MutableAppearance();
    for (auto& group : children_) {
        for (auto& row : group->Children()) {
            for (auto& child : row->Children()) {
                if (auto* s = dynamic_cast<Slider*>(child.get())) {
                    if (s->id == "refreshRate") adv.refreshRateHz = static_cast<int>(s->value);
                } else if (auto* t = dynamic_cast<Toggle*>(child.get())) {
                    if (t->id == "debugLog") adv.debugLog = t->value;
                } else if (auto* cb = dynamic_cast<ComboBox*>(child.get())) {
                    if (cb->id == "cardMaxExpandRatio") app.cardMaxExpandRatio = cb->selectedIndex;
                }
            }
        }
    }
}

// ═══════════════════════════════════════
// 页面 7：关于
// ═══════════════════════════════════════

void AboutPage::BuildContent(const moekoe::Config& cfg) {
    auto c1 = MakeCard("");
    {
        auto nameText = std::make_unique<TextBlock>();
        nameText->id = "about_name";
        nameText->text = "TaskbarLyrics";
        nameText->style = TextBlock::Style::Title;
        c1->AddChild(std::move(nameText));
    }
    {
        auto verText = std::make_unique<TextBlock>();
        verText->id = "about_version";
        verText->text = std::string("版本: ") + moekoe::constants::PLUGIN_VERSION;
        verText->style = TextBlock::Style::Body;
        c1->AddChild(std::move(verText));
    }
    {
        auto pluginText = std::make_unique<TextBlock>();
        pluginText->id = "about_plugin";
        pluginText->text = "MoeKoeMusic Plugin";
        pluginText->style = TextBlock::Style::Caption;
        c1->AddChild(std::move(pluginText));
    }
    {
        auto licenseText = std::make_unique<TextBlock>();
        licenseText->id = "about_license";
        licenseText->text = "许可证: Apache License 2.0";
        licenseText->style = TextBlock::Style::Body;
        c1->AddChild(std::move(licenseText));
    }
    {
        auto licenseLink = std::make_unique<TextBlock>();
        licenseLink->id = "about_license_url";
        licenseLink->text = "https://www.apache.org/licenses/LICENSE-2.0";
        licenseLink->style = TextBlock::Style::Caption;
        c1->AddChild(std::move(licenseLink));
    }
    AddChild(std::move(c1));
}

void AboutPage::CollectChanges(moekoe::Config& cfg) {
    // 关于页面不修改配置
}

} // namespace moekoe::ui
