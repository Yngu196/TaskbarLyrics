// SPDX-License-Identifier: GPL-2.0
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
    LARGE_INTEGER freq, counter;
    ::QueryPerformanceFrequency(&freq);
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
    // characters 数组中第一个 startTime <= currentTime*1000 的行即为匹配
    // 要求 characters 非空,且按 startTime 升序(原数据通常如此)
    const int64_t tMs = static_cast<int64_t>(currentTimeSec * 1000.0);
    const int n = static_cast<int>(lyrics_.lines.size());
    int lo = 0, hi = n - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const auto& chars = lyrics_.lines[mid].characters;
        if (chars.empty()) {
            lo = mid + 1;
            continue;
        }
        const int64_t startMs = chars.front().startTime;
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
    out.currentTime = state_.currentTime;

    // ── 本地时钟死推算：播放状态下用本地时间插值 currentTime ──
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

    if (!lyrics_.valid || lyrics_.lines.empty()) {
        out.hasLyrics = false;
        return out;
    }
    out.hasLyrics = true;

    const int idx = FindLineIndex(effectiveTime);
    if (idx < 0) {
        // 进度在第一行之前,显示空状态
        return out;
    }

    const auto& line = lyrics_.lines[idx];
    out.currentLineIndex = idx;
    out.currentLine      = line.text;
    out.currentTranslated= line.translated;

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
            // 最后一个字符: 进度 = (已唱字符数) / 总字符数
            out.progress = static_cast<double>(chars.size()) /
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
