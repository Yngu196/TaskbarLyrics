// SPDX-License-Identifier: GPL-2.0
// websocket_client.cpp - WebSocket 客户端实现
#include "websocket_client.h"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace moekoe {

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

// 前向声明
void DebugLog(const std::string& msg);

// 解析 KuGou krc 格式字符串为 LyricsData
LyricsData ParseKrc(const std::string& krcText) {
    LyricsData data;
    if (krcText.empty()) {
        DebugLog("ParseKrc: input empty");
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

    DebugLog("ParseKrc: input len=" + std::to_string(krcText.size()) +
             " normalized_len=" + std::to_string(normalized.size()));

    std::istringstream stream(normalized);
    std::string line;
    int lineCount = 0;
    int skippedMeta = 0;
    int parsedLines = 0;

    while (std::getline(stream, line)) {
        ++lineCount;
        if (line.empty()) continue;
        if (line[0] != '[') {
            if (lineCount <= 10) {
                DebugLog("ParseKrc: line#" + std::to_string(lineCount) +
                         " not start with [, first20=" + line.substr(0, 20));
            }
            continue;
        }

        // 跳过元数据头: [ti:...], [ar:...], [al:...], [by:...], [offset:...]
        if (line.size() > 2 && !std::isdigit(static_cast<unsigned char>(line[1]))) {
            ++skippedMeta;
            if (skippedMeta <= 5) {
                DebugLog("ParseKrc: skip meta line#" + std::to_string(lineCount) +
                         " tag=" + line.substr(0, std::min<size_t>(20, line.size())));
            }
            continue;
        }

        DebugLog("ParseKrc: processing lyric line#" + std::to_string(lineCount) +
                 " first40=" + line.substr(0, std::min<size_t>(40, line.size())));

        // 找到 ']' 提取时间戳
        auto closeBracket = line.find(']');
        if (closeBracket == std::string::npos || closeBracket < 3) {
            DebugLog("ParseKrc: line#" + std::to_string(lineCount) + " no close bracket");
            continue;
        }

        // 解析 [startMs,duration]
        std::string timingStr = line.substr(1, closeBracket - 1);
        auto commaPos = timingStr.find(',');
        if (commaPos == std::string::npos) {
            DebugLog("ParseKrc: line#" + std::to_string(lineCount) + " no comma in timing");
            continue;
        }

        int64_t lineStartMs = 0;
        try {
            lineStartMs = std::stoll(timingStr.substr(0, commaPos));
        } catch (...) {
            DebugLog("ParseKrc: line#" + std::to_string(lineCount) + " stoll failed");
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
            try {
                charStartMs = std::stoll(ctStr.substr(0, c1));
                charDurMs   = std::stoll(ctStr.substr(c1 + 1, c2 - c1 - 1));
            } catch (...) {
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
        data.lines.push_back(std::move(lyricLine));
        ++parsedLines;

        if (parsedLines <= 3) {
            DebugLog("ParseKrc: parsed line#" + std::to_string(parsedLines) +
                     " text='" + fullText + "' chars=" + std::to_string(charCount));
        }
    }

    DebugLog("ParseKrc: total_lines=" + std::to_string(lineCount) +
             " skipped_meta=" + std::to_string(skippedMeta) +
             " parsed=" + std::to_string(parsedLines) +
             " data_lines=" + std::to_string(data.lines.size()));

    data.valid = !data.lines.empty();
    return data;
}

// 重连退避时间
int BackoffSeconds(int attempt) {
    if (attempt <= 0) return 1;
    if (attempt == 1) return 1;
    if (attempt == 2) return 2;
    if (attempt == 3) return 4;
    if (attempt == 4) return 8;
    return 15;
}

void DebugLog(const std::string& msg) {
    // 写到项目目录 debug.log
    const char* logPath = "D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log";
    if (FILE* f = fopen(logPath, "a")) {
        fprintf(f, "[%llu] %s\n", GetTickCount64() % 100000, msg.c_str());
        fclose(f);
    }
}

} // namespace

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() {
    Disconnect();
}

bool WebSocketClient::Connect(const std::string& url) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        url_ = url;
    }
    stopRequested_.store(false);

    // 初始状态：未连接
    if (onStatus_) onStatus_(false);

    if (!client_) {
        client_ = std::make_unique<ix::WebSocket>();
    }

    // 启动后台重连循环（幂等）
    if (!reconnectThread_.joinable()) {
        reconnectThread_ = std::thread([this] { ReconnectLoop(); });
    } else {
        reconnectNow_.store(true);
    }
    return true;
}

void WebSocketClient::Disconnect() {
    stopRequested_.store(true);
    reconnectNow_.store(false);

    if (client_) {
        try {
            client_->stop();
        } catch (...) { /* ignore */ }
    }

    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }

    if (connected_.exchange(false)) {
        if (onStatus_) onStatus_(false);
    }
    client_.reset();
}

bool WebSocketClient::SendControl(const std::string& command) {
    if (!client_ || !connected_.load()) return false;
    json j;
    j["type"] = "control";
    j["data"] = {{"command", command}};
    auto result = client_->send(j.dump());
    return result.success;
}

void WebSocketClient::RequestReconnect() {
    reconnectNow_.store(true);
}

void WebSocketClient::ReconnectLoop() {
    int attempt = 0;
    DebugLog("ReconnectLoop started");
    while (!stopRequested_.load()) {
        // 如果已连接,持续监控
        if (connected_.load() && client_) {
            std::this_thread::sleep_for(200ms);
            continue;
        }

        // 等待退避
        const int waitSec = BackoffSeconds(attempt++);
        DebugLog("Reconnect: waiting " + std::to_string(waitSec) + "s (attempt " + std::to_string(attempt-1) + ")");
        for (int i = 0; i < waitSec * 10 && !stopRequested_.load() && !reconnectNow_.load(); ++i) {
            std::this_thread::sleep_for(100ms);
        }
        if (stopRequested_.load()) break;
        reconnectNow_.store(false);
        if (attempt > 5) attempt = 5; // 上限 15 秒

        // 取出当前 URL
        std::string urlCopy;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            urlCopy = url_;
        }
        DebugLog("Reconnect: connecting to " + urlCopy);

        // 配置客户端
        try {
            client_ = std::make_unique<ix::WebSocket>();
            client_->setUrl(urlCopy);
        } catch (...) {
            DebugLog("Reconnect: exception creating WebSocket");
            continue;
        }

        // 绑定消息回调
        auto self = this;
        client_->setOnMessageCallback(
            [self](const ix::WebSocketMessagePtr& msg) {
                if (msg->type == ix::WebSocketMessageType::Open) {
                    DebugLog("WS: opened");
                    self->connected_.store(true);
                    if (self->onStatus_) self->onStatus_(true);
                } else if (msg->type == ix::WebSocketMessageType::Close) {
                    DebugLog("WS: closed");
                    self->connected_.store(false);
                    if (self->onStatus_) self->onStatus_(false);
                } else if (msg->type == ix::WebSocketMessageType::Message) {
                    DebugLog("WS: message received (" + std::to_string(msg->str.size()) + " bytes)");
                    if (!msg->str.empty()) {
                        try {
                            self->DispatchWsMessage(msg->str);
                        } catch (const std::exception& e) {
                            DebugLog("WS: Dispatch exception: " + std::string(e.what()));
                        } catch (...) {
                            DebugLog("WS: Dispatch unknown exception");
                        }
                    }
                } else if (msg->type == ix::WebSocketMessageType::Error) {
                    DebugLog("WS: ERROR - " + msg->errorInfo.reason);
                    self->connected_.store(false);
                    if (self->onStatus_) self->onStatus_(false);
                }
            });

        // 启动（同步）—— ix::WebSocket::start 内部会启动线程
        DebugLog("Reconnect: calling client->start()");
        client_->start();
        DebugLog("Reconnect: client->start() returned");

        // 等到连接成功 / 失败 / 停止
        DebugLog("Reconnect: waiting up to 5s for connection...");
        for (int i = 0; i < 50 && !stopRequested_.load(); ++i) { // 5s 连接窗口
            if (connected_.load()) break;
            std::this_thread::sleep_for(100ms);
        }
        if (stopRequested_.load()) break;

        if (!connected_.load()) {
            DebugLog("Reconnect: connection failed after 5s");
            // 启动失败,等待下个循环重连
            try { client_->stop(); } catch (...) {}
            client_.reset();
        } else {
            DebugLog("Reconnect: connected successfully");
        }
    }
    DebugLog("ReconnectLoop ended");
}

void WebSocketClient::DispatchWsMessage(const std::string& raw) {
    json j;
    try {
        j = json::parse(raw);
    } catch (...) {
        DebugLog("Dispatch: JSON parse failed");
        return;
    }

    if (!j.contains("type")) {
        DebugLog("Dispatch: no type field in message");
        return;
    }
    const std::string type = j.value("type", "");
    DebugLog("Dispatch: type=" + type + " data_keys=" + [&]{
        std::string k;
        if (j.contains("data") && j["data"].is_object()) {
            for (auto& [key,_] : j["data"].items()) { k += key + ","; }
        } else if (j.contains("data") && j["data"].is_array()) {
            k = "array[" + std::to_string(j["data"].size()) + "]";
        }
        return k;
    }());

    if (type == "lyrics") {
        LyricsData data;
        // 实际格式: data = { currentSong, currentTime, duration, lyricsData: [...] }
        // lyricsData 可能是数组，也可能是 JSON 字符串化的数组
        json lyricsArray = json::array();
        bool hasLD = false;

        if (j.contains("data") && j["data"].is_object() && j["data"].contains("lyricsData")) {
            const auto& ld = j["data"]["lyricsData"];
            if (ld.is_array()) {
                lyricsArray = ld;
                hasLD = true;
                DebugLog("Dispatch: lyricsData is array, size=" + std::to_string(ld.size()));
            } else if (ld.is_string()) {
                std::string ldStr = ld.get<std::string>();
                DebugLog("Dispatch: lyricsData is string, len=" + std::to_string(ldStr.size()) +
                         " first100=" + ldStr.substr(0, 100));
                data = ParseKrc(ldStr);
                hasLD = data.valid;
                DebugLog("Dispatch: krc parsed, lines=" + std::to_string(data.lines.size()) +
                         " valid=" + (data.valid?"Y":"N"));
            } else {
                DebugLog("Dispatch: lyricsData unexpected type=" + std::to_string(static_cast<int>(ld.type())));
            }
        }

        if (hasLD)
        {
            // 从歌词消息中提取当前播放时间，更新播放器状态
            if (j["data"].contains("currentTime") && onState_) {
                PlayerState st;
                st.isPlaying   = true;
                st.currentTime = j["data"]["currentTime"].get<double>();
                // currentSong 可能是 string 或 object，安全提取
                if (j["data"].contains("currentSong")) {
                    const auto& cs = j["data"]["currentSong"];
                    if (cs.is_string()) {
                        st.songTitle = cs.get<std::string>();
                    } else if (cs.is_object()) {
                        // object 格式: {name, artist, ...}
                        if (cs.contains("name") && cs["name"].is_string()) {
                            st.songTitle = cs["name"].get<std::string>();
                        }
                        if (cs.contains("artist") && cs["artist"].is_string()) {
                            st.songTitle += " - " + cs["artist"].get<std::string>();
                        }
                    }
                }
                onState_(st);
            }

            // 只有 lyricsData 是数组格式时才解析 JSON 行
            // KRC 格式已经在上面 ParseKrc 中处理完毕，跳过此循环
            for (const auto& lineJson : lyricsArray) {
                LyricLine line;
                if (data.lines.empty()) {
                    std::string sample = lineJson.dump();
                    if (sample.size() > 300) sample = sample.substr(0, 300) + "...";
                    DebugLog("Dispatch: first line sample: " + sample);
                }
                line.text       = lineJson.value("text",       "");
                line.translated = lineJson.value("translated", "");

                if (lineJson.contains("characters") && lineJson["characters"].is_array()) {
                    for (const auto& c : lineJson["characters"]) {
                        CharacterTiming ct;
                        ct.ch        = c.value("char",      "");
                        ct.startTime = c.value("startTime", static_cast<int64_t>(0));
                        ct.endTime   = c.value("endTime",   static_cast<int64_t>(0));
                        if (!ct.ch.empty()) {
                            line.characters.push_back(std::move(ct));
                        }
                    }
                }
                data.lines.push_back(std::move(line));
            }
        }
        data.valid = !data.lines.empty();
        DebugLog("Dispatch: lyrics parsed, lines=" + std::to_string(data.lines.size()) + " valid=" + (data.valid?"Y":"N"));
        try { if (onLyrics_) onLyrics_(data); } catch (...) { DebugLog("Dispatch: onLyrics_ exception"); }
    } else if (type == "playerState") {
        PlayerState st;
        if (j.contains("data") && j["data"].is_object()) {
            const auto& d = j["data"];
            st.isPlaying   = d.value("isPlaying",   false);
            st.currentTime = d.value("currentTime", 0.0);
            st.songTitle   = d.value("songTitle",   "");
        }
        DebugLog("Dispatch: playerState received, isPlaying=" + std::to_string(st.isPlaying) + " currentTime=" + std::to_string(st.currentTime) + " song=" + st.songTitle);
        try { if (onState_) onState_(st); } catch (...) { DebugLog("Dispatch: onState_ exception"); }
    } else if (type == "welcome") {
        // 服务器欢迎消息,忽略
    } else {
        // 未知消息类型,忽略以保持前向兼容
    }
}

} // namespace moekoe
