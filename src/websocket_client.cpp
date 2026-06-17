// SPDX-License-Identifier: GPL-2.0
// websocket_client.cpp - WebSocket 客户端实现
#include "websocket_client.h"
#include "api_enabler.h"
#include "constants.h"
#include "logger.h"

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

// 重连退避时间
int BackoffSeconds(int attempt) {
    if (attempt <= 0) return 1;
    if (attempt == 1) return 1;
    if (attempt == 2) return 2;
    if (attempt == 3) return 4;
    if (attempt == 4) return 8;
    return 15;
}

} // namespace

// 解析 KuGou krc 格式字符串为 LyricsData（公共静态方法，供 HTTP API 等外部调用）
LyricsData WebSocketClient::ParseKrcString(const std::string& krcText) {
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
        try {
            lineStartMs = std::stoll(timingStr.substr(0, commaPos));
        } catch (...) {
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

        // 限制单行字符数，防止 DoS
        if (lyricLine.characters.size() > constants::MAX_CHARS_PER_LINE) {
            lyricLine.characters.resize(constants::MAX_CHARS_PER_LINE);
        }

        data.lines.push_back(std::move(lyricLine));
        ++parsedLines;

        // 限制歌词总行数，防止内存耗尽
        if (data.lines.size() >= constants::MAX_LYRIC_LINES) {
            break;
        }
    }

    data.valid = !data.lines.empty();
    return data;
}

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

    // 协作式等待：ReconnectLoop 内部已使用短间隔轮询 stopRequested_，
    // 理论上应在 ~100ms 内退出。保留超时作为安全网。
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
    Log("ReconnectLoop started");
    while (!stopRequested_.load()) {
        // 如果已连接,持续监控（短间隔以快速响应 stopRequested_）
        if (connected_.load() && client_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // 等待退避
        const int waitSec = BackoffSeconds(attempt++);
        Log("Reconnect: waiting " + std::to_string(waitSec) + "s (attempt " + std::to_string(attempt-1) + ")");
        for (int i = 0; i < waitSec * 10 && !stopRequested_.load() && !reconnectNow_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(constants::RECONNECT_WAIT_GRANULARITY_MS));
        }
        if (stopRequested_.load()) break;
        reconnectNow_.store(false);
        if (attempt > constants::MAX_RECONNECT_ATTEMPTS) attempt = constants::MAX_RECONNECT_ATTEMPTS; // 上限 15 秒

        // 取出当前 URL
        std::string urlCopy;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            urlCopy = url_;
        }
        Log("Reconnect: connecting to " + urlCopy);

        // 配置客户端
        try {
            client_ = std::make_unique<ix::WebSocket>();
            client_->setUrl(urlCopy);
        } catch (...) {
            Log("Reconnect: exception creating WebSocket");
            continue;
        }

        // 绑定消息回调
        auto self = this;
        client_->setOnMessageCallback(
            [self](const ix::WebSocketMessagePtr& msg) {
                if (msg->type == ix::WebSocketMessageType::Open) {
                    Log("WS: opened");
                    self->connected_.store(true);
                    if (self->onStatus_) self->onStatus_(true);
                } else if (msg->type == ix::WebSocketMessageType::Close) {
                    Log("WS: closed");
                    self->connected_.store(false);
                    if (self->onStatus_) self->onStatus_(false);
                } else if (msg->type == ix::WebSocketMessageType::Message) {
                    if (!msg->str.empty()) {
                        try {
                            self->DispatchWsMessage(msg->str);
                        } catch (const std::exception& e) {
                            Log("WS: Dispatch exception: " + std::string(e.what()));
                        } catch (...) {
                            Log("WS: Dispatch unknown exception");
                        }
                    }
                } else if (msg->type == ix::WebSocketMessageType::Error) {
                    Log("WS: ERROR - " + msg->errorInfo.reason);
                    self->connected_.store(false);
                    if (self->onStatus_) self->onStatus_(false);
                }
            });

        // 启动（同步）—— ix::WebSocket::start 内部会启动线程
        client_->start();

        // 等到连接成功 / 失败 / 停止
        for (int i = 0; i < constants::WS_CONNECT_TIMEOUT_ITERATIONS && !stopRequested_.load(); ++i) { // 5s 连接窗口
            if (connected_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(constants::RECONNECT_WAIT_GRANULARITY_MS));
        }
        if (stopRequested_.load()) break;

        if (!connected_.load()) {
            Log("Reconnect: connection failed after 5s");
            // 启动失败,等待下个循环重连
            try { client_->stop(); } catch (...) {}
            client_.reset();

            // 连续多次失败后，尝试检测并自动开启 MoeKoeMusic 的 API 模式
            // 仅在 attempt == 2（即第3次失败，约已等待 1+2=3 秒）时触发一次
            if (attempt == 2) {
                Log("Reconnect: triggering API auto-enable check...");
                auto result = ApiEnabler::TryEnableApi();
                Log("Reconnect: API auto-enable result = " + ApiEnabler::ResultToString(result));
            }
        } else {
            Log("Reconnect: connected successfully");
        }
    }
    Log("ReconnectLoop ended");
}

void WebSocketClient::DispatchWsMessage(const std::string& raw) {
    // 安全检查：拒绝过大的消息，防止内存耗尽
    if (raw.size() > constants::MAX_WS_MESSAGE_SIZE) {
        Log("[WS] Message too large: " + std::to_string(raw.size()) + " bytes (max: " + std::to_string(constants::MAX_WS_MESSAGE_SIZE) + "), discarded");
        return;
    }

    json j;
    try {
        j = json::parse(raw);
    } catch (...) {
        Log("Dispatch: JSON parse failed");
        return;
    }

    if (!j.contains("type")) {
        Log("Dispatch: no type field in message");
        return;
    }
    const std::string type = j.value("type", "");

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
            } else if (ld.is_string()) {
                std::string ldStr = ld.get<std::string>();
                data = ParseKrcString(ldStr);
                hasLD = data.valid;
            } else {
                Log("Dispatch: lyricsData unexpected type=" + std::to_string(static_cast<int>(ld.type())));
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
                        // object 格式: {name, artist, pic, ...}
                        if (cs.contains("name") && cs["name"].is_string()) {
                            st.songTitle = cs["name"].get<std::string>();
                            st.songName = cs["name"].get<std::string>();
                        }
                        if (cs.contains("artist") && cs["artist"].is_string()) {
                            st.songTitle += " - " + cs["artist"].get<std::string>();
                        }
                        // 提取专辑封面 URL（尝试已知字段名）
                        for (const auto& key : {"pic", "cover", "albumArt", "image", "poster", "img", "album_pic"}) {
                            if (cs.contains(key) && cs[key].is_string()) {
                                st.coverArtUrl = cs[key].get<std::string>();
                                Log("[WS] Extracted coverArtUrl from currentSong.%s: %s\n",
                                    key, st.coverArtUrl.substr(0, 80).c_str());
                                break;
                            }
                        }
                    }
                }
                onState_(st);
            }

            // 只有 lyricsData 是数组格式时才解析 JSON 行
            // KRC 格式已经在上面 ParseKrc 中处理完毕，跳过此循环
            for (const auto& lineJson : lyricsArray) {
                // 限制歌词总行数
                if (data.lines.size() >= constants::MAX_LYRIC_LINES) break;

                LyricLine line;
                line.text       = lineJson.value("text",       "");
                line.translated = lineJson.value("translated", "");

                if (lineJson.contains("characters") && lineJson["characters"].is_array()) {
                    for (const auto& c : lineJson["characters"]) {
                        // 限制单行字符数
                        if (line.characters.size() >= constants::MAX_CHARS_PER_LINE) break;

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
        try { if (onLyrics_) onLyrics_(data); } catch (...) { Log("Dispatch: onLyrics_ exception"); }
    } else if (type == "playerState") {
        PlayerState st;
        if (j.contains("data") && j["data"].is_object()) {
            const auto& d = j["data"];
            st.isPlaying   = d.value("isPlaying",   false);
            st.currentTime = d.value("currentTime", 0.0);
            st.songTitle   = d.value("songTitle",   "");

            // 提取封面 URL：支持 data 层级直接包含 coverArtUrl/pic 等字段
            for (const auto& key : {"coverArtUrl", "pic", "cover", "albumArt", "image", "poster"}) {
                if (d.contains(key) && d[key].is_string()) {
                    st.coverArtUrl = d[key].get<std::string>();
                    break;
                }
            }
            // 提取歌曲名称（用于封面降级显示）
            if (d.contains("songName") && d["songName"].is_string()) {
                st.songName = d["songName"].get<std::string>();
            }

            // 兼容 currentSong 嵌套对象格式
            if (d.contains("currentSong") && d["currentSong"].is_object()) {
                const auto& cs = d["currentSong"];
                if (st.coverArtUrl.empty()) {
                    for (const auto& key : {"pic", "cover", "albumArt", "image", "poster", "album_pic"}) {
                        if (cs.contains(key) && cs[key].is_string()) {
                            st.coverArtUrl = cs[key].get<std::string>();
                            break;
                        }
                    }
                }
                if (cs.contains("name") && cs["name"].is_string() && st.songName.empty()) {
                    st.songName = cs["name"].get<std::string>();
                }
            }
        }
        try { if (onState_) onState_(st); } catch (...) { Log("Dispatch: onState_ exception"); }
    } else if (type == "welcome") {
        // 服务器欢迎消息,忽略
    } else {
        // 未知消息类型,忽略以保持前向兼容
    }
}

} // namespace moekoe
