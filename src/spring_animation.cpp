// SPDX-License-Identifier: GPL-3.0
// spring_animation.cpp - 弹簧物理动画（缓动函数、弹簧步进、行切换淡入淡出）
#include "renderer.h"
#include "constants.h"

#include <algorithm>
#include <cmath>

namespace moekoe {

float TaskbarRenderer::EaseOutCubic(float t) {
    // ease-out cubic: f(t) = 1 - (1-t)^3
    float c = std::clamp(t, 0.0f, 1.0f);
    float inv = 1.0f - c;
    return 1.0f - inv * inv * inv;
}

float TaskbarRenderer::EaseInOutQuad(float t) {
    // ease-in-out quad: t<0.5 → 2*t^2, 否则 1-(2-2t)^2/2
    float c = std::clamp(t, 0.0f, 1.0f);
    if (c < 0.5f) {
        return 2.0f * c * c;
    }
    float inv = 1.0f - c;
    return 1.0f - 2.0f * inv * inv;
}

float TaskbarRenderer::EaseOutBack(float t) {
    // ease-out back：末端轻微"冲出"然后回弹
    // f(t) = 1 + c3*(t-1)^3 + c2*(t-1)^2, c1=1.70158, c2=c1+1, c3=c1+1
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float c = std::clamp(t, 0.0f, 1.0f);
    float v = c - 1.0f;
    return 1.0f + c3 * v * v * v + c1 * v * v;
}

bool TaskbarRenderer::UpdateLyricFade(const std::wstring& newText)
{
    // 首帧：新行文本入库，不做 fade
    static std::wstring lastText;
    if (lastText.empty()) {
        lastText = newText;
        return false;
    }

    // 歌词行发生变化且新旧均非空 → 启动 fade
    const bool lineChanged = (newText != lastText);
    if (lineChanged && !newText.empty() && !lastText.empty()) {
        lyricFadeOldText_ = lastText;

        LARGE_INTEGER li, freq;
        ::QueryPerformanceCounter(&li);
        ::QueryPerformanceFrequency(&freq);
        lyricFadeStartTime_ = static_cast<double>(li.QuadPart) / static_cast<double>(freq.QuadPart);
        lyricFadeActive_ = true;
    }
    lastText = newText;

    if (!lyricFadeActive_) {
        return false;
    }

    // 检查动画是否到期
    LARGE_INTEGER li2, freq2;
    ::QueryPerformanceCounter(&li2);
    ::QueryPerformanceFrequency(&freq2);
    const double now = static_cast<double>(li2.QuadPart) / static_cast<double>(freq2.QuadPart);
    const double elapsed = now - lyricFadeStartTime_;
    const double dur = static_cast<double>(constants::LYRIC_FADE_DURATION_MS) / 1000.0;

    if (elapsed >= dur) {
        lyricFadeActive_ = false;
        lyricFadeOldText_.clear();
        lyricFadeOldTrans_.clear();
        return false;
    }

    return true;
}

// ═════ P3-②: 卡拉OK进度弹簧 ═════
// 使用弹簧-阻尼物理模型将显示进度向目标平滑收敛。
// stiffness=120（刚度）、damping=14（接近临界阻尼），约 50ms 内完成主要运动。
// 返回 true 表示弹簧仍在运动中，需持续重绘。
bool TaskbarRenderer::UpdateProgressSpring(double target, double now)
{
    // 首次调用初始化
    if (springLastTime_ == 0.0) {
        springProgress_ = target;
        springVelocity_ = 0.0;
        springLastTime_ = now;
        return false;
    }

    const double dt = now - springLastTime_;
    springLastTime_ = now;

    // 时间步长钳位：避免暂停/调试期间超大 dt 导致弹簧爆炸
    const double clampedDt = (dt > 0.1) ? 0.1 : ((dt <= 0.0) ? 1.0 / 60.0 : dt);

    // 弹簧-阻尼物理：F = -k*(x-target) - c*v
    const double k = constants::KARAOKE_PROGRESS_SPRING_STIFFNESS;
    const double c = constants::KARAOKE_PROGRESS_SPRING_DAMPING;
    const double dx = springProgress_ - target;
    const double acceleration = -k * dx - c * springVelocity_;

    // 半隐式欧拉积分（先更新速度再更新位置，数值更稳定）
    springVelocity_ += acceleration * clampedDt;
    springProgress_ += springVelocity_ * clampedDt;

    // 收敛判定：位移和速度均足够小时视为静止，吸附到目标值
    const bool converged = (std::abs(springProgress_ - target) < 0.0005 &&
                            std::abs(springVelocity_) < 0.001);
    if (converged) {
        springProgress_ = target;
        springVelocity_ = 0.0;
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════
// 频谱渲染实现
// ═══════════════════════════════════════════


} // namespace moekoe
