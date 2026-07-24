// SPDX-License-Identifier: GPL-3.0
// test_krc_parser.cpp - KRC 歌词解析器单元测试
//
// 覆盖：
//   - 空输入与无效输入
//   - 元数据头跳过（[ti:...] 等）
//   - 标准 KRC 行解析（[startMs,dur] + <charStart,charDur,flag>char）
//   - 字符级时间轴累加与行起始时间偏移
//   - 纯文本（无 <> 标记）行
//   - 换行符标准化（\r\n / \r）
//   - DoS 防护：超大行/超大输入截断
//   - [language:base64] 翻译提取
//   - 残缺/畸形输入容错（无 ']'、无逗号、无 '>'）
//
#include <catch2/catch_all.hpp>

#include "lyrics/lyrics_data.h"
#include "lyrics/krc_parser.h"

#include <string>

// Catch2 v3 手动提供 main（避免静态库 main 被链接器丢弃）
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

using namespace moekoe;

// ──── 1. 空输入与无效输入 ────

TEST_CASE("ParseKrcString 空输入返回无效数据", "[krc_parser]") {
    LyricsData data = ParseKrcString("");
    REQUIRE(data.valid == false);
    REQUIRE(data.lines.empty());
}

TEST_CASE("ParseKrcString 纯空白输入返回无效数据", "[krc_parser]") {
    LyricsData data = ParseKrcString("   \n\n  ");
    REQUIRE(data.valid == false);
    REQUIRE(data.lines.empty());
}

TEST_CASE("ParseKrcString 仅含元数据头返回无效数据", "[krc_parser]") {
    // 元数据行（[ti:...]、[ar:...] 等）首字符非数字，应被跳过
    LyricsData data = ParseKrcString(
        "[ti:测试歌曲]\n"
        "[ar:测试歌手]\n"
        "[al:测试专辑]\n"
        "[by:制作人]\n"
        "[offset:100]\n");
    REQUIRE(data.valid == false);
    REQUIRE(data.lines.empty());
}

// ──── 2. 标准 KRC 行解析 ────

TEST_CASE("ParseKrcString 解析单行 KRC 含字符级时间轴", "[krc_parser]") {
    // 一行 KRC: [行起始ms,行持续ms]<字符起始ms,字符持续ms,0>字
    // 注意: KRC 格式允许 <...> 后跟多个字符作为一个 timing 单元
    std::string krc =
        "[0,2000]<0,500,0>你<500,500,0>好<1000,500,0>世<1500,500,0>界";

    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);

    const auto& line = data.lines[0];
    REQUIRE(line.text == "你好世界");
    REQUIRE(line.characters.size() == 4);

    // 字符时间 = 行起始 + 字符起始偏移
    CHECK(line.characters[0].ch == "你");
    CHECK(line.characters[0].startTime == 0);
    CHECK(line.characters[0].endTime == 500);

    CHECK(line.characters[1].ch == "好");
    CHECK(line.characters[1].startTime == 500);
    CHECK(line.characters[1].endTime == 1000);

    CHECK(line.characters[2].ch == "世");
    CHECK(line.characters[2].startTime == 1000);
    CHECK(line.characters[2].endTime == 1500);

    CHECK(line.characters[3].ch == "界");
    CHECK(line.characters[3].startTime == 1500);
    CHECK(line.characters[3].endTime == 2000);
}

TEST_CASE("ParseKrcString 行起始时间偏移正确叠加", "[krc_parser]") {
    // 行起始 1000ms，字符起始 0ms → 字符绝对时间 1000ms
    std::string krc = "[1000,3000]<0,1000,0>A<1000,1000,0>B";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    REQUIRE(data.lines[0].characters.size() == 2);

    CHECK(data.lines[0].characters[0].startTime == 1000);
    CHECK(data.lines[0].characters[0].endTime == 2000);
    CHECK(data.lines[0].characters[1].startTime == 2000);
    CHECK(data.lines[0].characters[1].endTime == 3000);
}

TEST_CASE("ParseKrcString 多行 KRC 解析顺序保持", "[krc_parser]") {
    std::string krc =
        "[0,1000]<0,500,0>第一<500,500,0>行\n"
        "[2000,1000]<0,500,0>第二<500,500,0>行\n"
        "[4000,1000]<0,1000,0>第三行";

    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 3);
    CHECK(data.lines[0].text == "第一行");
    CHECK(data.lines[1].text == "第二行");
    CHECK(data.lines[2].text == "第三行");
}

// ──── 3. 纯文本行（无 <> 标记）── ────

TEST_CASE("ParseKrcString 无字符级时间轴的行仍解析文本", "[krc_parser]") {
    // 只有 [startMs,dur] 没有 <> 标记
    std::string krc = "[5000,2000]纯文本歌词行";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].text == "纯文本歌词行");
    CHECK(data.lines[0].characters.empty());
}

TEST_CASE("ParseKrcString 混合纯文本与字符级时间轴", "[krc_parser]") {
    // < 前有纯文本，< 后有字符
    std::string krc = "[0,1000]前缀<0,500,0>字";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].text == "前缀字");
    REQUIRE(data.lines[0].characters.size() == 1);
    CHECK(data.lines[0].characters[0].ch == "字");
}

// ──── 4. 换行符标准化 ────

TEST_CASE("ParseKrcString 标准化 CRLF 换行符", "[krc_parser]") {
    std::string krc = "[0,1000]<0,1000,0>A\r\n[2000,1000]<0,1000,0>B";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 2);
    CHECK(data.lines[0].text == "A");
    CHECK(data.lines[1].text == "B");
}

TEST_CASE("ParseKrcString 标准化单独 CR 换行符", "[krc_parser]") {
    std::string krc = "[0,1000]<0,1000,0>A\r[2000,1000]<0,1000,0>B";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 2);
    CHECK(data.lines[0].text == "A");
    CHECK(data.lines[1].text == "B");
}

// ──── 5. 畸形输入容错 ────

TEST_CASE("ParseKrcString 缺少 ']' 的行被跳过", "[krc_parser]") {
    std::string krc = "[0,1000<0,1000,0>A\n[2000,1000]<0,1000,0>B";
    LyricsData data = ParseKrcString(krc);

    // 第一行缺 ']' 被跳过，只解析第二行
    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].text == "B");
}

TEST_CASE("ParseKrcString 时间戳无逗号被跳过", "[krc_parser]") {
    // [0 1000] 没有逗号分隔
    std::string krc = "[0 1000]<0,1000,0>A\n[2000,1000]<0,1000,0>B";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].text == "B");
}

TEST_CASE("ParseKrcString 缺少 '>' 的字符标记正常终止", "[krc_parser]") {
    // < 后无 > ，应终止解析该行字符级时间轴
    std::string krc = "[0,1000]<0,1000,0>A<2000,1000,0B";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    // 第一个字符正常解析，第二个 < 无 > 应终止
    CHECK(data.lines[0].characters.size() == 1);
    CHECK(data.lines[0].characters[0].ch == "A");
}

TEST_CASE("ParseKrcString 非数字时间戳被跳过", "[krc_parser]") {
    // 时间戳不是数字
    std::string krc = "[abc,1000]<0,1000,0>A\n[2000,1000]<0,1000,0>B";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].text == "B");
}

TEST_CASE("ParseKrcString 非 KRC 行（不以 [ 开头）被跳过", "[krc_parser]") {
    std::string krc =
        "这是一行普通文本\n"
        "[0,1000]<0,1000,0>有效行\n"
        "另一行普通文本";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].text == "有效行");
}

// ──── 6. DoS 防护 ────

TEST_CASE("ParseKrcString 超大行不崩溃且截断字符数", "[krc_parser]") {
    // 构造一个远超 MAX_CHARS_PER_LINE 的行
    std::string krc = "[0,1000000]";
    for (int i = 0; i < 2000; ++i) {
        krc += "<" + std::to_string(i * 10) + ",10,0>A";
    }
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    // 应被截断到 MAX_CHARS_PER_LINE
    CHECK(data.lines[0].characters.size() <= 1000);
    // 文本仍应累积（渲染需要）
    CHECK(data.lines[0].text.size() > 100);
}

// ──── 7. 翻译提取 ────

TEST_CASE("ParseKrcString 无 language 标签时 translated 为空", "[krc_parser]") {
    std::string krc = "[0,1000]<0,1000,0>Hello";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].translated.empty());
}

TEST_CASE("ParseKrcString 无效 Base64 的 language 标签不崩溃", "[krc_parser]") {
    // 畸形 Base64，应被静默忽略
    std::string krc =
        "[0,1000]<0,1000,0>Hello\n"
        "[language:!!!invalid_base64!!!]";
    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].text == "Hello");
    // 翻译解析失败，translated 保持为空
    CHECK(data.lines[0].translated.empty());
}

TEST_CASE("ParseKrcString 有效 language 标签提取翻译", "[krc_parser]") {
    // 构造一个有效的 [language:base64] 标签
    // JSON 内容: {"content":[{"type":1,"lyricContent":[["Translation"]]}]}
    std::string jsonStr = R"({"content":[{"type":1,"lyricContent":[["你好"]]}]})";

    // 标准 Base64 编码
    static const char* kBase64Table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto base64_encode = [&](const std::string& in) -> std::string {
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : in) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(kBase64Table[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(kBase64Table[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    };

    std::string b64 = base64_encode(jsonStr);

    std::string krc =
        "[0,1000]<0,1000,0>Hello\n"
        "[language:" + b64 + "]";

    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 1);
    CHECK(data.lines[0].text == "Hello");
    CHECK(data.lines[0].translated == "你好");
}

// ──── 8. 综合：完整 KRC 文档 ────

TEST_CASE("ParseKrcString 完整 KRC 文档解析", "[krc_parser]") {
    std::string krc =
        "[ti:测试歌曲]\n"
        "[ar:测试歌手]\n"
        "[al:测试专辑]\n"
        "[by:制作人]\n"
        "[offset:0]\n"
        "\n"
        "[0,2000]<0,500,0>你<500,500,0>好<1000,1000,0>世<1500,500,0>界\n"
        "[2000,2000]<0,1000,0>第<1000,1000,0>二<1500,500,0>行\n"
        "\n"
        "[4000,1000]纯文本行";

    LyricsData data = ParseKrcString(krc);

    REQUIRE(data.valid == true);
    REQUIRE(data.lines.size() == 3);

    CHECK(data.lines[0].text == "你好世界");
    CHECK(data.lines[0].characters.size() == 4);

    CHECK(data.lines[1].text == "第二行");
    CHECK(data.lines[1].characters.size() == 3);

    CHECK(data.lines[2].text == "纯文本行");
    CHECK(data.lines[2].characters.empty());
}
