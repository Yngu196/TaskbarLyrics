// SPDX-License-Identifier: GPL-3.0
// marquee_engine.cpp - 跑马灯状态机（滚动位置计算、停顿逻辑、文本宽度测量）
#include "render/renderer.h"
#include "render/renderer_utils.h"
#include "core/constants.h"

#include <algorithm>
#include <cmath>

namespace moekoe {
using renderer_utils::Utf8ToWide;
using renderer_utils::GetCurrentTimeSeconds;

TaskbarRenderer::MarqueeMode TaskbarRenderer::ParseMarqueeMode(const std::string& mode) {
    // 旧版本的 loop 配置不再提供，统一回退到往返滚动，避免升级后出现
    // UI 没有对应选项但内部仍执行旧循环逻辑的状态。
    if (mode == "loop" || mode == "Loop") return MarqueeMode::Bounce;
    if (mode == "off" || mode == "Off")   return MarqueeMode::Off;
    return MarqueeMode::Bounce;  // default
}

float TaskbarRenderer::UpdateMarquee(const std::string& lyricText, float progress, bool& needRedraw, float availableWidth) {
    needRedraw = false;

    const MarqueeMode mode = ParseMarqueeMode(settings_.marqueeMode);

    // 跑马灯关闭 → 始终不滚动
    if (!settings_.enableMarquee || mode == MarqueeMode::Off) {
        if (marqueeState_ != MarqueeState::Idle) {
            marqueeState_ = MarqueeState::Idle;
            scrollOffset_ = 0.0f;
            needRedraw = true;
        }
        return 0.0f;
    }

    // 使用传入的实际可用宽度（已考虑封面偏移），未传入时回退到 TEXT_PADDING_X 对称计算
    const float availWidth = (availableWidth > 0.0f)
        ? availableWidth
        : (static_cast<FLOAT>(width_) - constants::TEXT_PADDING_X * 2.0f *
           static_cast<float>(dpi_) / 96.0f);

    // 检测歌词文本变化 → 重置状态机
    if (lyricText != marqueeLastText_) {
        marqueeLastText_ = lyricText;
        scrollOffset_ = 0.0f;
        marqueeProgress_ = 0.0f;

        // 测量文本宽度
        marqueeTextWidth_ = 0.0f;
        marqueeMaxOffset_ = 0.0f;
        if (!lyricText.empty() && dwriteFactory_ && textFormat_) {
            const std::wstring wText = Utf8ToWide(lyricText);
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    wText.c_str(), static_cast<UINT32>(wText.size()),
                    textFormat_.Get(), availWidth, static_cast<FLOAT>(height_),
                    layout.GetAddressOf()))) {
                DWRITE_TEXT_METRICS m{};
                if (SUCCEEDED(layout->GetMetrics(&m))) {
                    marqueeTextWidth_ = m.width;
                }
            }
        }

        // 判断是否需要滚动：文本宽度 > 可用宽度
        if (marqueeTextWidth_ > availWidth + 1.0f) {
            marqueeMaxOffset_ = marqueeTextWidth_ - availWidth;
            // 长歌词直接进入滚动状态（跟随高亮进度），不需要先 Delay 等待。
            // Delay 仅用于 bounce 模式的回位后循环。
            marqueeState_ = MarqueeState::ScrollLeft;
        } else {
            // 短文本不需要滚动
            marqueeState_ = MarqueeState::Idle;
            marqueeMaxOffset_ = 0.0f;
        }

        stateStartTime_ = GetCurrentTimeSeconds();
        marqueeLastUpdateTime_ = stateStartTime_;
        needRedraw = true;
        return 0.0f;
    }

    // 空文本或无需滚动
    if (marqueeState_ == MarqueeState::Idle) {
        return 0.0f;
    }

    // 记录当前高亮进度（用于控制回位时机）
    marqueeProgress_ = progress;

    const double now = GetCurrentTimeSeconds();
    const double elapsed = now - stateStartTime_;

    // 计算有效滚动速度（超长歌词自动加速）
    float speed = settings_.marqueeSpeedPxPerSec;
    if (marqueeTextWidth_ > availWidth * constants::MARQUEE_SPEEDUP_THRESHOLD) {
        // 超出越多越快，最高 3 倍速
        const float ratio = marqueeTextWidth_ / availWidth;
        speed *= std::min(ratio / constants::MARQUEE_SPEEDUP_THRESHOLD, 3.0f);
    }

    // 帧间增量（秒），用于恒定速度的步进计算
    const double frameDelta = (marqueeLastUpdateTime_ > 0.0) ? (now - marqueeLastUpdateTime_) : 0.016;
    marqueeLastUpdateTime_ = now;

    switch (marqueeState_) {
    case MarqueeState::Idle:
        return 0.0f;

    case MarqueeState::Delay:
        // 等待 delayMs 后开始向左滚动
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueeDelayMs)) {
            marqueeState_ = MarqueeState::ScrollLeft;
            stateStartTime_ = now;
            marqueeLastUpdateTime_ = now;
            scrollOffset_ = 0.0f;
        }
        return 0.0f;

    case MarqueeState::ScrollLeft: {
        // 基于高亮进度计算目标滚动位置，然后以恒定速度平滑逼近。
        const float progressClamped = std::clamp(progress, 0.0f, 1.0f);
        const float targetOffset = progressClamped * marqueeMaxOffset_;

        const float maxStep = static_cast<float>(frameDelta) * speed;
        if (scrollOffset_ < targetOffset) {
            scrollOffset_ = std::min(scrollOffset_ + maxStep, targetOffset);
        } else if (scrollOffset_ > targetOffset) {
            // 进度回退时（罕见），也平滑跟回
            scrollOffset_ = std::max(scrollOffset_ - maxStep, targetOffset);
        }
        needRedraw = true;

        // 只有同时满足以下两个条件才触发回位序列：
        // 1. 已滚动到最大偏移量（整句歌词末端已可见）
        // 2. 高亮进度已完成（progress >= 1.0）
        if (scrollOffset_ >= marqueeMaxOffset_ && progress >= 1.0f) {
            scrollOffset_ = marqueeMaxOffset_;
            marqueeState_ = MarqueeState::PauseRight;
            stateStartTime_ = now;
        }
        return scrollOffset_;
    }

    case MarqueeState::PauseRight:
        // 右端点暂停 pauseMs
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueePauseMs)) {
            marqueeState_ = MarqueeState::ScrollRight;
            stateStartTime_ = now;
            marqueeLastUpdateTime_ = now;
        }
        return marqueeMaxOffset_;

    case MarqueeState::ScrollRight: {
        const float distance = static_cast<float>(frameDelta) * speed;
        scrollOffset_ = marqueeMaxOffset_ - std::min(distance, marqueeMaxOffset_);
        needRedraw = true;

        if (scrollOffset_ <= 0.0f) {
            scrollOffset_ = 0.0f;
            marqueeState_ = MarqueeState::PauseLeft;
            stateStartTime_ = now;
        }
        return scrollOffset_;
    }

    case MarqueeState::PauseLeft:
        // 左端点暂停 pauseMs 后回到 Delay
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueePauseMs)) {
            marqueeState_ = MarqueeState::Delay;
            stateStartTime_ = now;
        }
        return 0.0f;
    }

    return 0.0f;
}

float TaskbarRenderer::UpdateVerticalMarquee(const std::wstring& lyricText,
                                             float glyphHeight,
                                             float availableHeight,
                                             bool& needRedraw) {
    needRedraw = false;
    const MarqueeMode mode = ParseMarqueeMode(settings_.marqueeMode);
    if (!settings_.enableMarquee || mode == MarqueeMode::Off || lyricText.empty() ||
        glyphHeight <= 0.0f || availableHeight <= 0.0f) {
        if (verticalMarqueeState_ != MarqueeState::Idle || verticalScrollOffset_ != 0.0f) {
            verticalMarqueeState_ = MarqueeState::Idle;
            verticalScrollOffset_ = 0.0f;
            verticalMaxOffset_ = 0.0f;
            needRedraw = true;
        }
        return 0.0f;
    }

    const float maxOffset = std::max(0.0f,
        glyphHeight * static_cast<float>(lyricText.size()) - availableHeight);
    const double now = GetCurrentTimeSeconds();
    if (lyricText != verticalMarqueeText_ ||
        std::abs(maxOffset - verticalMaxOffset_) > 0.5f) {
        verticalMarqueeText_ = lyricText;
        verticalScrollOffset_ = 0.0f;
        verticalMaxOffset_ = maxOffset;
        verticalStateStartTime_ = now;
        verticalLastUpdateTime_ = now;
        verticalMarqueeState_ = maxOffset > 1.0f ? MarqueeState::Delay : MarqueeState::Idle;
        needRedraw = true;
        return 0.0f;
    }

    if (verticalMarqueeState_ == MarqueeState::Idle) return 0.0f;

    const double elapsed = now - verticalStateStartTime_;
    const double frameDelta = verticalLastUpdateTime_ > 0.0
        ? now - verticalLastUpdateTime_ : 0.016;
    verticalLastUpdateTime_ = now;
    const float step = std::max(0.0f, static_cast<float>(frameDelta)) *
        std::max(1.0f, settings_.marqueeSpeedPxPerSec);

    switch (verticalMarqueeState_) {
    case MarqueeState::Delay:
        needRedraw = true;
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueeDelayMs)) {
            verticalMarqueeState_ = MarqueeState::ScrollLeft;
            verticalStateStartTime_ = now;
        }
        break;
    case MarqueeState::ScrollLeft:
        verticalScrollOffset_ = std::min(verticalScrollOffset_ + step, verticalMaxOffset_);
        needRedraw = true;
        if (verticalScrollOffset_ >= verticalMaxOffset_) {
            verticalMarqueeState_ = MarqueeState::PauseRight;
            verticalStateStartTime_ = now;
        }
        break;
    case MarqueeState::PauseRight:
        needRedraw = true;
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueePauseMs)) {
            verticalMarqueeState_ = MarqueeState::ScrollRight;
            verticalStateStartTime_ = now;
        }
        break;
    case MarqueeState::ScrollRight:
        verticalScrollOffset_ = std::max(0.0f, verticalScrollOffset_ - step);
        needRedraw = true;
        if (verticalScrollOffset_ <= 0.0f) {
            verticalMarqueeState_ = MarqueeState::PauseLeft;
            verticalStateStartTime_ = now;
        }
        break;
    case MarqueeState::PauseLeft:
        needRedraw = true;
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueePauseMs)) {
            verticalMarqueeState_ = MarqueeState::Delay;
            verticalStateStartTime_ = now;
        }
        break;
    case MarqueeState::Idle:
        break;
    }
    return verticalScrollOffset_;
}

// ═════════════════════════════════════════
// 卡片模式歌词切换动画（淡入淡出 + 位移）
// ═════════════════════════════════════════


} // namespace moekoe
