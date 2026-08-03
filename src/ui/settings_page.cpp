// SPDX-License-Identifier: GPL-3.0
// settings_page.cpp - 设置页面实现
#include "ui/settings_page.h"
#include "ui/color_utils.h"
#include "core/constants.h"

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
            h += 12;  // 卡片间距
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
    static auto getFactory = []() {
        static Microsoft::WRL::ComPtr<IDWriteFactory> f;
        if (!f) ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(f.GetAddressOf()));
        return f;
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

    // 子元素（Card 等），传递 DrawContext
    for (auto& child : children_) {
        if (child->IsVisible()) child->Draw(rt, ctx);
    }
}

// ═══════════════════════════════════════
// 辅助：创建标准 Card
// ═══════════════════════════════════════

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

    // Card: 显示内容
    auto c1 = MakeCard("显示内容");
    {
        auto dm = std::make_unique<ComboBox>();
        dm->id = "displayMode";
        dm->label = "显示模式";
        dm->items = {"单行歌词", "双行歌词"};
        dm->selectedIndex = isCard ? 1 : 0;
        c1->AddChild(std::move(dm));
    }
    {
        auto t = std::make_unique<Toggle>();
        t->id = "karaoke";
        t->label = "逐字高亮";
        t->value = a.enableKaraoke;
        t->SetVisible(!isCard);
        c1->AddChild(std::move(t));
    }
    AddChild(std::move(c1));

    // Card: 翻译模式
    auto c2 = MakeCard("翻译模式");
    {
        auto dm = std::make_unique<ComboBox>();
        dm->id = "translationMode";
        dm->label = "翻译模式";
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
        c2->AddChild(std::move(dm));
    }
    AddChild(std::move(c2));
}

void LyricsPage::CollectChanges(moekoe::Config& cfg) {
    auto& ap = cfg.MutableAppearance();
    for (auto& card : children_) {
        for (auto& child : card->Children()) {
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
            }
        }
    }
}

void LyricsPage::UpdateForDisplayMode(const std::string& displayMode, const moekoe::Config& cfg) {
    bool isCard = (displayMode == "card");
    const auto& a = cfg.Appearance();

    for (auto& card : children_) {
        for (auto& child : card->Children()) {
            // 更新"卡拉OK效果"可见性
            if (auto* t = dynamic_cast<Toggle*>(child.get())) {
                if (t->id == "karaoke") {
                    t->SetVisible(!isCard);
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

// ═══════════════════════════════════════
// 页面 2：外观
// ═══════════════════════════════════════

void AppearancePage::BuildContent(const moekoe::Config& cfg) {
    const auto& a = cfg.Appearance();
    bool isCard = (a.displayMode == "card");

    // ── 通用选项（两种模式都可见）──

    // Card: 封面
    auto c0 = MakeCard("封面");
    c0->id = "coverCard";
    {
        auto t = std::make_unique<Toggle>();
        t->id = "enableCover";
        t->label = "显示专辑封面";
        t->value = a.enableCover;
        c0->AddChild(std::move(t));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "coverSize";
        s->label = "封面尺寸";
        s->minValue = 16; s->maxValue = 80;
        s->value = static_cast<float>(a.coverSize);
        s->suffix = " dp";
        c0->AddChild(std::move(s));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "coverOffsetX";
        s->label = "左右偏移";
        s->minValue = -50; s->maxValue = 50;
        s->value = static_cast<float>(a.coverOffsetX);
        s->suffix = " dp";
        c0->AddChild(std::move(s));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "coverCornerRadius";
        s->label = "封面圆角";
        s->minValue = 0; s->maxValue = 100;
        s->value = static_cast<float>(a.coverCornerRadius);
        s->suffix = "%";
        c0->AddChild(std::move(s));
    }
    AddChild(std::move(c0));

    // Card: 主题颜色
    auto cTheme = MakeCard("主题颜色");
    cTheme->id = "themeColorCard";
    {
        auto dm = std::make_unique<ComboBox>();
        dm->id = "settingsTheme";
        dm->label = "设置界面主题";
        dm->items = {"蓝（默认）", "深色", "浅色", "白色"};
        dm->selectedIndex = (a.settingsTheme == "dark")  ? 1 :
                            (a.settingsTheme == "light") ? 2 :
                            (a.settingsTheme == "white") ? 3 : 0;
        cTheme->AddChild(std::move(dm));
    }
    AddChild(std::move(cTheme));

    // ── 单行歌词区域 ──

    // Card: 字体
    auto c1 = MakeCard("字体");
    c1->id = "karaokeFont";
    {
        auto s = std::make_unique<Slider>();
        s->id = "fontSize";
        s->label = "字号";
        s->minValue = 10; s->maxValue = 28;
        s->value = static_cast<float>(a.fontSize);
        s->suffix = "";
        c1->AddChild(std::move(s));
    }
    {
        auto lr = std::make_unique<LabelRow>();
        lr->id = "fontFamily";
        lr->label = "字体";
        lr->textValue = a.fontFamily;
        c1->AddChild(std::move(lr));
    }
    c1->SetVisible(!isCard);
    AddChild(std::move(c1));

    // Card: 颜色
    auto c2 = MakeCard("颜色");
    c2->id = "karaokeColor";
    {
        auto cr = std::make_unique<ColorRow>();
        cr->id = "normalColor";
        cr->label = "普通歌词颜色";
        cr->textValue = a.normalColor;
        cr->colorValue = HexToColorF(a.normalColor);
        c2->AddChild(std::move(cr));
    }
    {
        auto cr = std::make_unique<ColorRow>();
        cr->id = "highlightColor";
        cr->label = "高亮歌词颜色";
        cr->textValue = a.highlightColor;
        cr->colorValue = HexToColorF(a.highlightColor);
        c2->AddChild(std::move(cr));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "opacity";
        s->label = "不透明度";
        s->minValue = 20; s->maxValue = 100;
        s->value = static_cast<float>(a.normalOpacity * 100.0);
        s->suffix = "%";
        c2->AddChild(std::move(s));
    }
    c2->SetVisible(!isCard);
    AddChild(std::move(c2));

    // Card: 长歌词滚动（跑马灯）
    auto c3 = MakeCard("长歌词滚动（跑马灯）");
    c3->id = "marqueeCard";
    {
        auto t = std::make_unique<Toggle>();
        t->id = "marquee";
        t->label = "启用跑马灯";
        t->value = a.enableMarquee;
        c3->AddChild(std::move(t));
    }
    {
        auto dm = std::make_unique<ComboBox>();
        dm->id = "marqueeMode";
        dm->label = "滚动模式";
        dm->items = {"往返滚动（推荐）", "循环跑马灯", "关闭（截断显示）"};
        dm->selectedIndex = (a.marqueeMode == "loop") ? 1 : (a.marqueeMode == "off" ? 2 : 0);
        c3->AddChild(std::move(dm));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "marqueeDelay";
        s->label = "开始延迟";
        s->minValue = 0; s->maxValue = 5000;
        s->value = static_cast<float>(a.marqueeDelayMs);
        s->suffix = " ms";
        c3->AddChild(std::move(s));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "marqueePause";
        s->label = "端点暂停";
        s->minValue = 0; s->maxValue = 3000;
        s->value = static_cast<float>(a.marqueePauseMs);
        s->suffix = " ms";
        c3->AddChild(std::move(s));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "marqueeSpeed";
        s->label = "滚动速度";
        s->minValue = 10; s->maxValue = 200;
        s->value = a.marqueeSpeedPxPerSec;
        s->suffix = " px/s";
        c3->AddChild(std::move(s));
    }
    c3->SetVisible(!isCard);
    AddChild(std::move(c3));

    // Card: 歌词位置（仅单行）
    auto cLyricPos = MakeCard("歌词位置");
    cLyricPos->id = "karaokeOffset";
    {
        auto s = std::make_unique<Slider>();
        s->id = "lyricOffsetY";
        s->label = "上下偏移";
        s->minValue = -100; s->maxValue = 100;
        s->value = static_cast<float>(a.lyricOffsetY);
        s->suffix = " px";
        cLyricPos->AddChild(std::move(s));
    }
    cLyricPos->SetVisible(!isCard);
    AddChild(std::move(cLyricPos));

    // Card: 单行背景（仅单行模式可见）
    auto cSingleBg = MakeCard("单行背景");
    cSingleBg->id = "singleLineBg";
    {
        auto dm = std::make_unique<ComboBox>();
        dm->id = "singleLineBackgroundMode";
        dm->label = "单行背景";
        dm->items = {"半透明毛玻璃", "纯透明"};
        dm->selectedIndex = (a.singleLineBackgroundMode == "transparent") ? 1 : 0;
        cSingleBg->AddChild(std::move(dm));
    }
    cSingleBg->SetVisible(!isCard);
    AddChild(std::move(cSingleBg));

    // Card: 预设主题
    auto c4 = MakeCard("预设主题");
    c4->id = "themePresets";
    {
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
        c4->AddChild(std::move(tp));
    }
    c4->SetVisible(!isCard);
    AddChild(std::move(c4));

    // ── 双行歌词区域 ──

    // Card: 卡片字体
    auto c5 = MakeCard("卡片字体");
    c5->id = "cardFont";
    {
        auto s = std::make_unique<Slider>();
        s->id = "cardFontSizeCurrent";
        s->label = "当前行字号";
        s->minValue = 10; s->maxValue = 20;
        s->value = static_cast<float>(a.cardFontSizeCurrent);
        s->suffix = "";
        c5->AddChild(std::move(s));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "cardFontSizeNext";
        s->label = "下一行字号";
        s->minValue = 8; s->maxValue = 18;
        s->value = static_cast<float>(a.cardFontSizeNext);
        s->suffix = "";
        c5->AddChild(std::move(s));
    }
    {
        auto lr = std::make_unique<LabelRow>();
        lr->id = "cardFontFamily";
        lr->label = "字体";
        lr->textValue = a.cardFontFamily.empty() ? "(与主模式相同)" : a.cardFontFamily;
        c5->AddChild(std::move(lr));
    }
    c5->SetVisible(isCard);
    AddChild(std::move(c5));

    // Card: 双行颜色
    auto c6 = MakeCard("双行颜色");
    c6->id = "cardColor";
    {
        auto cr = std::make_unique<ColorRow>();
        cr->id = "cardCurrentColor";
        cr->label = "当前行颜色";
        cr->textValue = a.cardCurrentColor;
        cr->colorValue = HexToColorF(a.cardCurrentColor);
        c6->AddChild(std::move(cr));
    }
    {
        auto cr = std::make_unique<ColorRow>();
        cr->id = "cardNextColor";
        cr->label = "下一行颜色";
        cr->textValue = a.cardNextColor;
        cr->colorValue = HexToColorF(a.cardNextColor);
        c6->AddChild(std::move(cr));
    }
    c6->SetVisible(isCard);
    AddChild(std::move(c6));

    // Card: 双行背景
    auto c7 = MakeCard("双行背景");
    c7->id = "cardBg";
    {
        auto dm = std::make_unique<ComboBox>();
        dm->id = "cardBackgroundMode";
        dm->label = "双行背景";
        dm->items = {"半透明毛玻璃", "纯透明"};
        dm->selectedIndex = (a.cardBackgroundMode == "transparent") ? 1 : 0;
        c7->AddChild(std::move(dm));
    }
    {
        auto t = std::make_unique<Toggle>();
        t->id = "cardDynamicWidth";
        t->label = "歌词显示扩展";
        t->value = a.cardDynamicWidth;
        c7->AddChild(std::move(t));
    }
    c7->SetVisible(isCard);
    AddChild(std::move(c7));

    // Card: 歌词位置（仅双行）
    auto cCardOffset = MakeCard("歌词位置");
    cCardOffset->id = "cardLineOffset";
    {
        auto s = std::make_unique<Slider>();
        s->id = "cardLine1OffsetY";
        s->label = "首行歌词偏移";
        s->minValue = -100; s->maxValue = 100;
        s->value = static_cast<float>(a.cardLine1OffsetY);
        s->suffix = " px";
        cCardOffset->AddChild(std::move(s));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "cardLine2OffsetY";
        s->label = "第二行偏移";
        s->minValue = -100; s->maxValue = 100;
        s->value = static_cast<float>(a.cardLine2OffsetY);
        s->suffix = " px";
        cCardOffset->AddChild(std::move(s));
    }
    cCardOffset->SetVisible(isCard);
    AddChild(std::move(cCardOffset));
}

void AppearancePage::UpdateVisibility(const std::string& displayMode) {
    bool isCard = (displayMode == "card");
    for (auto& child : children_) {
        const std::string& cid = child->id;
        if (cid == "karaokeFont" || cid == "karaokeColor" ||
            cid == "marqueeCard" || cid == "themePresets" ||
            cid == "karaokeOffset" || cid == "singleLineBg") {
            child->SetVisible(!isCard);
        } else if (cid == "cardFont" || cid == "cardColor" || cid == "cardBg" ||
                   cid == "cardLineOffset") {
            child->SetVisible(isCard);
        }
        // coverCard 和 themeColorCard 始终可见，无需切换
    }
}

void AppearancePage::CollectChanges(moekoe::Config& cfg) {
    auto& ap = cfg.MutableAppearance();
    for (auto& card : children_) {
        for (auto& child : card->Children()) {
            if (auto* s = dynamic_cast<Slider*>(child.get())) {
                if (s->id == "fontSize")           ap.fontSize = static_cast<int>(s->value);
                else if (s->id == "opacity")       ap.normalOpacity = s->value / 100.f;
                else if (s->id == "marqueeDelay")  ap.marqueeDelayMs = static_cast<int>(s->value);
                else if (s->id == "marqueePause")  ap.marqueePauseMs = static_cast<int>(s->value);
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
                    ap.marqueeMode = (cb->selectedIndex == 1) ? "loop" :
                                     (cb->selectedIndex == 2) ? "off" : "bounce";
                else if (cb->id == "cardBackgroundMode")
                    ap.cardBackgroundMode = (cb->selectedIndex == 1) ? "transparent" : "frosted";
                else if (cb->id == "singleLineBackgroundMode")
                    ap.singleLineBackgroundMode = (cb->selectedIndex == 1) ? "transparent" : "frosted";
                else if (cb->id == "settingsTheme")
                    ap.settingsTheme = (cb->selectedIndex == 1) ? "dark" :
                                       (cb->selectedIndex == 2) ? "light" :
                                       (cb->selectedIndex == 3) ? "white" : "blue";
            } else if (auto* cr = dynamic_cast<ColorRow*>(child.get())) {
                if (cr->id == "normalColor")       ap.normalColor = cr->textValue;
                else if (cr->id == "highlightColor") ap.highlightColor = cr->textValue;
                else if (cr->id == "cardCurrentColor") ap.cardCurrentColor = cr->textValue;
                else if (cr->id == "cardNextColor")    ap.cardNextColor = cr->textValue;
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

// ═══════════════════════════════════════
// 页面 3：窗口
// ═══════════════════════════════════════

void WindowPage::BuildContent(const moekoe::Config& cfg) {
    auto c1 = MakeCard("位置");
    {
        auto hint = std::make_unique<TextBlock>();
        hint->id = "posHint";
        hint->text = "拖动歌词窗口可直接调整位置";
        hint->style = TextBlock::Style::Caption;
        c1->AddChild(std::move(hint));
    }
    {
        auto btn = std::make_unique<Button>();
        btn->id = "resetPos";
        btn->text = "重置位置";
        btn->isDanger = true;
        c1->AddChild(std::move(btn));
    }
    AddChild(std::move(c1));
}

void WindowPage::CollectChanges(moekoe::Config& cfg) {
    // 重置位置通过回调处理，不修改配置字段
}

// ═══════════════════════════════════════
// 页面 4：行为
// ═══════════════════════════════════════

void BehaviorPage::BuildContent(const moekoe::Config& cfg) {
    const auto& adv = cfg.Advanced();

    // Card: 启动
    auto c1 = MakeCard("启动");
    {
        auto t = std::make_unique<Toggle>();
        t->id = "autoStart";
        t->label = "开机自动启动";
        t->value = cfg.IsAutoStart();
        c1->AddChild(std::move(t));
    }
    AddChild(std::move(c1));

    // Card: 窗口行为
    auto c2 = MakeCard("窗口行为");
    {
        auto t = std::make_unique<Toggle>();
        t->id = "enableFullscreenHide";
        t->label = "全屏时自动隐藏";
        t->value = adv.enableFullscreenHide;
        c2->AddChild(std::move(t));
    }
    AddChild(std::move(c2));
}

void BehaviorPage::CollectChanges(moekoe::Config& cfg) {
    auto& adv = cfg.MutableAdvanced();
    for (auto& card : children_) {
        for (auto& child : card->Children()) {
            if (auto* t = dynamic_cast<Toggle*>(child.get())) {
                if (t->id == "autoStart")           cfg.SetAutoStart(t->value);
                else if (t->id == "enableFullscreenHide") adv.enableFullscreenHide = t->value;
            }
        }
    }
}

// ═══════════════════════════════════════
// 页面 5：高级
// ═══════════════════════════════════════

void AdvancedPage::BuildContent(const moekoe::Config& cfg) {
    const auto& adv = cfg.Advanced();

    auto c1 = MakeCard("高级设置");
    {
        auto hint = std::make_unique<TextBlock>();
        hint->id = "advHint";
        hint->text = "⚠ 修改端口可能影响使用";
        hint->style = TextBlock::Style::Caption;
        c1->AddChild(std::move(hint));
    }
    {
        auto lr = std::make_unique<LabelRow>();
        lr->id = "wsPort";
        lr->label = "WebSocket 端口";
        lr->textValue = std::to_string(adv.websocketPort);
        lr->readOnly = false;
        c1->AddChild(std::move(lr));
    }
    {
        auto btn = std::make_unique<Button>();
        btn->id = "resetPort";
        btn->text = "重置端口";
        c1->AddChild(std::move(btn));
    }
    {
        auto s = std::make_unique<Slider>();
        s->id = "refreshRate";
        s->label = "刷新率";
        s->minValue = 15; s->maxValue = 120;
        s->value = static_cast<float>(adv.refreshRateHz);
        s->suffix = " FPS";
        c1->AddChild(std::move(s));
    }
    {
        auto t = std::make_unique<Toggle>();
        t->id = "debugLog";
        t->label = "调试日志";
        t->value = adv.debugLog;
        c1->AddChild(std::move(t));
    }
    AddChild(std::move(c1));

    // Card: 导出
    auto c2 = MakeCard("导出");
    {
        auto btn = std::make_unique<Button>();
        btn->id = "exportLog";
        btn->text = "导出日志";
        c2->AddChild(std::move(btn));
    }
    {
        auto btn = std::make_unique<Button>();
        btn->id = "exportDiagnostic";
        btn->text = "导出诊断信息";
        c2->AddChild(std::move(btn));
    }
    AddChild(std::move(c2));
}

void AdvancedPage::CollectChanges(moekoe::Config& cfg) {
    auto& adv = cfg.MutableAdvanced();
    for (auto& card : children_) {
        for (auto& child : card->Children()) {
            if (auto* lr = dynamic_cast<LabelRow*>(child.get())) {
                if (lr->id == "wsPort") adv.websocketPort = atoi(lr->textValue.c_str());
            } else if (auto* s = dynamic_cast<Slider*>(child.get())) {
                if (s->id == "refreshRate") adv.refreshRateHz = static_cast<int>(s->value);
            } else if (auto* t = dynamic_cast<Toggle*>(child.get())) {
                if (t->id == "debugLog") adv.debugLog = t->value;
            }
        }
    }
}

// ═══════════════════════════════════════
// 页面 6：关于
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
    AddChild(std::move(c1));
}

void AboutPage::CollectChanges(moekoe::Config& cfg) {
    // 关于页面不修改配置
}

} // namespace moekoe::ui
