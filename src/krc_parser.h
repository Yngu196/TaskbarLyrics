// SPDX-License-Identifier: GPL-3.0
// krc_parser.h - KuGou KRC 歌词格式解析器
//
// 职责:
//   - 解析 KuGou krc 格式字符串为 LyricsData
//   - 支持字符级时间轴 <start,dur,flag>char
//   - 支持 [language:...] 标签的翻译提取（Base64 + JSON）
//   - 内置 DoS 防护：限制单行字符数与歌词总行数
//
// 该模块从 websocket_client.cpp 提取，便于独立测试与复用。
//
#pragma once

#include "lyrics_data.h"

#include <string>

namespace moekoe {

// 解析 KuGou krc 格式字符串为 LyricsData
// 输入: krcText - 原始 KRC 文本（含 [ti:...] 元数据、[startMs,dur] 行时间戳、
//                  <charStart,charDur,flag>char 字符级时间轴、[language:base64] 翻译）
// 输出: LyricsData，空输入或无有效行时 valid=false
// 安全: 单行字符数超过 MAX_CHARS_PER_LINE 时截断；总行数超过 MAX_LYRIC_LINES 时停止
LyricsData ParseKrcString(const std::string& krcText);

} // namespace moekoe
