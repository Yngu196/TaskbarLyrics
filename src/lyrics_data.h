// SPDX-License-Identifier: GPL-3.0
// lyrics_data.h - 共享数据结构和类型定义
//
// 该文件定义在整个插件中流转的核心数据结构：
//   - CharacterTiming : 单字时间轴
//   - LyricLine       : 单行歌词
//   - LyricsData      : 完整歌词
//   - PlayerState     : 播放器状态
//   - RenderState     : 渲染时使用的同步状态
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace moekoe {

// 单字时间轴（MoeKoeMusic 原始 JSON 格式）
struct CharacterTiming {
    std::string ch;        // 字符（UTF-8）
    int64_t     startTime{0}; // 毫秒
    int64_t     endTime{0};   // 毫秒
};

// 单行歌词
struct LyricLine {
    std::string                   text;        // 整行文本
    std::string                   translated; // 翻译（可空）
    int64_t                       startTime{0}; // 行起始时间（毫秒，用于 LRC 等无字符级时间轴的格式）
    std::vector<CharacterTiming>  characters;  // 逐字时间轴（KRC 格式）
};

// 完整歌词数据
struct LyricsData {
    std::vector<LyricLine> lines;
    bool                   valid{false}; // 数据是否有效（非空）
};

// 播放器状态
struct PlayerState {
    bool        isPlaying{false};
    double      currentTime{0.0};   // 秒
    std::string songTitle;           // 可选,用于调试
    std::string coverArtUrl;         // 专辑封面 URL（可能为空）
    std::string songName;            // 歌曲名称（用于封面降级显示）
};

// 悬停时点击的控制按钮
enum class HoverButton {
    None = 0,
    Prev,
    PlayPause,
    Next,
};

// 渲染状态（由解析器在每帧计算）
struct RenderState {
    std::string currentLine;          // 当前行文本
    std::string currentTranslated;    // 当前行翻译
    double      progress{0.0};        // 0.0 ~ 1.0
    int         currentLineIndex{-1}; // -1 表示未匹配
    bool        hasLyrics{false};
    bool        isPlaying{false};
    double      currentTime{0.0};     // 秒
    bool        isHovering{false};    // 鼠标是否悬停在歌词窗口上
    bool        isDragging{false};    // 是否正在拖动歌词窗口

    // 卡片模式专用
    std::string nextLine;             // 下一行歌词文本（预览）
    std::string nextTranslated;       // 下一行翻译（卡片"仅翻译"模式用）
    std::string coverArtUrl;          // 专辑封面 URL（可能为空）
    std::string songName;             // 歌曲名称（用于封面降级显示）

    // 频谱数据（纯音乐时使用，由 SpectrumCapture::GetSpectrum() 填充）
    std::vector<float> spectrumBands;
};

} // namespace moekoe
