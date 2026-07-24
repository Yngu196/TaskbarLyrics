// SPDX-License-Identifier: GPL-3.0
// color_utils.h - D2D 颜色工具函数（提取自 d2d_settings_window.cpp）
//
// 提供 Hex ↔ D2D1_COLOR_F 互转、HSL ↔ RGB 互转、颜色插值等工具函数。
// 所有函数均为纯计算，无状态，定义为 inline。
//
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <d2d1.h>
#include <string>

namespace moekoe {

// ── Hex 字符串 ↔ D2D1_COLOR_F ──

inline D2D1_COLOR_F HexToColorF(const std::string& hex, float alpha = 1.0f) {
    D2D1_COLOR_F c = {0, 0, 0, alpha};
    if (hex.size() >= 7 && hex[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        if (std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            c.r = r / 255.0f; c.g = g / 255.0f; c.b = b / 255.0f;
        }
    }
    return c;
}

inline std::string ColorFToHex(const D2D1_COLOR_F& c) {
    char buf[8];
    auto clampByte = [](float v) -> int {
        return static_cast<int>(std::clamp(v * 255.0f, 0.0f, 255.0f));
    };
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", clampByte(c.r), clampByte(c.g), clampByte(c.b));
    return buf;
}

// ── 颜色插值 ──

inline D2D1_COLOR_F Lerp(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + t * (b.r - a.r), a.g + t * (b.g - a.g),
            a.b + t * (b.b - a.b), a.a + t * (b.a - a.a)};
}

// ── HSL ↔ RGB 转换 ──

inline D2D1_COLOR_F HSLToRGB(float h, float s, float l) {
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;
    s = std::clamp(s, 0.0f, 1.0f);
    l = std::clamp(l, 0.0f, 1.0f);

    auto hueToRgb = [](float p, float q, float t) -> float {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6 * t;
        if (t < 0.5f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6;
        return p;
    };

    if (s == 0) return {l, l, l, 1.0f};

    float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    float p = 2 * l - q;
    float hNorm = h / 360.0f;

    return {hueToRgb(p, q, hNorm + 1.0f / 3.0f),
            hueToRgb(p, q, hNorm),
            hueToRgb(p, q, hNorm - 1.0f / 3.0f),
            1.0f};
}

inline void RGBToHSL(const D2D1_COLOR_F& rgb, float& h, float& s, float& l) {
    float r = rgb.r, g = rgb.g, b = rgb.b;
    float maxVal = std::max({r, g, b});
    float minVal = std::min({r, g, b});
    float delta = maxVal - minVal;

    l = (maxVal + minVal) / 2.0f;

    if (delta < 0.0001f) {
        h = 0; s = 0;
        return;
    }

    s = l > 0.5f ? delta / (2.0f - maxVal - minVal) : delta / (maxVal + minVal);

    if (maxVal == r)
        h = 60.0f * (std::fmod((g - b) / delta, 6.0f));
    else if (maxVal == g)
        h = 60.0f * ((b - r) / delta + 2.0f);
    else
        h = 60.0f * ((r - g) / delta + 4.0f);

    if (h < 0) h += 360.0f;
}

} // namespace moekoe
