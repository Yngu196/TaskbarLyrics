// SPDX-License-Identifier: GPL-3.0
// lyrics_parser.h - 歌词解析与同步
//
// 职责:
//   - 持有最新的 LyricsData / PlayerState
//   - 主循环每帧调用 GetCurrentRenderState()
//   - 二分查找当前时间对应的歌词行与字符进度
//
#pragma once

#include "lyrics/lyrics_data.h"

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

    // 用户拖动媒体卡片进度条后调用：把本地播放时间立即置为目标值，
    // 使界面不必等待播放器回传 playerState 就能显示新进度
    void SeekTo(double seconds);

    bool HasLyrics() const;
    bool IsPlaying() const;

    // ---- 辅助:LRC 解析(备用) ----
    struct LrcLine {
        double      timeSec{0.0};
        std::string text;
        std::string translated; // 翻译文本（双语 LRC）
    };
    static std::vector<LrcLine> ParseLRC(const std::string& lrcContent);

    // 二分查找 currentTimeSec 对应的歌词行索引;未匹配返回 -1
    // 单元测试需要白盒访问，始终声明为 public（const 方法，暴露无害）
    int FindLineIndex(double currentTimeSec) const;

private:
    // 互斥:歌词数据 / 播放状态均可能被 WebSocket 线程更新
    mutable std::mutex   mutex_;
    LyricsData            lyrics_;
    PlayerState           state_;

    // 本地时钟死推算：记录收到 playerState 时的本地时间
    // 用于在 GetCurrentRenderState() 中推算播放进度，使逐字高亮平滑推进
    mutable double lastUpdateWallTime_{0.0};

    // 最近一次非空的歌曲元数据：MoeKoeMusic 的进度更新消息
    // 只含 playing/time 不含歌曲信息，用于保留封面/歌名不被清空
    std::string lastSongTitle_;
    std::string lastSongName_;
    std::string lastArtist_;
    std::string lastCoverArtUrl_;
    double      lastDuration_{0.0};  // 最近一次有效的歌曲总时长（秒）

    // 本地 seek 后的“旧进度抑制”窗口：seek 请求要经过
    // EXE→扩展→页面 异步传递，期间回传的 playerState 仍是旧进度，
    // 直接采用会让进度条/歌词瞬间跳回原位置
    double      seekTarget_{-1.0};        // 最近一次 seek 的目标时间（秒）
    double      seekSuppressUntil_{0.0};  // 抑制截止的本地墙钟时间（秒）
};

} // namespace moekoe
