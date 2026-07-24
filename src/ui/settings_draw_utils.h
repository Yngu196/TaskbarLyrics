// SPDX-License-Identifier: GPL-3.0
// settings_draw_utils.h - D2D 绘制原语辅助函数（提取自 d2d_settings_window.cpp）
//
// 提供圆角矩形填充/描边、单行文本绘制等底层 D2D 绘制原语。
// 所有函数无状态，定义为 inline。
//
#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <string>

namespace moekoe {

// ── 圆角矩形填充 ──
inline void FillRoundedRect(ID2D1RenderTarget* rt, ID2D1Brush* brush,
                            float x, float y, float w, float h, float r) {
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x, y, x + w, y + h), r, r};
    rt->FillRoundedRectangle(rr, brush);
}

// ── 圆角矩形描边 ──
inline void DrawRoundedRect(ID2D1RenderTarget* rt, ID2D1Brush* brush, float strokeWidth,
                            float x, float y, float w, float h, float r) {
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x, y, x + w, y + h), r, r};
    rt->DrawRoundedRectangle(rr, brush, strokeWidth);
}

// ── 单行文本绘制 ──
inline void DrawTextLine(ID2D1RenderTarget* rt, IDWriteTextFormat* fmt,
                         ID2D1Brush* brush, const wchar_t* text,
                         float x, float y, float maxWidth) {
    rt->DrawText(text, static_cast<UINT>(wcslen(text)), fmt,
                 D2D1::RectF(x, y, x + maxWidth, y + 200), brush);
}

} // namespace moekoe
