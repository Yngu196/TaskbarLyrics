// SPDX-License-Identifier: Apache-2.0
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
    // 旧版本的 loop 配置不再提供，统一回退到滚动开启，避免升级后出现
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

        // 只有同时满足以下两个条件才进入终端态：
        // 1. 已滚动到最大偏移量（整句歌词末端已可见）
        // 2. 高亮进度已完成（progress >= 1.0）
        if (scrollOffset_ >= marqueeMaxOffset_ && progress >= 1.0f) {
            scrollOffset_ = marqueeMaxOffset_;
            marqueeState_ = MarqueeState::PauseRight;  // 终端态：定住不回弹
            stateStartTime_ = now;
        }
        return scrollOffset_;
    }

    case MarqueeState::PauseRight:
        // 终端态：唱完停在末端（PauseRight 复用为 HoldEnd，旧 bounce 逻辑已移除）
        return marqueeMaxOffset_;
    }

    return 0.0f;
}

float TaskbarRenderer::UpdateVerticalMarquee(const std::wstring& lyricText,
                                             float progress,
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
        // 与横向任务栏一致：新歌词直接进入跟随进度的滚动阶段，避免固定延迟
        // 使短句在尚未超出实际可视区域时就开始上移。
        verticalMarqueeState_ = maxOffset > 1.0f ? MarqueeState::ScrollLeft : MarqueeState::Idle;
        needRedraw = true;
        return 0.0f;
    }

    if (verticalMarqueeState_ == MarqueeState::Idle) return 0.0f;

    const double elapsed = now - verticalStateStartTime_;
    const double frameDelta = verticalLastUpdateTime_ > 0.0
        ? now - verticalLastUpdateTime_ : 0.016;
    verticalLastUpdateTime_ = now;
    // 使用纵向实际可视高度计算超长文本的加速比；不能套用横向窗口宽度。
    float speed = std::max(1.0f, settings_.marqueeSpeedPxPerSec);
    const float textHeight = glyphHeight * static_cast<float>(lyricText.size());
    if (textHeight > availableHeight * constants::MARQUEE_SPEEDUP_THRESHOLD) {
        const float ratio = textHeight / availableHeight;
        speed *= std::min(ratio / constants::MARQUEE_SPEEDUP_THRESHOLD, 3.0f);
    }
    const float step = std::max(0.0f, static_cast<float>(frameDelta)) * speed;

    switch (verticalMarqueeState_) {
    case MarqueeState::Delay:
        // 仅在回到起点后的下一轮使用延迟；首轮滚动与横向任务栏同步。
        needRedraw = true;
        if (elapsed * 1000.0 >= static_cast<double>(settings_.marqueeDelayMs)) {
            verticalMarqueeState_ = MarqueeState::ScrollLeft;
            verticalStateStartTime_ = now;
            verticalLastUpdateTime_ = now;
            verticalScrollOffset_ = 0.0f;
        }
        break;
    case MarqueeState::ScrollLeft: {
        // 让滚动目标跟随歌词进度，保证整句在播放结束前刚好移动到末端；
        // step 只负责限制每帧变化，避免进度跳变造成视觉突跳。
        const float targetOffset = std::clamp(progress, 0.0f, 1.0f) * verticalMaxOffset_;
        if (verticalScrollOffset_ < targetOffset) {
            verticalScrollOffset_ = std::min(verticalScrollOffset_ + step, targetOffset);
        } else if (verticalScrollOffset_ > targetOffset) {
            verticalScrollOffset_ = std::max(verticalScrollOffset_ - step, targetOffset);
        }
        needRedraw = true;
        if (verticalScrollOffset_ >= verticalMaxOffset_ && progress >= 1.0f) {
            verticalMarqueeState_ = MarqueeState::PauseRight;  // 终端态：定住不回弹
            verticalStateStartTime_ = now;
        }
        break;
    }
    case MarqueeState::PauseRight:
        // 终端态：唱完停在末端（PauseRight 复用为 HoldEnd，旧 bounce 逻辑已移除）
        break;
    default:
        break;
    }
    return verticalScrollOffset_;
}

// ═══════════════════════════════════════
// 卡片模式跑马灯（进度驱动单程滚动：跟随逐字高亮从行首开始滚，滚到尽头停住）
// ═══════════════════════════════════════

float TaskbarRenderer::UpdateCardMarquee(CardMarqueeState& m, const std::wstring& text,
                                         IDWriteTextFormat* format, float availWidth,
                                         float boxHeight, bool enabled, double progress,
                                         bool& needRedraw) {
    needRedraw = false;

    // 禁用（扩展宽度开启 / 跑马灯关闭 / 空文本）→ 重置并停用
    if (!enabled || text.empty() || availWidth <= 0.0f) {
        if (m.state != MarqueeState::Idle || m.offset != 0.0f || m.layout) {
            m.state = MarqueeState::Idle;
            m.offset = 0.0f;
            m.maxOffset = 0.0f;
            m.layout.Reset();
            needRedraw = true;
        }
        m.text = text;
        return 0.0f;
    }

    const double now = GetCurrentTimeSeconds();

    // 文本变化 → 重建布局缓存并重置状态机
    if (text != m.text) {
        m.text = text;
        m.offset = 0.0f;
        m.maxOffset = 0.0f;
        m.textWidth = 0.0f;
        m.layout.Reset();
        if (dwriteFactory_ && format) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    text.c_str(), static_cast<UINT32>(text.size()),
                    format, 10000.0f, boxHeight, layout.GetAddressOf()))) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                    m.textWidth = metrics.width;
                    m.layout = layout;  // 无换行布局（format 已设 NO_WRAP）
                }
            }
        }
        if (m.textWidth > availWidth + 1.0f) {
            m.maxOffset = m.textWidth - availWidth;
            m.state = MarqueeState::ScrollLeft;  // 进度驱动：跟随高亮立即开始滚动
        } else {
            m.state = MarqueeState::Idle;    // 短文本不需要滚动
            m.layout.Reset();
        }
        m.stateStart = now;
        m.lastUpdate = now;
        needRedraw = true;
        return 0.0f;
    }

    if (m.state == MarqueeState::Idle) return 0.0f;

    const double frameDelta = m.lastUpdate > 0.0 ? now - m.lastUpdate : 0.016;
    m.lastUpdate = now;

    // 恒定速度（超长文本自动加速，与卡拉OK跑马灯一致）
    float speed = std::max(1.0f, settings_.marqueeSpeedPxPerSec);
    if (m.textWidth > availWidth * constants::MARQUEE_SPEEDUP_THRESHOLD) {
        const float ratio = m.textWidth / availWidth;
        speed *= std::min(ratio / constants::MARQUEE_SPEEDUP_THRESHOLD, 3.0f);
    }
    const float step = std::max(0.0f, static_cast<float>(frameDelta)) * speed;

    switch (m.state) {
    case MarqueeState::ScrollLeft: {
        // 进度驱动：目标偏移 = maxOffset × 逐字进度。
        // 高亮从行首推进的同时滚动同步开始（边滚动边高亮），
        // 进度到 1 时目标到达 maxOffset，唱完必然滚完。
        const float clamped = static_cast<float>(std::clamp(progress, 0.0, 1.0));
        const float target = m.maxOffset * clamped;
        const float prev = m.offset;
        if (m.offset < target) {
            m.offset = std::min(m.offset + step, target);
        } else if (m.offset > target) {
            m.offset = std::max(m.offset - step, target);
        }
        needRedraw = (m.offset != prev);
        if (m.offset >= m.maxOffset) {
            // 滚到尽头后停在终端态（PauseRight 复用为 HoldEnd），不回弹
            m.state = MarqueeState::PauseRight;
            needRedraw = true;
        }
        return m.offset;
    }

    case MarqueeState::PauseRight:
        // 终端态：保持停在文本末端（不回弹）
        return m.maxOffset;

    default:
        return 0.0f;
    }
}

// 卡片模式歌词切换动画（淡入淡出 + 位移）
// ═════════════════════════════════════════


} // namespace moekoe
