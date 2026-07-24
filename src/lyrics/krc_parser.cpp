// SPDX-License-Identifier: GPL-3.0
// krc_parser.cpp - KuGou KRC 歌词格式解析器实现
//
// 从 websocket_client.cpp 提取，逻辑保持一致。
//
#include "lyrics/krc_parser.h"

#include "core/constants.h"
#include "util/logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace moekoe {

using json = nlohmann::json;

namespace {
// 不抛异常的整数解析，失败返回 false
// 用 std::from_chars 替代 std::stoll，避免 first-chance exception 干扰调试器
bool ParseInt64(std::string_view s, int64_t& out) {
    const auto* first = s.data();
    const auto* last  = s.data() + s.size();
    // 跳过前导空白
    while (first < last && (*first == ' ' || *first == '\t')) ++first;
    if (first == last) return false;
    const auto res = std::from_chars(first, last, out);
    return res.ec == std::errc{} && res.ptr == last;
}
} // namespace

// 解析 KuGou krc 格式字符串为 LyricsData
LyricsData ParseKrcString(const std::string& krcText) {
    LyricsData data;
    if (krcText.empty()) {
        return data;
    }

    // 标准化换行符: \r\n -> \n, 单独的 \r -> \n
    std::string normalized;
    normalized.reserve(krcText.size());
    for (size_t i = 0; i < krcText.size(); ++i) {
        if (krcText[i] == '\r') {
            normalized += '\n';
            if (i + 1 < krcText.size() && krcText[i + 1] == '\n') {
                ++i; // 跳过 \r\n 中的 \n
            }
        } else {
            normalized += krcText[i];
        }
    }

    std::istringstream stream(normalized);
    std::string line;
    int lineCount = 0;
    int skippedMeta = 0;
    int parsedLines = 0;

    while (std::getline(stream, line)) {
        ++lineCount;
        if (line.empty()) continue;
        if (line[0] != '[') {
            continue;
        }

        // 跳过元数据头: [ti:...], [ar:...], [al:...], [by:...], [offset:...]
        if (line.size() > 2 && !std::isdigit(static_cast<unsigned char>(line[1]))) {
            ++skippedMeta;
            continue;
        }

        // 找到 ']' 提取时间戳
        auto closeBracket = line.find(']');
        if (closeBracket == std::string::npos || closeBracket < 3) {
            continue;
        }

        // 解析 [startMs,duration]
        std::string timingStr = line.substr(1, closeBracket - 1);
        auto commaPos = timingStr.find(',');
        if (commaPos == std::string::npos) {
            continue;
        }

        int64_t lineStartMs = 0;
        if (!ParseInt64(timingStr.substr(0, commaPos), lineStartMs)) {
            continue;
        }

        // 解析字符级时间轴: <charStart,charDuration,flag>char
        std::string content = line.substr(closeBracket + 1);
        LyricLine lyricLine;
        std::string fullText;

        size_t pos = 0;
        int charCount = 0;
        while (pos < content.size()) {
            // 找到下一个 <
            auto openAngle = content.find('<', pos);
            if (openAngle == std::string::npos) {
                // 剩余纯文本
                fullText += content.substr(pos);
                break;
            }
            // < 前的纯文本（如果有）
            if (openAngle > pos) {
                fullText += content.substr(pos, openAngle - pos);
            }

            auto closeAngle = content.find('>', openAngle);
            if (closeAngle == std::string::npos) break;

            // 解析 <charStart,charDuration,flag>
            std::string ctStr = content.substr(openAngle + 1, closeAngle - openAngle - 1);
            auto c1 = ctStr.find(',');
            auto c2 = ctStr.find(',', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) {
                pos = closeAngle + 1;
                continue;
            }

            int64_t charStartMs = 0, charDurMs = 0;
            if (!ParseInt64(ctStr.substr(0, c1), charStartMs) ||
                !ParseInt64(ctStr.substr(c1 + 1, c2 - c1 - 1), charDurMs)) {
                pos = closeAngle + 1;
                continue;
            }

            // 提取 > 后面的字符（到下一个 < 或结尾）
            pos = closeAngle + 1;
            auto nextOpen = content.find('<', pos);
            std::string ch;
            if (pos < content.size()) {
                ch = content.substr(pos, (nextOpen == std::string::npos) ? std::string::npos : nextOpen - pos);
                if (!ch.empty()) {
                    // 防止恶意 KRC 超大 timings 数组耗尽内存
                    if (lyricLine.characters.size() >= constants::MAX_CHARS_PER_LINE) {
                        if (lyricLine.characters.size() == constants::MAX_CHARS_PER_LINE) {
                            Log("[PARSER] KRC timings array reached MAX_CHARS_PER_LINE (%d), truncating\n",
                                constants::MAX_CHARS_PER_LINE);
                        }
                        // 仍然累积文字以便正确渲染，但不再增加时间轴
                        fullText += ch;
                        pos = (nextOpen == std::string::npos) ? content.size() : nextOpen;
                        continue;
                    }
                    CharacterTiming ct;
                    ct.ch        = ch;
                    ct.startTime = lineStartMs + charStartMs;
                    ct.endTime   = ct.startTime + charDurMs;
                    lyricLine.characters.push_back(std::move(ct));
                    ++charCount;
                }
                fullText += ch;
                pos = (nextOpen == std::string::npos) ? content.size() : nextOpen;
            }
        }

        lyricLine.text = fullText;

        // 限制单行字符数，防止 DoS
        if (lyricLine.characters.size() > constants::MAX_CHARS_PER_LINE) {
            lyricLine.characters.resize(constants::MAX_CHARS_PER_LINE);
        }

        // 限制歌词总行数，防止内存耗尽
        if (data.lines.size() >= constants::MAX_LYRIC_LINES) {
            Log("[PARSER] KRC lyrics reached MAX_LYRIC_LINES (%d), truncating\n",
                constants::MAX_LYRIC_LINES);
            break;
        }

        data.lines.push_back(std::move(lyricLine));
        ++parsedLines;
    }

    // 解析 [language:...] 标签提取翻译
    std::string langLine;
    {
        std::istringstream langStream(normalized);
        std::string l;
        while (std::getline(langStream, l)) {
            if (l.size() > 10 && l[0] == '[' && l[1] == 'l' && l.substr(0, 10) == "[language:") {
                langLine = l;
                break;
            }
        }
    }
    if (!langLine.empty()) {
        // 提取 Base64 内容: [language:xxxx]
        auto colonPos = langLine.find(':');
        auto closePos = langLine.find(']', colonPos);
        if (colonPos != std::string::npos && closePos != std::string::npos) {
            std::string b64 = langLine.substr(colonPos + 1, closePos - colonPos - 1);
            // 补齐 padding
            while (b64.size() % 4 != 0) b64 += '=';
            // URL-safe Base64 还原
            for (auto& c : b64) {
                if (c == '-') c = '+';
                else if (c == '_') c = '/';
            }
            // 手动 Base64 解码
            static const char* kBase64Table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::vector<uint8_t> decoded;
            decoded.reserve(b64.size() * 3 / 4);
            for (size_t i = 0; i < b64.size(); i += 4) {
                int vals[4] = {-1, -1, -1, -1};
                for (int j = 0; j < 4 && (i + j) < b64.size(); ++j) {
                    char c = b64[i + j];
                    // 跳过 padding 字符
                    if (c == '=') continue;
                    const char* p = strchr(kBase64Table, c);
                    if (p) {
                        vals[j] = (int)(p - kBase64Table);
                    } else {
                        moekoe::Log("[PARSER] Invalid Base64 character '%c' at position %zu\n", c, i + j);
                    }
                }
                if (vals[0] >= 0 && vals[1] >= 0) decoded.push_back((uint8_t)((vals[0] << 2) | (vals[1] >> 4)));
                if (vals[1] >= 0 && vals[2] >= 0) decoded.push_back((uint8_t)((vals[1] << 4) | (vals[2] >> 2)));
                if (vals[2] >= 0 && vals[3] >= 0) decoded.push_back((uint8_t)((vals[2] << 6) | vals[3]));
            }
            // 解析 JSON（使用 allow_exceptions=false 避免抛出 parse_error，
            // 否则 VS 调试器会在 first-chance exception 处中断，干扰测试调试）
            std::string jsonStr(decoded.begin(), decoded.end());
            json langJson = json::parse(jsonStr, nullptr, false);
            if (!langJson.is_discarded() &&
                langJson.contains("content") && langJson["content"].is_array()) {
                for (const auto& section : langJson["content"]) {
                    if (section.value("type", -1) == 1 && section.contains("lyricContent") && section["lyricContent"].is_array()) {
                        const auto& lc = section["lyricContent"];
                        size_t maxIdx = (std::min)(lc.size(), data.lines.size());
                        for (size_t idx = 0; idx < maxIdx; ++idx) {
                            if (lc[idx].is_array() && !lc[idx].empty() && lc[idx][0].is_string()) {
                                data.lines[idx].translated = lc[idx][0].get<std::string>();
                            }
                        }
                        break; // 只取第一个 type=1 的翻译
                    }
                }
            }
        }
    }

    data.valid = !data.lines.empty();
    return data;
}

} // namespace moekoe
