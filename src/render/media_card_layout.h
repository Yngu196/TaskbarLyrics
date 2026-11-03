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
    //
    // 布局参考 Win11 媒体浮窗：
    //   ┌──────────────────────────────────┐
    //   │ ♪ MoeKoe Music        ┌────────┐ │
    //   │ 歌曲名（粗体）          │  封面   │ │
    //   │ 歌手                   └────────┘ │
    //   │        ⏮     ⏸     ⏭             │
    //   │  ────────●──────────────────      │
    //   │  0:01                  3:45       │
    //   └──────────────────────────────────┘
    static MediaCardLayout Compute(float dpiScale) {
        using namespace constants;
        MediaCardLayout l;
        l.width  = MEDIA_CARD_WIDTH_DP * dpiScale;
        l.height = MEDIA_CARD_HEIGHT_DP * dpiScale;
        const float pad = MEDIA_CARD_PADDING_DP * dpiScale;

        // 封面：右上角
        l.coverX    = l.width - pad - MEDIA_CARD_COVER_SIZE_DP * dpiScale;
        l.coverY    = MEDIA_CARD_COVER_TOP_DP * dpiScale;
        l.coverSize = MEDIA_CARD_COVER_SIZE_DP * dpiScale;

        // 文本区：封面左侧
        l.textX     = pad;
        l.textRight = l.coverX - 12.0f * dpiScale;

        // 顶部应用标识行（音符 + 应用名）
        l.metaTop    = 16.0f * dpiScale;
        l.metaHeight = 15.0f * dpiScale;

        // 标题/歌手（应用标识行下方）
        l.titleTop     = 40.0f * dpiScale;
        l.titleHeight  = 26.0f * dpiScale;
        l.artistTop    = 68.0f * dpiScale;
        l.artistHeight = 20.0f * dpiScale;

        // 控制按钮：底部水平居中（prev | play | next 等中心距）
        l.sideRadius = MEDIA_CARD_BUTTON_SIDE_DP * dpiScale * 0.5f;
        l.playRadius = MEDIA_CARD_BUTTON_PLAY_DP * dpiScale * 0.5f;
        l.controlsCy = 136.0f * dpiScale;
        l.playCx = l.width * 0.5f;
        l.prevCx = l.playCx - MEDIA_CARD_BUTTON_GAP_DP * dpiScale * 0.5f;
        l.nextCx = l.playCx + MEDIA_CARD_BUTTON_GAP_DP * dpiScale * 0.5f;

        // 进度条与时间
        l.seekX0 = pad;
        l.seekX1 = l.width - pad;
        l.seekCy = (MEDIA_CARD_HEIGHT_DP - MEDIA_CARD_PROGRESS_BOTTOM_DP) * dpiScale;
        l.seekThickness = MEDIA_CARD_PROGRESS_THICKNESS_DP * dpiScale;
        l.knobRadius    = 6.0f * dpiScale;
        l.timeTop    = (MEDIA_CARD_HEIGHT_DP - 28.0f) * dpiScale;
        l.timeHeight = 16.0f * dpiScale;
        return l;
    }
};

} // namespace moekoe
