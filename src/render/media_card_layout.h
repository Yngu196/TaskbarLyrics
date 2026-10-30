// SPDX-License-Identifier: GPL-3.0
// media_card_layout.h - 媒体卡片（点击任务栏歌词弹出的歌曲卡片）共享布局
//
// 渲染器与命中测试窗口共用同一套几何计算，避免两处坐标公式各改一半
// 导致「看到的位置」和「点到的位置」不一致。
//
// 单位说明：Compute() 返回物理像素坐标（已乘以 DPI 缩放），
// 与渲染目标的 96 DPI 坐标系一致。
#pragma once

#include "core/constants.h"

namespace moekoe {

struct MediaCardLayout {
    float width{0.0f};
    float height{0.0f};

    float coverX{0.0f};
    float coverY{0.0f};
    float coverSize{0.0f};

    float textX{0.0f};
    float textRight{0.0f};
    float titleTop{0.0f};
    float titleHeight{0.0f};
    float artistTop{0.0f};
    float artistHeight{0.0f};
    float metaTop{0.0f};
    float metaHeight{0.0f};

    float controlsCy{0.0f};
    float prevCx{0.0f};
    float playCx{0.0f};
    float nextCx{0.0f};
    float playRadius{0.0f};
    float sideRadius{0.0f};

    float seekX0{0.0f};
    float seekX1{0.0f};
    float seekCy{0.0f};
    float seekThickness{0.0f};
    float knobRadius{0.0f};
    float timeTop{0.0f};
    float timeHeight{0.0f};

    // dpiScale = 当前 DPI / 96
    static MediaCardLayout Compute(float dpiScale) {
        using namespace constants;
        MediaCardLayout l;
        l.width  = MEDIA_CARD_WIDTH_DP * dpiScale;
        l.height = MEDIA_CARD_HEIGHT_DP * dpiScale;

        l.coverX    = MEDIA_CARD_PADDING_DP * dpiScale;
        l.coverY    = MEDIA_CARD_COVER_TOP_DP * dpiScale;
        l.coverSize = MEDIA_CARD_COVER_SIZE_DP * dpiScale;

        l.textX = l.coverX + l.coverSize + MEDIA_CARD_PADDING_DP * dpiScale;

        // 右侧控制按钮：下一首 / 播放暂停 / 上一首
        const float pad     = MEDIA_CARD_PADDING_DP * dpiScale;
        const float sideR   = MEDIA_CARD_BUTTON_SIDE_DP * dpiScale * 0.5f;
        const float playR   = MEDIA_CARD_BUTTON_PLAY_DP * dpiScale * 0.5f;
        const float gap     = MEDIA_CARD_BUTTON_GAP_DP * dpiScale;
        l.sideRadius = sideR;
        l.playRadius = playR;
        l.nextCx = l.width - pad - sideR;
        l.playCx = l.nextCx - sideR - gap - playR;
        l.prevCx = l.playCx - playR - gap - sideR;

        l.textRight = l.prevCx - sideR - gap;

        l.titleTop     = 18.0f * dpiScale;
        l.titleHeight  = 28.0f * dpiScale;
        l.artistTop    = 47.0f * dpiScale;
        l.artistHeight = 22.0f * dpiScale;
        l.metaTop      = 71.0f * dpiScale;
        l.metaHeight   = 18.0f * dpiScale;

        l.controlsCy = l.coverY + l.coverSize * 0.5f;

        l.seekX0 = pad;
        l.seekX1 = l.width - pad;
        l.seekCy = (MEDIA_CARD_HEIGHT_DP - MEDIA_CARD_PROGRESS_BOTTOM_DP) * dpiScale;
        l.seekThickness = MEDIA_CARD_PROGRESS_THICKNESS_DP * dpiScale;
        l.knobRadius    = 6.0f * dpiScale;
        l.timeTop    = (MEDIA_CARD_HEIGHT_DP - 27.0f) * dpiScale;
        l.timeHeight = 16.0f * dpiScale;
        return l;
    }
};

} // namespace moekoe
