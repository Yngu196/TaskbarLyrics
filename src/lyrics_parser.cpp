// SPDX-License-Identifier: GPL-3.0
// lyrics_parser.cpp - 歌词解析与同步实现
#include "lyrics_parser.h"

#include <algorithm>
#include <cstdint>
#include <regex>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace moekoe {

namespace {

/// 高精度本地时钟（秒），基于 QueryPerformanceCounter
double GetWallTimeSeconds() {
    static const LARGE_INTEGER freq = []{
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter;
    ::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
}

} // namespace

void LyricsParser::UpdateLyrics(const LyricsData& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    lyrics_ = data;
}

void LyricsParser::UpdatePlayerState(const PlayerState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    // 记录本地高精度时钟，用于 GetCurrentRenderState() 中推算时间
    lastUpdateWallTime_ = GetWallTimeSeconds();
}

bool LyricsParser::HasLyrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lyrics_.valid && !lyrics_.lines.empty();
}

bool LyricsParser::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.isPlaying;
}

int LyricsParser::FindLineIndex(double currentTimeSec) const {
    // 优先使用 characters 数组的 startTime（字符级精确同步）
    // 若 characters 为空则回退到 line 级别的 startTime（来自 LRC 行标签）
    const int64_t tMs = static_cast<int64_t>(currentTimeSec * 1000.0);
    const int n = static_cast<int>(lyrics_.lines.size());
    int lo = 0, hi = n - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const auto& line = lyrics_.lines[mid];

        // 获取该行的起始时间：优先 characters，回退到 line.startTime
        int64_t startMs = 0;
        if (!line.characters.empty()) {
            startMs = line.characters.front().startTime;
        } else {
            startMs = line.startTime;  // LRC 行级时间戳
        }

        if (startMs <= tMs) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

RenderState LyricsParser::GetCurrentRenderState() const {
    RenderState out;
    std::lock_guard<std::mutex> lock(mutex_);

    out.isPlaying   = state_.isPlaying;

    // ── 本地时钟推算：播放状态下用本地时间插值 currentTime ──
    // 目的：即使 playerState 消息频率低（如每秒一次），progress 也能每帧平滑推进
    // 原理：estimatedTime = lastReceivedTime + (localNow - localThen)
    double effectiveTime = state_.currentTime;
    if (state_.isPlaying && lastUpdateWallTime_ > 0.0) {
        const double elapsed = GetWallTimeSeconds() - lastUpdateWallTime_;
        // 限定最大插值 10 秒，防止异常（睡眠恢复、debug 断点等）导致时间狂奔
        if (elapsed > 0.0 && elapsed < 10.0) {
            effectiveTime = state_.currentTime + elapsed;
        }
    }
    out.currentTime = effectiveTime;

    if (!lyrics_.valid || lyrics_.lines.empty()) {
        out.hasLyrics = false;
        return out;
    }
    out.hasLyrics = true;

    // 检测纯音乐标记：若所有非空歌词行均为纯音乐占位文本，
    // 则视为无歌词，交由上层渲染频谱而非文字。
    {
        bool allInstrumental = true;
        for (const auto& line : lyrics_.lines) {
            if (line.text.empty()) continue;
            if (line.text.find("纯音乐") == std::string::npos &&
                line.text.find("Instrumental") == std::string::npos &&
                line.text.find("instrumental") == std::string::npos) {
                allInstrumental = false;
                break;
            }
        }
        if (allInstrumental) {
            out.hasLyrics = false;
            return out;
        }
    }

    const int idx = FindLineIndex(effectiveTime);
    if (idx < 0) {
        // 进度在第一行之前 → 兜底：返回第一行（独立模式 currentTime=0 时触发）
        if (!lyrics_.lines.empty()) {
            const auto& line = lyrics_.lines[0];
            out.currentLineIndex = 0;
            out.currentLine      = line.text;
            out.currentTranslated= line.translated;
        }
        return out;
    }

    const auto& line = lyrics_.lines[idx];
    out.currentLineIndex = idx;
    out.currentLine      = line.text;
    out.currentTranslated= line.translated;

    // 卡片模式：输出下一行歌词预览
    if (idx + 1 < static_cast<int>(lyrics_.lines.size())) {
        out.nextLine = lyrics_.lines[idx + 1].text;
    }

    // 传递封面 URL 和歌曲名（来自 PlayerState）
    out.coverArtUrl = state_.coverArtUrl;
    out.songName    = state_.songName;

    // 在该行内计算字符级进度
    if (!line.characters.empty()) {
        const int64_t tMs = static_cast<int64_t>(effectiveTime * 1000.0);
        const auto& chars = line.characters;
        // 找到 tMs 落入哪个字符
        int charIdx = -1;
        for (size_t i = 0; i < chars.size(); ++i) {
            if (tMs >= chars[i].startTime && tMs <= chars[i].endTime) {
                charIdx = static_cast<int>(i);
                break;
            }
            if (tMs > chars[i].endTime &&
                (i + 1 >= chars.size() || tMs < chars[i + 1].startTime)) {
                charIdx = static_cast<int>(i);
                break;
            }
        }
        if (charIdx < 0) {
            if (tMs < chars.front().startTime) charIdx = -1;
            else charIdx = static_cast<int>(chars.size()) - 1;
        }

        if (charIdx < 0) {
            out.progress = 0.0;
        } else if (static_cast<size_t>(charIdx) >= chars.size() - 1) {
            // 最后一个字符: 与其他字符一样的平滑插值
            const auto& cur = chars[chars.size() - 1];
            const int64_t dur = std::max<int64_t>(1, cur.endTime - cur.startTime);
            const double inside = std::clamp(
                static_cast<double>(tMs - cur.startTime) / static_cast<double>(dur),
                0.0, 1.0);
            out.progress = (static_cast<double>(chars.size() - 1) + inside) /
                           static_cast<double>(chars.size());
        } else {
            // 进度 = (已唱完整字符数 + 当前字符内进度) / 总字符数
            const auto& cur = chars[charIdx];
            const int64_t dur = std::max<int64_t>(1, cur.endTime - cur.startTime);
            const double inside = std::clamp(
                static_cast<double>(tMs - cur.startTime) / static_cast<double>(dur),
                0.0, 1.0);
            out.progress = (static_cast<double>(charIdx) + inside) /
                           static_cast<double>(chars.size());
        }
    } else {
        // 没有逐字时间轴 -> 使用整行
        if (!line.text.empty()) {
            out.progress = 0.0; // 简化: 整行一次性显示
        }
    }

    return out;
}

// ---- LRC 解析(备用方案) ----
std::vector<LyricsParser::LrcLine> LyricsParser::ParseLRC(const std::string& lrcContent) {
    std::vector<LrcLine> result;
    static const std::regex pattern(R"(\[(\d{2}):(\d{2})\.(\d{2,3})\](.*))");
    std::istringstream stream(lrcContent);
    std::string line;
    std::smatch match;
    while (std::getline(stream, line)) {
        if (std::regex_match(line, match, pattern)) {
            int    minutes = std::stoi(match[1].str());
            int    seconds = std::stoi(match[2].str());
            double frac    = 0.0;
            const std::string msStr = match[3].str();
            if (msStr.length() == 3) {
                frac = std::stoi(msStr) / 1000.0;
            } else {
                frac = std::stoi(msStr) / 100.0;
            }
            result.push_back({minutes * 60 + seconds + frac, match[4].str()});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const LrcLine& a, const LrcLine& b) { return a.timeSec < b.timeSec; });
    return result;
}

} // namespace moekoe
