// SPDX-License-Identifier: Apache-2.0
// math_utils.h - 渲染与 UI 共用的缓动及颜色插值函数
#pragma once

#include <algorithm>
#include <d2d1.h>

namespace moekoe::math {

inline float EaseOutCubic(float t) {
    const float c = std::clamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - c;
    return 1.0f - inv * inv * inv;
}

inline float EaseOutBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float c = std::clamp(t, 0.0f, 1.0f);
    const float v = c - 1.0f;
    return 1.0f + c3 * v * v * v + c1 * v * v;
}

inline D2D1_COLOR_F LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + t * (b.r - a.r), a.g + t * (b.g - a.g),
            a.b + t * (b.b - a.b), a.a + t * (b.a - a.a)};
}

} // namespace moekoe::math
