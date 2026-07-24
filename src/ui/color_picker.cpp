// SPDX-License-Identifier: GPL-3.0
// color_picker.cpp - D2D 颜色选择器弹窗实现

#include "ui/color_picker.h"
#include "ui/settings_draw_utils.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <windowsx.h>
#include <wrl/client.h>

namespace moekoe {

using namespace Microsoft::WRL;

// 本地辅助：UTF-8 → 宽字符（与 d2d_settings_window.cpp 中的 Utf8ToWide 相同逻辑）
static std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

// ═══════════════════════════════════════════════════════════════════
// 生命周期
// ═══════════════════════════════════════════════════════════════════

void ColorPickerPopup::Activate(HWND hwnd, const D2D1_COLOR_F& initialColor, int titleBarHeight) {
    // 从当前 RGB 颜色计算 HSL，确保弹窗初始状态与当前颜色一致
    RGBToHSL(initialColor, hue_, sat_, lum_);

    // 计算弹窗位置（客户区坐标）：在窗口右侧偏上
    RECT clientRc;
    GetClientRect(hwnd, &clientRc);
    const int popupW = 224;
    const int popupH = 220;
    int popupX = clientRc.right - popupW - 8;
    int popupY = titleBarHeight + 16;

    popupRect_ = {popupX, popupY, popupX + popupW, popupY + popupH};

    // 色板网格区域（左侧 180x180）
    const int gridPad = 8;
    gridRect_ = {popupX + gridPad, popupY + gridPad,
                 popupX + gridPad + 180, popupY + gridPad + 180};

    // 亮度条区域（色板右侧）
    const int barW = 18;
    const int barPadX = 4;
    barRect_ = {gridRect_.right + barPadX, gridRect_.top,
                gridRect_.right + barPadX + barW, gridRect_.bottom};

    // 预览色块（色板下方）
    previewRect_ = {gridRect_.left, gridRect_.bottom + 6,
                    gridRect_.left + 40, gridRect_.bottom + 28};

    // "确定"按钮位置（Draw() 中计算后写入，此处给占位值）
    confirmRect_ = {};

    active_ = true;
    SetCapture(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void ColorPickerPopup::Deactivate(HWND hwnd) {
    active_ = false;
    ReleaseCapture();
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ═══════════════════════════════════════════════════════════════════
// 输入事件
// ═══════════════════════════════════════════════════════════════════

ColorPickerPopup::ActionResult ColorPickerPopup::HandleMouseDown(int x, int y,
                                                                 D2D1_COLOR_F* newColor,
                                                                 std::string* newHex) {
    if (!active_) return ActionResult::None;

    // 检查确认按钮
    if (PtInRect(&confirmRect_, {x, y})) {
        *newColor = HSLToRGB(hue_, sat_, lum_);
        *newHex = ColorFToHex(*newColor);
        return ActionResult::Confirmed;
    }

    // 检查色板网格
    if (PtInRect(&gridRect_, {x, y})) {
        const int gw = gridRect_.right - gridRect_.left;
        const int gh = gridRect_.bottom - gridRect_.top;
        float nx = std::clamp(static_cast<float>(x - gridRect_.left) / gw, 0.0f, 1.0f);
        float ny = std::clamp(static_cast<float>(y - gridRect_.top) / gh, 0.0f, 1.0f);
        hue_ = nx * 360.0f;
        sat_ = 1.0f - ny;
        return ActionResult::Handled;
    }

    // 检查亮度条
    if (PtInRect(&barRect_, {x, y})) {
        const int bh = barRect_.bottom - barRect_.top;
        float ny = std::clamp(static_cast<float>(y - barRect_.top) / bh, 0.0f, 1.0f);
        lum_ = 1.0f - ny;
        return ActionResult::Handled;
    }

    // 检查是否在弹窗内（但在交互区域外）→ 不处理
    if (PtInRect(&popupRect_, {x, y})) {
        return ActionResult::Handled;  // 消费但不做任何事，防止穿透到下层控件
    }

    // 弹窗外点击 → 取消
    return ActionResult::Cancelled;
}

void ColorPickerPopup::HandleMouseMove(int x, int y, bool leftButtonDown) {
    if (!active_ || !leftButtonDown) return;

    // 色板网格拖动
    if (PtInRect(&gridRect_, {x, y})) {
        const int gw = gridRect_.right - gridRect_.left;
        const int gh = gridRect_.bottom - gridRect_.top;
        float nx = std::clamp(static_cast<float>(x - gridRect_.left) / gw, 0.0f, 1.0f);
        float ny = std::clamp(static_cast<float>(y - gridRect_.top) / gh, 0.0f, 1.0f);
        hue_ = nx * 360.0f;
        sat_ = 1.0f - ny;
        return;
    }

    // 亮度条拖动
    if (PtInRect(&barRect_, {x, y})) {
        const int bh = barRect_.bottom - barRect_.top;
        float ny = std::clamp(static_cast<float>(y - barRect_.top) / bh, 0.0f, 1.0f);
        lum_ = 1.0f - ny;
        return;
    }
}

// ═══════════════════════════════════════════════════════════════════
// 绘制
// ═══════════════════════════════════════════════════════════════════

void ColorPickerPopup::Draw(ID2D1RenderTarget* rt,
                            bool isDarkMode,
                            const D2DThemeColors& theme,
                            IDWriteTextFormat* valueFmt,
                            IDWriteTextFormat* hintFmt,
                            ID2D1SolidColorBrush* textSecondaryBrush,
                            int scrollOffset) {
    if (!active_) return;

    // 弹窗背景（考虑滚动偏移）
    ComPtr<ID2D1SolidColorBrush> popupBg, popupBorder;
    D2D1_COLOR_F bgc = isDarkMode ? D2D1::ColorF(0.18f, 0.18f, 0.22f, 0.98f)
                                  : D2D1::ColorF(0.97f, 0.97f, 0.99f, 0.98f);
    rt->CreateSolidColorBrush(bgc, &popupBg);
    FillRoundedRect(rt, popupBg.Get(),
                    static_cast<float>(popupRect_.left),
                    static_cast<float>(popupRect_.top - scrollOffset),
                    static_cast<float>(popupRect_.right - popupRect_.left),
                    static_cast<float>(popupRect_.bottom - popupRect_.top), 8.f);

    rt->CreateSolidColorBrush(theme.border, &popupBorder);
    DrawRoundedRect(rt, popupBorder.Get(), 1.5f,
                    static_cast<float>(popupRect_.left),
                    static_cast<float>(popupRect_.top - scrollOffset),
                    static_cast<float>(popupRect_.right - popupRect_.left),
                    static_cast<float>(popupRect_.bottom - popupRect_.top), 8.f);

    // ── 绘制色板（Hue × Saturation 网格）──
    const int gw = gridRect_.right - gridRect_.left;
    const int gh = gridRect_.bottom - gridRect_.top;

    ComPtr<ID2D1Bitmap> gridBitmap;
    D2D1_SIZE_U bmpSizeU = D2D1::SizeU(static_cast<UINT>(gw), static_cast<UINT>(gh));
    D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT hr = rt->CreateBitmap(bmpSizeU, bmpProps, &gridBitmap);
    if (SUCCEEDED(hr)) {
        std::vector<uint32_t> pixels(gw * gh);
        for (int py = 0; py < gh; ++py) {
            float curSat = 1.0f - static_cast<float>(py) / static_cast<float>(gh - 1);
            for (int px = 0; px < gw; ++px) {
                float curHue = 360.0f * static_cast<float>(px) / static_cast<float>(gw - 1);
                D2D1_COLOR_F c = HSLToRGB(curHue, curSat, lum_);
                uint8_t r = static_cast<uint8_t>(std::clamp(c.r * 255.0f, 0.0f, 255.0f));
                uint8_t g = static_cast<uint8_t>(std::clamp(c.g * 255.0f, 0.0f, 255.0f));
                uint8_t b = static_cast<uint8_t>(std::clamp(c.b * 255.0f, 0.0f, 255.0f));
                pixels[py * gw + px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
        gridBitmap->CopyFromMemory(nullptr, pixels.data(), gw * 4);
        float gridTop = static_cast<float>(gridRect_.top - scrollOffset);
        rt->DrawBitmap(gridBitmap.Get(),
                       D2D1::RectF(static_cast<float>(gridRect_.left), gridTop,
                                   static_cast<float>(gridRect_.right),
                                   gridTop + static_cast<float>(gh)),
                       1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // 色板光标十字
    int cx = static_cast<int>(hue_ / 360.0f * (gw - 1)) + gridRect_.left;
    int cy = static_cast<int>((1.0f - sat_) * (gh - 1)) + gridRect_.top - scrollOffset;

    ComPtr<ID2D1SolidColorBrush> cursorBr;
    D2D1_COLOR_F cursorClr = lum_ > 0.5f ? D2D1::ColorF(0, 0, 0, 0.9f)
                                          : D2D1::ColorF(1, 1, 1, 0.9f);
    rt->CreateSolidColorBrush(cursorClr, &cursorBr);
    float cx_f = static_cast<float>(cx);
    float cy_f = static_cast<float>(cy);
    rt->DrawLine(D2D1::Point2F(cx_f - 6, cy_f), D2D1::Point2F(cx_f + 6, cy_f),
                 cursorBr.Get(), 1.5f);
    rt->DrawLine(D2D1::Point2F(cx_f, cy_f - 6), D2D1::Point2F(cx_f, cy_f + 6),
                 cursorBr.Get(), 1.5f);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx_f, cy_f), 4.5f, 4.5f),
                    cursorBr.Get(), 1.5f);

    // ── 绘制亮度条 ──
    const int bw = barRect_.right - barRect_.left;
    const int bh = barRect_.bottom - barRect_.top;
    ComPtr<ID2D1Bitmap> barBitmap;
    D2D1_SIZE_U barSizeU = D2D1::SizeU(static_cast<UINT>(bw), static_cast<UINT>(bh));
    HRESULT barHr = rt->CreateBitmap(barSizeU, bmpProps, &barBitmap);
    if (SUCCEEDED(barHr)) {
        std::vector<uint32_t> barPixels(bw * bh);
        for (int py = 0; py < bh; ++py) {
            float curLum = 1.0f - static_cast<float>(py) / static_cast<float>(bh - 1);
            D2D1_COLOR_F c = HSLToRGB(hue_, sat_, curLum);
            uint8_t r = static_cast<uint8_t>(std::clamp(c.r * 255.0f, 0.0f, 255.0f));
            uint8_t g = static_cast<uint8_t>(std::clamp(c.g * 255.0f, 0.0f, 255.0f));
            uint8_t b = static_cast<uint8_t>(std::clamp(c.b * 255.0f, 0.0f, 255.0f));
            for (int px = 0; px < bw; ++px) {
                barPixels[py * bw + px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
        barBitmap->CopyFromMemory(nullptr, barPixels.data(), bw * 4);
        float barTop = static_cast<float>(barRect_.top - scrollOffset);
        rt->DrawBitmap(barBitmap.Get(),
                       D2D1::RectF(static_cast<float>(barRect_.left), barTop,
                                   static_cast<float>(barRect_.right),
                                   barTop + static_cast<float>(bh)),
                       1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // 亮度条边框
    float barTopF = static_cast<float>(barRect_.top - scrollOffset);
    DrawRoundedRect(rt, popupBorder.Get(), 1.0f,
                    static_cast<float>(barRect_.left), barTopF,
                    static_cast<float>(bw), static_cast<float>(bh), 4.f);

    // 亮度条滑块指示
    int barCy = static_cast<int>((1.0f - lum_) * (bh - 1)) + barRect_.top - scrollOffset;
    float barCy_f = static_cast<float>(barCy);
    float barRight = static_cast<float>(barRect_.right + 2);
    rt->DrawLine(D2D1::Point2F(barRight, barCy_f),
                 D2D1::Point2F(barRight + 6, barCy_f - 4),
                 cursorBr.Get(), 1.5f);
    rt->DrawLine(D2D1::Point2F(barRight, barCy_f),
                 D2D1::Point2F(barRight + 6, barCy_f + 4),
                 cursorBr.Get(), 1.5f);

    // ── 预览色块 ──
    D2D1_COLOR_F previewColor = HSLToRGB(hue_, sat_, lum_);
    ComPtr<ID2D1SolidColorBrush> prevBr, prevBorderBr;
    rt->CreateSolidColorBrush(previewColor, &prevBr);
    float prevTop = static_cast<float>(previewRect_.top - scrollOffset);
    FillRoundedRect(rt, prevBr.Get(),
                    static_cast<float>(previewRect_.left), prevTop,
                    static_cast<float>(previewRect_.right - previewRect_.left),
                    static_cast<float>(previewRect_.bottom - previewRect_.top), 4.f);
    rt->CreateSolidColorBrush(theme.border, &prevBorderBr);
    DrawRoundedRect(rt, prevBorderBr.Get(), 1.0f,
                    static_cast<float>(previewRect_.left), prevTop,
                    static_cast<float>(previewRect_.right - previewRect_.left),
                    static_cast<float>(previewRect_.bottom - previewRect_.top), 4.f);

    // ── Hex 文本 ──
    std::string hexStr = ColorFToHex(previewColor);
    std::wstring wHex = Utf8ToWideLocal(hexStr);
    float hexX = static_cast<float>(previewRect_.right + 8);
    float hexY = static_cast<float>(previewRect_.top - scrollOffset + 4);
    DrawTextLine(rt, valueFmt, textSecondaryBrush,
                 wHex.c_str(), hexX, hexY, 80.f);

    // ── "确定"按钮 ──
    const float btnW = 56.f, btnH = 24.f;
    const float btnX = hexX;
    const float btnY = static_cast<float>(previewRect_.bottom - scrollOffset + 6);
    ComPtr<ID2D1SolidColorBrush> confirmBg, confirmTxt;
    rt->CreateSolidColorBrush(theme.accent, &confirmBg);
    FillRoundedRect(rt, confirmBg.Get(), btnX, btnY, btnW, btnH, 4.f);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &confirmTxt);
    const wchar_t* okText = L"确定";
    DrawTextLine(rt, hintFmt, confirmTxt.Get(), okText, btnX + 12.f, btnY + 3.f, 36.f);

    // 存储确认按钮区域供 HitTest 使用
    confirmRect_ = {static_cast<int>(btnX), static_cast<int>(btnY + scrollOffset),
                    static_cast<int>(btnX + btnW), static_cast<int>(btnY + scrollOffset + btnH)};
}

} // namespace moekoe
