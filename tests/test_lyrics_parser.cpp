// SPDX-License-Identifier: GPL-3.0
// test_lyrics_parser.cpp - 歌词解析核心逻辑单元测试
//
// 覆盖：
//   - FindLineIndex：二分查找边界
//   - ParseLRC：LRC 文本解析
//   - GetCurrentRenderState：KRC 数据集成验证

#include <catch2/catch_all.hpp>

#include "lyrics_data.h"

#include "lyrics_parser.h"

#include <vector>
#include <string>

// Catch2 v3 手动提供 main（避免静态库 main 被链接器丢弃）
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

using namespace moekoe;

// ──── 辅助函数：构造测试用 LyricsData ────

static LyricsData MakeData(const std::vector<int64_t>& startTimes,
                           const std::vector<std::string>& texts = {}) {
    LyricsData data;
    data.valid = true;
    for (size_t i = 0; i < startTimes.size(); ++i) {
        LyricLine line;
        line.startTime = startTimes[i];
        line.text = (i < texts.size()) ? texts[i] : ("Line " + std::to_string(i));
        data.lines.push_back(line);
    }
    return data;
}

// ──── FindLineIndex 二分查找测试 ────

TEST_CASE("FindLineIndex - empty lines returns -1", "[FindLineIndex]") {
    LyricsParser parser;
    REQUIRE(parser.FindLineIndex(0.0) == -1);
    REQUIRE(parser.FindLineIndex(10.0) == -1);
}

TEST_CASE("FindLineIndex - single element exact match", "[FindLineIndex]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({1000}));
    REQUIRE(parser.FindLineIndex(1.0) == 0);
}

TEST_CASE("FindLineIndex - single element time before", "[FindLineIndex]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({5000}));
    // 500ms < 5000ms, 没有任何 startTime <= 500ms
    REQUIRE(parser.FindLineIndex(0.5) == -1);
}

TEST_CASE("FindLineIndex - single element time after", "[FindLineIndex]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({1000}));
    // 2000ms > 1000ms, startTime <= tMs 成立，best=0
    REQUIRE(parser.FindLineIndex(2.0) == 0);
}

TEST_CASE("FindLineIndex - exact match in multi-element", "[FindLineIndex]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({0, 2000, 5000, 10000}));
    REQUIRE(parser.FindLineIndex(2.0) == 1);
    REQUIRE(parser.FindLineIndex(5.0) == 2);
}

TEST_CASE("FindLineIndex - time between two elements", "[FindLineIndex]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({0, 3000, 6000}));
    // 4.0s = 4000ms, 3000 <= 4000 < 6000, best = 1
    REQUIRE(parser.FindLineIndex(4.0) == 1);
}

TEST_CASE("FindLineIndex - time before first element", "[FindLineIndex]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({3000, 6000}));
    // 1.0s = 1000ms, 没有任何 startTime <= 1000ms
    REQUIRE(parser.FindLineIndex(1.0) == -1);
}

TEST_CASE("FindLineIndex - time after last element", "[FindLineIndex]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({1000, 3000, 6000}));
    // 10.0s = 10000ms, 所有 startTime <= 10000ms, best = 2
    REQUIRE(parser.FindLineIndex(10.0) == 2);
}

TEST_CASE("FindLineIndex - large dataset binary search correctness", "[FindLineIndex]") {
    LyricsParser parser;
    std::vector<int64_t> times;
    for (int i = 0; i < 100; ++i) {
        times.push_back(static_cast<int64_t>(i) * 1000); // 0, 1000, 2000, ..., 99000
    }
    parser.UpdateLyrics(MakeData(times));
    REQUIRE(parser.FindLineIndex(0.0) == 0);        // 第一行
    REQUIRE(parser.FindLineIndex(50.0) == 50);       // 精确匹配
    REQUIRE(parser.FindLineIndex(99.0) == 99);       // 最后一行的精确
    REQUIRE(parser.FindLineIndex(150.0) == 99);      // 超过最后
    REQUIRE(parser.FindLineIndex(50.5) == 50);       // 介于 50-51 秒间
    REQUIRE(parser.FindLineIndex(-1.0) == -1);       // 负数时间
}

// ──── FindLineIndex with characters (KRC) ────

TEST_CASE("FindLineIndex - KRC characters preferred over line startTime", "[FindLineIndex]") {
    LyricsParser parser;
    LyricsData data;
    data.valid = true;
    {
        LyricLine line;
        line.startTime = 99999; // 行级时间戳很大，但 characters 有精确时间
        line.text = "hello";
        line.characters.push_back({"h", 1000, 1100});
        line.characters.push_back({"e", 1200, 1300});
        data.lines.push_back(line);
    }
    parser.UpdateLyrics(data);
    // 1.05s = 1050ms, characters[0].startTime=1000 <= 1050, 行级 99999 > 1050
    // 如果走 characters，best=0; 如果走行级 startTime，best=-1
    REQUIRE(parser.FindLineIndex(1.05) == 0);
}

TEST_CASE("FindLineIndex - KRC multi-line with characters", "[FindLineIndex]") {
    LyricsParser parser;
    LyricsData data;
    data.valid = true;

    // Line 0: characters at 0-500, 500-1000
    {
        LyricLine line;
        line.startTime = 0;
        line.text = "AB";
        line.characters.push_back({"A", 0, 500});
        line.characters.push_back({"B", 500, 1000});
        data.lines.push_back(line);
    }
    // Line 1: characters at 2000-2500, 2500-3000
    {
        LyricLine line;
        line.startTime = 2000;
        line.text = "CD";
        line.characters.push_back({"C", 2000, 2500});
        line.characters.push_back({"D", 2500, 3000});
        data.lines.push_back(line);
    }

    parser.UpdateLyrics(data);
    REQUIRE(parser.FindLineIndex(0.3) == 0);    // 在第一行字符范围内
    REQUIRE(parser.FindLineIndex(2.3) == 1);    // 在第二行字符范围内
    REQUIRE(parser.FindLineIndex(1.5) == 0);    // 介于两行之间，应匹配上行
}

// ──── ParseLRC 测试 ────

TEST_CASE("ParseLRC - standard format with 2-digit ms", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC("[00:12.34]Hello World\n[00:15.67]Second Line\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].timeSec == Catch::Approx(12.34));
    REQUIRE(result[0].text == "Hello World");
    REQUIRE(result[1].timeSec == Catch::Approx(15.67));
    REQUIRE(result[1].text == "Second Line");
}

TEST_CASE("ParseLRC - 3-digit milliseconds", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC("[01:23.456]Three digit ms\n");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].timeSec == Catch::Approx(83.456));
    REQUIRE(result[0].text == "Three digit ms");
}

TEST_CASE("ParseLRC - mixed 2-digit and 3-digit ms", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC(
        "[00:00.500]First\n[00:01.25]Second\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].timeSec == Catch::Approx(0.5));
    REQUIRE(result[1].timeSec == Catch::Approx(1.25));
}

TEST_CASE("ParseLRC - empty content", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC("");
    REQUIRE(result.empty());
}

TEST_CASE("ParseLRC - only metadata tags", "[ParseLRC]") {
    // 元数据标签不含毫秒时间戳，应全部被过滤
    auto result = LyricsParser::ParseLRC("[ti:Title]\n[ar:Artist]\n[al:Album]\n[by:Author]\n");
    REQUIRE(result.empty());
}

TEST_CASE("ParseLRC - mixed metadata and lyrics", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC(
        "[ti:Title]\n[ar:Artist]\n[00:05.00]First lyric\n[00:10.00]Second lyric\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].text == "First lyric");
    REQUIRE(result[1].text == "Second lyric");
}

TEST_CASE("ParseLRC - lines sorted by time", "[ParseLRC]") {
    // 乱序输入，输出应按时间排序
    auto result = LyricsParser::ParseLRC(
        "[00:20.00]Later\n[00:05.00]Earlier\n[00:10.00]Middle\n");
    REQUIRE(result.size() == 3);
    REQUIRE(result[0].timeSec == Catch::Approx(5.0));
    REQUIRE(result[1].timeSec == Catch::Approx(10.0));
    REQUIRE(result[2].timeSec == Catch::Approx(20.0));
}

TEST_CASE("ParseLRC - empty lyric text after timestamp", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC("[00:10.00]\n[00:20.00]Has text\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].text == "");
    REQUIRE(result[0].timeSec == Catch::Approx(10.0));
    REQUIRE(result[1].text == "Has text");
}

TEST_CASE("ParseLRC - minutes with leading zero", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC("[03:45.00]Three minutes\n");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].timeSec == Catch::Approx(225.0));
}

TEST_CASE("ParseLRC - malformed lines skipped", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC(
        "garbage line\n[00:01.00]Valid\nnot a timestamp\n[00:02.00]Also valid\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].text == "Valid");
    REQUIRE(result[1].text == "Also valid");
}

TEST_CASE("ParseLRC - single newline only", "[ParseLRC]") {
    auto result = LyricsParser::ParseLRC("\n");
    REQUIRE(result.empty());
}

TEST_CASE("ParseLRC - lyrics with translated lines", "[ParseLRC]") {
    // 带翻译行（如 QQ 音乐的 [offset:0] 标注或无时间戳翻译行）
    auto result = LyricsParser::ParseLRC(
        "[00:15.00]Hello\n翻译行无时间戳\n[00:20.00]World\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].text == "Hello");
    REQUIRE(result[1].text == "World");
}

TEST_CASE("ParseLRC - bilingual with same timestamp (dual-line)", "[ParseLRC]") {
    // 相同时间戳的相邻行：第一行原文，第二行翻译
    auto result = LyricsParser::ParseLRC(
        "[00:15.00]Hello\n[00:15.00]你好\n[00:20.00]World\n[00:20.00]世界\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].text == "Hello");
    REQUIRE(result[0].translated == "你好");
    REQUIRE(result[1].text == "World");
    REQUIRE(result[1].translated == "世界");
}

TEST_CASE("ParseLRC - bilingual with slash separator", "[ParseLRC]") {
    // 斜杠分隔格式: "原文 / 翻译"
    auto result = LyricsParser::ParseLRC(
        "[00:10.00]Hello / 你好\n[00:20.00]World / 世界\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].text == "Hello");
    REQUIRE(result[0].translated == "你好");
    REQUIRE(result[1].text == "World");
    REQUIRE(result[1].translated == "世界");
}

TEST_CASE("ParseLRC - bilingual mixed modes", "[ParseLRC]") {
    // 混合格式：斜杠分隔 + 无翻译
    auto result = LyricsParser::ParseLRC(
        "[00:10.00]Hello / 你好\n[00:20.00]No translation\n");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].text == "Hello");
    REQUIRE(result[0].translated == "你好");
    REQUIRE(result[1].text == "No translation");
    REQUIRE(result[1].translated.empty());
}

// ──── GetCurrentRenderState 集成测试 ────

TEST_CASE("GetCurrentRenderState - no lyrics returns empty state", "[GetCurrentRenderState]") {
    LyricsParser parser;
    auto state = parser.GetCurrentRenderState();
    REQUIRE(state.hasLyrics == false);
}

TEST_CASE("GetCurrentRenderState - with lyrics but no player state", "[GetCurrentRenderState]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({0, 2000, 4000}, {"A", "B", "C"}));
    // 未设置 PlayerState，currentTime 默认为 0.0
    auto state = parser.GetCurrentRenderState();
    REQUIRE(state.hasLyrics == true);
    REQUIRE(state.currentLineIndex == 0);
    REQUIRE(state.currentLine == "A");
    REQUIRE(state.nextLine == "B");
}

TEST_CASE("GetCurrentRenderState - progress before first line falls back", "[GetCurrentRenderState]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({5000, 10000}, {"A", "B"}));
    PlayerState ps;
    ps.isPlaying = false;
    ps.currentTime = 2.0; // 在第一个时间戳之前
    parser.UpdatePlayerState(ps);
    auto state = parser.GetCurrentRenderState();
    // FindLineIndex 返回 -1，代码兜底返回第一行
    REQUIRE(state.currentLineIndex == 0);
    REQUIRE(state.currentLine == "A");
}

TEST_CASE("GetCurrentRenderState - KRC character progress calculation", "[GetCurrentRenderState]") {
    LyricsParser parser;
    // 构造 KRC 数据：一行 3 个字符，每个 1000ms
    // Char 0: 1000-2000, Char 1: 2000-3000, Char 2: 3000-4000
    LyricsData data;
    data.valid = true;
    {
        LyricLine line;
        line.startTime = 1000;
        line.text = "ABC";
        line.characters.push_back({"A", 1000, 2000});
        line.characters.push_back({"B", 2000, 3000});
        line.characters.push_back({"C", 3000, 4000});
        data.lines.push_back(line);
    }
    parser.UpdateLyrics(data);

    PlayerState ps;
    ps.isPlaying = false;
    ps.currentTime = 2.5; // 2500ms: 在第二个字符 (B) 的中间
    parser.UpdatePlayerState(ps);

    auto state = parser.GetCurrentRenderState();
    REQUIRE(state.currentLineIndex == 0);
    REQUIRE(state.currentLine == "ABC");
    // progress = (1 + 0.5) / 3 = 0.5
    REQUIRE(state.progress == Catch::Approx(0.5));
}

TEST_CASE("GetCurrentRenderState - KRC last character progress", "[GetCurrentRenderState]") {
    LyricsParser parser;
    LyricsData data;
    data.valid = true;
    {
        LyricLine line;
        line.startTime = 0;
        line.text = "AB";
        line.characters.push_back({"A", 0, 1000});
        line.characters.push_back({"B", 1000, 2000});
        data.lines.push_back(line);
    }
    parser.UpdateLyrics(data);

    {
        PlayerState ps;
        ps.isPlaying = false;
        ps.currentTime = 1.5; // 在最后一个字符中间
        parser.UpdatePlayerState(ps);
        auto state = parser.GetCurrentRenderState();
        // progress = (1 + 0.5) / 2 = 0.75
        REQUIRE(state.progress == Catch::Approx(0.75));
    }

    {
        PlayerState ps;
        ps.isPlaying = false;
        ps.currentTime = 3.0; // 超过所有字符
        parser.UpdatePlayerState(ps);
        auto state = parser.GetCurrentRenderState();
        // 最后一个字符，clamp 到 1.0 → (1 + 1.0) / 2 = 1.0
        REQUIRE(state.progress == Catch::Approx(1.0));
    }
}

TEST_CASE("GetCurrentRenderState - pure music detection", "[GetCurrentRenderState]") {
    LyricsParser parser;
    LyricsData data;
    data.valid = true;
    data.lines.push_back({"纯音乐，请欣赏", "", 0, {}});
    parser.UpdateLyrics(data);

    auto state = parser.GetCurrentRenderState();
    REQUIRE(state.hasLyrics == false);
}

TEST_CASE("GetCurrentRenderState - nextLine preview", "[GetCurrentRenderState]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({0, 2000, 4000}, {"First", "Second", "Third"}));

    PlayerState ps;
    ps.isPlaying = false;
    ps.currentTime = 1.0;
    parser.UpdatePlayerState(ps);

    auto state = parser.GetCurrentRenderState();
    REQUIRE(state.currentLine == "First");
    REQUIRE(state.nextLine == "Second");
}

TEST_CASE("GetCurrentRenderState - last line has no nextLine", "[GetCurrentRenderState]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({0, 2000, 4000}, {"First", "Second", "Third"}));

    PlayerState ps;
    ps.isPlaying = false;
    ps.currentTime = 5.0; // 在最后一行之后
    parser.UpdatePlayerState(ps);

    auto state = parser.GetCurrentRenderState();
    REQUIRE(state.currentLineIndex == 2);
    REQUIRE(state.nextLine == "");
}

TEST_CASE("GetCurrentRenderState - cover art and song name propagation", "[GetCurrentRenderState]") {
    LyricsParser parser;
    parser.UpdateLyrics(MakeData({0}, {"Song"}));

    PlayerState ps;
    ps.isPlaying = false;
    ps.currentTime = 1.0;
    ps.coverArtUrl = "https://example.com/cover.jpg";
    ps.songName = "Test Song";
    parser.UpdatePlayerState(ps);

    auto state = parser.GetCurrentRenderState();
    REQUIRE(state.coverArtUrl == "https://example.com/cover.jpg");
    REQUIRE(state.songName == "Test Song");
}

TEST_CASE("HasLyrics and IsPlaying", "[LyricsParser]") {
    LyricsParser parser;
    REQUIRE(parser.HasLyrics() == false);
    REQUIRE(parser.IsPlaying() == false);

    parser.UpdateLyrics(MakeData({0}, {"Test"}));
    REQUIRE(parser.HasLyrics() == true);

    PlayerState ps;
    ps.isPlaying = true;
    parser.UpdatePlayerState(ps);
    REQUIRE(parser.IsPlaying() == true);
}
