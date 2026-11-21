// SPDX-License-Identifier: Apache-2.0
// font_picker.cpp - 设置界面内置字体选择器实现
#include "ui/font_picker.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace moekoe {

namespace {

// ── 画刷/文本格式辅助（与 ui_elements.cpp 同风格） ──

Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> MakeBrush(ID2D1RenderTarget* rt, const D2D1_COLOR_F& c) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
    rt->CreateSolidColorBrush(c, b.GetAddressOf());
    return b;
}

// 共享 DWrite 工厂（进程内仅创建一次；此前每创建一个字体格式都新建工厂，
// 导致滚动到新字体时 UI 线程卡顿，是滑动卡顿的主因）
Microsoft::WRL::ComPtr<IDWriteFactory> SharedFactory() {
    static Microsoft::WRL::ComPtr<IDWriteFactory> f = [] {
        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
        return factory;
    }();
    return f;
}

Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeTextFormat(const wchar_t* family, float size,
                                                         DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    SharedFactory()->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"zh-Hans", fmt.GetAddressOf());
    return fmt;
}

// 每个字体族一个 TextFormat（跨帧缓存，键为字体族名）
Microsoft::WRL::ComPtr<IDWriteTextFormat> FamilyFormat(const std::wstring& family, float size) {
    static std::map<std::wstring, Microsoft::WRL::ComPtr<IDWriteTextFormat>> cache;
    auto it = cache.find(family);
    if (it != cache.end()) return it->second;
    auto fmt = MakeTextFormat(family.c_str(), size);
    // 创建失败时缓存空指针，避免每帧重试（绘制时回退 Segoe UI）
    auto [ins, _] = cache.emplace(family, std::move(fmt));
    return ins->second;
}

// 字符串转小写（用于大小写无关搜索，兼顾 ASCII；中文不受影响）
std::wstring ToLower(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    }
    return s;
}

constexpr float kRowH = 40.0f;       // 列表行高（DIP）
constexpr int   kWheelStep = 120;    // 滚轮一格的滚动量

} // namespace

// ═══════════════════════════════════════
// 系统字体族枚举
// ═══════════════════════════════════════

const std::vector<std::wstring>& FontPickerPopup::SystemFamilies() {
    static std::vector<std::wstring> families = [] {
        std::vector<std::wstring> out;
        Microsoft::WRL::ComPtr<IDWriteFactory> f = SharedFactory();
        if (!f) return out;
        Microsoft::WRL::ComPtr<IDWriteFontCollection> col;
        if (FAILED(f->GetSystemFontCollection(col.GetAddressOf(), FALSE))) return out;

        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        ::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

        const UINT32 count = col->GetFontFamilyCount();
        out.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            Microsoft::WRL::ComPtr<IDWriteFontFamily> ff;
            if (FAILED(col->GetFontFamily(i, ff.GetAddressOf()))) continue;
            Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
            if (FAILED(ff->GetFamilyNames(names.GetAddressOf()))) continue;

            UINT32 idx = 0;
            BOOL exists = FALSE;
            names->FindLocaleName(locale, &idx, &exists);
            if (!exists) names->FindLocaleName(L"en-us", &idx, &exists);
            if (!exists) idx = 0;

            UINT32 len = 0;
            if (FAILED(names->GetStringLength(idx, &len))) continue;
            ++len;  // 含结尾 NUL
            std::wstring name(len, L'\0');
            if (FAILED(names->GetString(idx, name.data(), len))) continue;
            name.resize(len - 1);  // 去掉结尾 NUL
            if (!name.empty()) out.push_back(std::move(name));
        }
        return out;
    }();
    return families;
}

// ═══════════════════════════════════════
// 过滤 / 几何
// ═══════════════════════════════════════

void FontPickerPopup::UpdateFilter() {
    const auto& families = SystemFamilies();
    if (searchText_.empty()) {
        filtered_.resize(families.size());
        for (size_t i = 0; i < families.size(); ++i) filtered_[i] = static_cast<int>(i);
    } else {
        const std::wstring needle = ToLower(searchText_);
        filtered_.clear();
        for (size_t i = 0; i < families.size(); ++i) {
            if (ToLower(families[i]).find(needle) != std::wstring::npos) {
                filtered_.push_back(static_cast<int>(i));
            }
        }
    }
    contentHeight_ = static_cast<int>(filtered_.size() * kRowH);
    scroll_ = (std::clamp)(scroll_, 0, MaxScroll());
}

void FontPickerPopup::ClientSize(int* clientW, int* clientH) const {
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    *clientW = static_cast<int>(rc.right / dpiScale_);
    *clientH = static_cast<int>(rc.bottom / dpiScale_);
}

int FontPickerPopup::MaxScroll() const {
    const int listH = listRect_.bottom - listRect_.top;
    return (std::max)(0, contentHeight_ - listH);
}

void FontPickerPopup::ComputeRects(int clientW, int clientH) {
    constexpr int kW = 460, kH = 520;
    const int titleBarHeight = titleBarHeight_;

    int px = (clientW - kW) / 2;
    int py = titleBarHeight + (clientH - titleBarHeight - kH) / 2;
    if (px < 8) px = 8;
    if (py < titleBarHeight + 8) py = titleBarHeight + 8;
    panelRect_  = {px, py, px + kW, py + kH};
    closeRect_  = {px + kW - 34, py + 6, px + kW - 6, py + 34};
    searchRect_ = {px + 16, py + 40, px + kW - 16, py + 68};
    listRect_   = {px + 16, py + 76, px + kW - 16, py + kH - 56};
    cancelRect_ = {px + kW - 16 - 80, py + kH - 42, px + kW - 16, py + kH - 12};
    okRect_     = {cancelRect_.left - 92, cancelRect_.top, cancelRect_.left - 12, cancelRect_.bottom};

    contentHeight_ = static_cast<int>(filtered_.size() * kRowH);
    scroll_ = (std::clamp)(scroll_, 0, MaxScroll());
}

// ═══════════════════════════════════════
// 生命周期
// ═══════════════════════════════════════

void FontPickerPopup::Activate(HWND hwnd, const std::wstring& currentFamily,
                               int titleBarHeight, float dpiScale) {
    hwnd_ = hwnd;
    dpiScale_ = dpiScale > 0.0f ? dpiScale : 1.0f;
    titleBarHeight_ = titleBarHeight;

    searchText_.clear();
    searchFocused_ = false;
    UpdateFilter();

    selected_ = -1;
    const auto& families = SystemFamilies();
    for (size_t i = 0; i < families.size(); ++i) {
        if (_wcsicmp(families[i].c_str(), currentFamily.c_str()) == 0) {
            selected_ = static_cast<int>(i);
            break;
        }
    }
    // 初始滚动：让选中行大致居中
    int cw = 0, ch = 0;
    ClientSize(&cw, &ch);
    ComputeRects(cw, ch);
    if (selected_ >= 0) {
        // 在过滤结果中找到选中项的行位置
        for (size_t r = 0; r < filtered_.size(); ++r) {
            if (filtered_[r] == selected_) {
                const int listH = listRect_.bottom - listRect_.top;
                const int target = static_cast<int>(r * kRowH - listH / 2.0f);
                scroll_ = (std::clamp)(target, 0, MaxScroll());
                break;
            }
        }
    }
    hover_ = -1;
    hoverOk_ = hoverCancel_ = hoverClose_ = hoverSearch_ = false;
    active_ = true;
}

void FontPickerPopup::Deactivate(HWND) {
    active_ = false;
    hwnd_ = nullptr;
    hover_ = -1;
    scroll_ = 0;
    searchText_.clear();
    searchFocused_ = false;
}

// ═══════════════════════════════════════
// 交互
// ═══════════════════════════════════════

FontPickerPopup::ActionResult FontPickerPopup::HandleMouseDown(int x, int y, std::wstring* outFamily) {
    int cw = 0, ch = 0;
    ClientSize(&cw, &ch);
    ComputeRects(cw, ch);

    if (PtInRect(&closeRect_, {x, y})) return ActionResult::Cancelled;
    if (PtInRect(&cancelRect_, {x, y})) return ActionResult::Cancelled;
    if (PtInRect(&okRect_, {x, y})) {
        if (GetSelected(outFamily)) return ActionResult::Confirmed;
        return ActionResult::Cancelled;  // 无有效选择时视为取消
    }
    if (PtInRect(&searchRect_, {x, y})) {
        searchFocused_ = true;
        return ActionResult::None;
    }
    searchFocused_ = false;

    if (PtInRect(&listRect_, {x, y})) {
        // 行位置与绘制对齐：第 i 行绘制在 [top + i*kRowH - scroll, +kRowH)
        const int rowPos = (y - listRect_.top + scroll_) / static_cast<int>(kRowH);
        if (rowPos >= 0 && rowPos < static_cast<int>(filtered_.size())) {
            selected_ = filtered_[rowPos];
        }
    }
    return ActionResult::None;
}

bool FontPickerPopup::HandleMouseMove(int x, int y) {
    int cw = 0, ch = 0;
    ClientSize(&cw, &ch);
    ComputeRects(cw, ch);

    const bool hOk     = PtInRect(&okRect_, {x, y}) != 0;
    const bool hCancel = PtInRect(&cancelRect_, {x, y}) != 0;
    const bool hClose  = PtInRect(&closeRect_, {x, y}) != 0;
    const bool hSearch = PtInRect(&searchRect_, {x, y}) != 0;

    int hRow = -1;
    if (PtInRect(&listRect_, {x, y})) {
        const int rowPos = (y - listRect_.top + scroll_) / static_cast<int>(kRowH);
        if (rowPos >= 0 && rowPos < static_cast<int>(filtered_.size())) {
            hRow = filtered_[rowPos];
        }
    }

    const bool changed = (hOk != hoverOk_) || (hCancel != hoverCancel_) ||
                         (hClose != hoverClose_) || (hSearch != hoverSearch_) ||
                         (hRow != hover_);
    hoverOk_ = hOk;
    hoverCancel_ = hCancel;
    hoverClose_ = hClose;
    hoverSearch_ = hSearch;
    hover_ = hRow;
    return changed;
}

void FontPickerPopup::HandleWheel(int delta) {
    if (listRect_.bottom <= listRect_.top) return;
    if (MaxScroll() <= 0) return;
    scroll_ = (std::clamp)(scroll_ - delta / WHEEL_DELTA * kWheelStep, 0, MaxScroll());
}

FontPickerPopup::KeyResult FontPickerPopup::HandleKey(UINT msg, WPARAM wParam) {
    if (msg == WM_KEYDOWN) {
        switch (wParam) {
        case VK_ESCAPE:
            return KeyResult::Cancel;
        case VK_RETURN:
            return GetSelected(nullptr) ? KeyResult::Confirm : KeyResult::None;
        case VK_BACK:
            if (!searchText_.empty()) {
                searchText_.pop_back();
                UpdateFilter();
                return KeyResult::Redraw;
            }
            return KeyResult::None;
        case VK_UP:
        case VK_DOWN: {
            if (filtered_.empty()) return KeyResult::None;
            // 在过滤结果中移动选中项
            int rowPos = 0;
            for (size_t r = 0; r < filtered_.size(); ++r) {
                if (filtered_[r] == selected_) { rowPos = static_cast<int>(r); break; }
            }
            rowPos += (wParam == VK_DOWN) ? 1 : -1;
            rowPos = (std::clamp)(rowPos, 0, static_cast<int>(filtered_.size()) - 1);
            selected_ = filtered_[rowPos];
            // 保证选中行可见
            const int listH = listRect_.bottom - listRect_.top;
            const int rowTop = static_cast<int>(rowPos * kRowH);
            if (rowTop < scroll_) scroll_ = rowTop;
            if (rowTop + static_cast<int>(kRowH) > scroll_ + listH) {
                scroll_ = (std::min)(MaxScroll(), rowTop + static_cast<int>(kRowH) - listH);
            }
            return KeyResult::Redraw;
        }
        default:
            return KeyResult::None;
        }
    }
    if (msg == WM_CHAR) {
        const wchar_t c = static_cast<wchar_t>(wParam);
        if (c >= 0x20) {  // 可打印字符（含中文）；控制字符忽略
            if (!searchFocused_) searchFocused_ = true;  // 直接打字自动聚焦搜索框
            searchText_ += c;
            UpdateFilter();
            return KeyResult::Redraw;
        }
        return KeyResult::None;
    }
    return KeyResult::None;
}

bool FontPickerPopup::GetSelected(std::wstring* outFamily) const {
    if (selected_ < 0 || selected_ >= static_cast<int>(SystemFamilies().size())) return false;
    if (outFamily) *outFamily = SystemFamilies()[selected_];
    return true;
}

// ═══════════════════════════════════════
// 绘制
// ═══════════════════════════════════════

void FontPickerPopup::Draw(ID2D1RenderTarget* rt, const D2DThemeColors& theme,
                           ID2D1SolidColorBrush* textSecondaryBrush, int titleBarHeight) {
    if (!active_) return;
    titleBarHeight_ = titleBarHeight;  // 与窗口缩放/布局保持同步

    // rt 尺寸即客户区（DIP，HwndRenderTarget 已按 DPI 变换）
    const D2D1_SIZE_F sz = rt->GetSize();
    const int clientW = static_cast<int>(sz.width);
    const int clientH = static_cast<int>(sz.height);
    ComputeRects(clientW, clientH);
    if (clientW <= 0 || clientH <= 0) return;

    // 1) 全屏遮罩
    auto dimBrush = MakeBrush(rt, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f));
    rt->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), dimBrush.Get());

    // 2) 面板
    auto panelBrush = MakeBrush(rt, theme.surface);
    auto borderBrush = MakeBrush(rt, theme.border);
    const auto panel = D2D1::RectF(static_cast<float>(panelRect_.left), static_cast<float>(panelRect_.top),
                                   static_cast<float>(panelRect_.right), static_cast<float>(panelRect_.bottom));
    rt->FillRoundedRectangle(D2D1::RoundedRect(panel, 12, 12), panelBrush.Get());
    rt->DrawRoundedRectangle(D2D1::RoundedRect(panel, 12, 12), borderBrush.Get(), 1.0f);

    // 3) 标题 + 关闭 ×
    if (auto titleFmt = MakeTextFormat(L"Segoe UI Variable", 15, DWRITE_FONT_WEIGHT_SEMI_BOLD)) {
        auto textBrush = MakeBrush(rt, theme.text);
        rt->DrawTextW(L"选择字体", 4, titleFmt.Get(),
                      D2D1::RectF(panel.left + 16, panel.top + 6, panel.right - 40, panel.top + 34),
                      textBrush.Get());
    }
    {
        auto xBrush = MakeBrush(rt, hoverClose_ ? theme.text : theme.textSecondary);
        const float cx = (closeRect_.left + closeRect_.right) / 2.0f;
        const float cy = (closeRect_.top + closeRect_.bottom) / 2.0f;
        rt->DrawLine(D2D1::Point2F(cx - 5, cy - 5), D2D1::Point2F(cx + 5, cy + 5), xBrush.Get(), 1.6f);
        rt->DrawLine(D2D1::Point2F(cx + 5, cy - 5), D2D1::Point2F(cx - 5, cy + 5), xBrush.Get(), 1.6f);
    }

    // 4) 搜索框
    {
        const auto sr = D2D1::RectF(static_cast<float>(searchRect_.left), static_cast<float>(searchRect_.top),
                                    static_cast<float>(searchRect_.right), static_cast<float>(searchRect_.bottom));
        auto bgBrush = MakeBrush(rt, theme.bg);
        rt->FillRoundedRectangle(D2D1::RoundedRect(sr, 6, 6), bgBrush.Get());
        auto bd = MakeBrush(rt, searchFocused_ ? theme.accent : theme.border);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(sr, 6, 6), bd.Get(), searchFocused_ ? 1.6f : 1.0f);

        if (auto fmt = MakeTextFormat(L"Segoe UI Variable", 13)) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const float pad = 10.0f;
            if (searchText_.empty()) {
                auto ph = MakeBrush(rt, theme.textSecondary);
                rt->DrawTextW(L"搜索字体…", 5, fmt.Get(),
                              D2D1::RectF(sr.left + pad, sr.top, sr.right - pad, sr.bottom), ph.Get());
            } else {
                auto t = MakeBrush(rt, theme.text);
                rt->DrawTextW(searchText_.c_str(), static_cast<UINT32>(searchText_.size()),
                              fmt.Get(),
                              D2D1::RectF(sr.left + pad, sr.top, sr.right - pad, sr.bottom), t.Get());
                // 光标（聚焦时）：测量文本宽度后画竖线
                if (searchFocused_) {
                    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
                    if (SUCCEEDED(SharedFactory()->CreateTextLayout(
                            searchText_.c_str(), static_cast<UINT32>(searchText_.size()),
                            fmt.Get(), sr.right - sr.left - pad * 2, sr.bottom - sr.top,
                            layout.GetAddressOf()))) {
                        DWRITE_TEXT_METRICS m{};
                        layout->GetMetrics(&m);
                        const float caretX = sr.left + pad + m.width + 1.0f;
                        auto caret = MakeBrush(rt, theme.accent);
                        rt->FillRectangle(D2D1::RectF(caretX, sr.top + 7, caretX + 1.5f, sr.bottom - 7),
                                          caret.Get());
                    }
                }
            }
        }
    }

    // 5) 字体列表（裁剪，遍历过滤结果）
    const int firstRow = scroll_ / static_cast<int>(kRowH);
    const int visibleRows = static_cast<int>((listRect_.bottom - listRect_.top) / kRowH) + 1;
    const int lastRow = (std::min)(static_cast<int>(filtered_.size()), firstRow + visibleRows + 1);

    rt->PushAxisAlignedClip(
        D2D1::RectF(static_cast<float>(listRect_.left), static_cast<float>(listRect_.top),
                    static_cast<float>(listRect_.right), static_cast<float>(listRect_.bottom)),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (filtered_.empty()) {
        auto emptyFmt = MakeTextFormat(L"Segoe UI Variable", 13);
        if (emptyFmt) {
            emptyFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto eb = MakeBrush(rt, theme.textSecondary);
            rt->DrawTextW(L"无匹配字体", 5, emptyFmt.Get(),
                          D2D1::RectF(static_cast<float>(listRect_.left), static_cast<float>(listRect_.top),
                                      static_cast<float>(listRect_.right), static_cast<float>(listRect_.bottom)),
                          eb.Get());
        }
    }

    for (int r = firstRow; r < lastRow; ++r) {
        const int i = filtered_[r];
        const auto& families = SystemFamilies();
        const float rowY = listRect_.top + r * kRowH - scroll_;
        const D2D1_RECT_F rowRect = D2D1::RectF(listRect_.left, rowY, listRect_.right, rowY + kRowH - 4);
        const bool sel = (i == selected_);
        const bool hov = (i == hover_);

        if (sel) {
            auto selBrush = MakeBrush(rt, D2D1::ColorF(theme.accent.r, theme.accent.g, theme.accent.b, 0.16f));
            rt->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 6, 6), selBrush.Get());
        } else if (hov) {
            auto hovBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 0.05f));
            rt->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 6, 6), hovBrush.Get());
        }

        // 字体族名（以该字体渲染预览；失败回退 Segoe UI）
        auto fmt = FamilyFormat(families[i], 14.0f);
        if (!fmt) fmt = MakeTextFormat(L"Segoe UI", 14.0f);
        if (fmt) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto nameBrush = MakeBrush(rt, sel ? theme.accent : theme.text);
            rt->DrawTextW(families[i].c_str(), static_cast<UINT32>(families[i].size()),
                          fmt.Get(),
                          D2D1::RectF(listRect_.left + 12, rowY, listRect_.left + (listRect_.right - listRect_.left) * 0.62f, rowY + kRowH - 4),
                          nameBrush.Get());
            // 右侧拉丁示例（复用同一格式，避免为每个字体创建两个 TextFormat）
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            rt->DrawTextW(L"AaBb 123", 8, fmt.Get(),
                          D2D1::RectF(listRect_.left + (listRect_.right - listRect_.left) * 0.62f, rowY,
                                      listRect_.right - 10, rowY + kRowH - 4),
                          textSecondaryBrush);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);  // 还原，供缓存复用
        }
    }
    rt->PopAxisAlignedClip();

    // 6) 滚动条（内容溢出时）
    const int listH = listRect_.bottom - listRect_.top;
    if (contentHeight_ > listH && contentHeight_ > 0) {
        const float trackH = static_cast<float>(listH);
        const float thumbH = (std::max)(24.0f, trackH * listH / contentHeight_);
        const float trackY = static_cast<float>(listRect_.top);
        const float maxScrollF = static_cast<float>(contentHeight_ - listH);
        const float thumbY = trackY + (trackH - thumbH) * (scroll_ / maxScrollF);
        auto thumbBrush = MakeBrush(rt, theme.border);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(listRect_.right - 4, thumbY, listRect_.right - 1, thumbY + thumbH), 2, 2),
            thumbBrush.Get());
    }

    // 7) 底部按钮：确定（主色）/ 取消
    auto btnFmt = MakeTextFormat(L"Segoe UI Variable", 13);
    if (btnFmt) {
        btnFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);  // 垂直居中
        btnFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);            // 水平居中
    }
    {
        const auto r = D2D1::RectF(static_cast<float>(okRect_.left), static_cast<float>(okRect_.top),
                                   static_cast<float>(okRect_.right), static_cast<float>(okRect_.bottom));
        auto okBrush = MakeBrush(rt, hoverOk_ ? theme.accentHover : theme.accent);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), okBrush.Get());
        if (btnFmt) {
            auto wBrush = MakeBrush(rt, D2D1::ColorF(1, 1, 1, 1));
            rt->DrawTextW(L"确定", 2, btnFmt.Get(), r, wBrush.Get());
        }
    }
    {
        const auto r = D2D1::RectF(static_cast<float>(cancelRect_.left), static_cast<float>(cancelRect_.top),
                                   static_cast<float>(cancelRect_.right), static_cast<float>(cancelRect_.bottom));
        auto bgBrush = MakeBrush(rt, theme.surface);
        auto bdBrush = MakeBrush(rt, theme.border);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), bgBrush.Get());
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 6, 6), bdBrush.Get(), 1.0f);
        if (btnFmt) {
            auto tBrush = MakeBrush(rt, hoverCancel_ ? theme.text : theme.textSecondary);
            rt->DrawTextW(L"取消", 2, btnFmt.Get(), r, tBrush.Get());
        }
    }
}

} // namespace moekoe
