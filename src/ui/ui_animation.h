// SPDX-License-Identifier: GPL-3.0
// ui_animation.h - 简单动画框架
//
// 职责:
//   - Animation<T>：泛型动画，支持 from→to 线性插值
//   - 缓动函数：EaseOutCubic / Spring
//   - 动画驱动：每帧 Tick() 推进时间，查询 Current() 获取当前值
//
#pragma once

#include <algorithm>
#include <cmath>
#include <chrono>
#include <vector>
#include <functional>
#include <d2d1.h>

namespace moekoe::ui {

// ── 缓动函数 ──

// EaseOutCubic：快速启动，缓慢结束
inline float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float f = 1.0f - t;
    return 1.0f - f * f * f;
}

// Spring：弹簧动画（带阻尼振荡）
// stiffness: 刚度（越大越快收敛）
// damping: 阻尼（越大振荡越少）
// 返回值在 [0, 1] 之外可能振荡
inline float Spring(float t, float stiffness = 180.0f, float damping = 12.0f);

// Spring 缓动的单参数适配版本（可直接作为 EasingFn 使用）
inline float SpringEase(float t) { return Spring(t); }

inline float Spring(float t, float stiffness, float damping) {
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    // 简化弹簧模型：欠阻尼振荡衰减
    float omega = std::sqrt(stiffness);
    float zeta = damping / (2.0f * omega);
    if (zeta < 1.0f) {
        // 欠阻尼
        float omegaD = omega * std::sqrt(1.0f - zeta * zeta);
        return 1.0f - std::exp(-zeta * omega * t * 10.0f) *
               (std::cos(omegaD * t * 10.0f) +
                (zeta * omega / omegaD) * std::sin(omegaD * t * 10.0f));
    } else {
        // 临界/过阻尼：退化为 EaseOut
        return EaseOutCubic(t);
    }
}

// ── 插值函数（必须在 Animation<T> 之前声明） ──

// float 插值
inline float Lerp(float a, float b, float t) {
    return a + t * (b - a);
}

// D2D1_COLOR_F 插值
inline D2D1_COLOR_F LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + t * (b.r - a.r), a.g + t * (b.g - a.g),
            a.b + t * (b.b - a.b), a.a + t * (b.a - a.a)};
}

// ── Animation<T> ──
// 泛型动画，支持 float / D2D1_COLOR_F 等可插值类型

template <typename T>
struct Animation {
    T from{};           // 起始值
    T to{};             // 目标值
    float durationMs{0};// 持续时间（毫秒）
    float elapsedMs{0}; // 已过时间（毫秒）
    bool active{false}; // 是否正在播放

    // 缓动函数类型
    using EasingFn = float(*)(float);
    EasingFn easing{EaseOutCubic};

    // 启动动画
    void Start(const T& fromVal, const T& toVal, float durMs, EasingFn fn = EaseOutCubic) {
        from = fromVal;
        to = toVal;
        durationMs = durMs;
        elapsedMs = 0;
        active = true;
        easing = fn;
    }

    // 从当前值启动
    void StartFromCurrent(const T& currentVal, const T& toVal, float durMs, EasingFn fn = EaseOutCubic) {
        from = currentVal;
        to = toVal;
        durationMs = durMs;
        elapsedMs = 0;
        active = true;
        easing = fn;
    }

    // 推进时间（返回是否仍在播放）
    bool Tick(float deltaMs) {
        if (!active) return false;
        elapsedMs += deltaMs;
        if (elapsedMs >= durationMs) {
            elapsedMs = durationMs;
            active = false;
            return false;
        }
        return true;
    }

    // 获取当前插值（默认使用 Lerp，D2D1_COLOR_F 特化使用 LerpColor）
    T Current() const {
        if (!active || durationMs <= 0) return to;
        float t = easing(elapsedMs / durationMs);
        return Lerp(from, to, t);
    }

    // 是否已完成
    bool IsDone() const { return !active; }

    // 强制完成
    void Finish() {
        elapsedMs = durationMs;
        active = false;
    }
};

// Animation<D2D1_COLOR_F> 的 Current() 特化
template <>
inline D2D1_COLOR_F Animation<D2D1_COLOR_F>::Current() const {
    if (!active || durationMs <= 0) return to;
    float t = easing(elapsedMs / durationMs);
    return LerpColor(from, to, t);
}

} // namespace moekoe::ui
