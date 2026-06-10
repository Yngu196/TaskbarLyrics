// SPDX-License-Identifier: GPL-2.0
// lyrics_parser.h - 歌词解析与同步
//
// 职责:
//   - 持有最新的 LyricsData / PlayerState
//   - 主循环每帧调用 GetCurrentRenderState()
//   - 二分查找当前时间对应的歌词行与字符进度
//
#pragma once

#include "lyrics_data.h"

#include <mutex>
#include <string>
#include <vector>

namespace moekoe {

class LyricsParser {
public:
    LyricsParser() = default;

    // 从 WebSocket 接收歌词时调用
    void UpdateLyrics(const LyricsData& data);

    // 从 WebSocket 接收播放状态时调用
    void UpdatePlayerState(const PlayerState& state);

    // 主循环每帧调用,返回当前应渲染的内容
    RenderState GetCurrentRenderState() const;

    bool HasLyrics() const;
    bool IsPlaying() const;

    // ---- 辅助:LRC 解析(备用) ----
    struct LrcLine {
        double      timeSec{0.0};
        std::string text;
    };
    static std::vector<LrcLine> ParseLRC(const std::string& lrcContent);

private:
    // 二分查找 currentTimeSec 对应的歌词行索引;未匹配返回 -1
    int FindLineIndex(double currentTimeSec) const;

    // 互斥:歌词数据 / 播放状态均可能被 WebSocket 线程更新
    mutable std::mutex   mutex_;
    LyricsData            lyrics_;
    PlayerState           state_;

    // 本地时钟死推算：记录收到 playerState 时的本地时间
    // 用于在 GetCurrentRenderState() 中推算播放进度，使逐字高亮平滑推进
    mutable double lastUpdateWallTime_{0.0};
};

} // namespace moekoe
