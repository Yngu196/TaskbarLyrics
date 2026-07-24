// SPDX-License-Identifier: GPL-3.0
// websocket_client.cpp - WebSocket 客户端实现
#include "websocket_client.h"

#include "constants.h"
#include "krc_parser.h"
#include "logger.h"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace moekoe {

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

// 重连退避时间（含随机抖动，防止多实例同步重连）
int BackoffSeconds(int attempt) {
    int base;
    if (attempt <= 0) base = 1;
    else if (attempt == 1) base = 1;
    else if (attempt == 2) base = 2;
    else if (attempt == 3) base = 4;
    else if (attempt == 4) base = 8;
    else base = 15;
    // ±30% 抖动 (0.7x ~ 1.3x)
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(70, 130);
    int jittered = base * dist(rng) / 100;
    return (jittered < 1) ? 1 : jittered;
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

    // 先等待 reconnectThread 退出，避免 ix::WebSocket::stop() 与 reconnectThread 死锁
    if (reconnectThread_.joinable()) {
        DWORD waitResult = ::WaitForSingleObject(
            reconnectThread_.native_handle(),
            moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
        if (waitResult == WAIT_TIMEOUT) {
            moekoe::Log("[WS] Reconnect thread join timed out (%d ms), detaching\n",
                       moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
            reconnectThread_.detach();
        } else {
            reconnectThread_.join();
        }
    }

    // reconnectThread 已退出后再关闭 client
    // ix::WebSocket::stop() 在等待关闭握手时可能无限阻塞，
    // 且与回调线程可能产生死锁。
    // 解决方案：在独立线程中异步清理，不阻塞主线程。
    // 进程退出后 OS 会自动回收所有资源。
    if (client_) {
        auto wsPtr = std::move(client_);  // 取走所有权
        client_.reset();
        // 在独立线程中执行 stop + 析构，detach 让其自行完成
        // 不等待：主线程必须继续执行后续关闭步骤
        std::thread([wsPtr = std::move(wsPtr)]() mutable {
            try { wsPtr->stop(); } catch (const std::exception& e) {
                LogError("[WS] Exception during async stop: %s\n", e.what());
            } catch (...) {
                LogError("[WS] Unknown exception during async stop\n");
            }
            // wsPtr 离开作用域自动析构
        }).detach();
        Log("[WS] WebSocket cleanup detached (async)\n");
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
        // 使用 weak_ptr 防止 detach 后回调访问已销毁的对象
        auto self = this;
        client_->setOnMessageCallback(
            [self](const ix::WebSocketMessagePtr& msg) {
                // 如果已请求停止，忽略所有回调
                if (self->stopRequested_.load()) return;
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
            try { client_->stop(); } catch (const std::exception& e) {
                LogError("[WS] Exception during reconnect stop: %s\n", e.what());
            } catch (...) {
                LogError("[WS] Unknown exception during reconnect stop\n");
            }
            client_.reset();

        } else {
            Log("Reconnect: connected successfully");
        }
    }
    Log("ReconnectLoop ended");
}

void WebSocketClient::DispatchWsMessage(const std::string& raw) {
    // 安全检查：拒绝过大的消息，防止内存耗尽
    if (raw.size() > constants::MAX_WS_MESSAGE_SIZE) {
        if (debugLog_) Log("[WS] Message too large: " + std::to_string(raw.size()) + " bytes (max: " + std::to_string(constants::MAX_WS_MESSAGE_SIZE) + "), discarded");
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
        // if (debugLog_) Log("[WS] Received lyrics message, size=%zu\n", raw.size());
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
                // if (debugLog_) Log("[WS] lyricsData is array, count=%zu\n", lyricsArray.size());
            } else if (ld.is_string()) {
                std::string ldStr = ld.get<std::string>();
                data = ParseKrcString(ldStr);
                hasLD = data.valid;
            } else {
                Log("Dispatch: lyricsData unexpected type=" + std::to_string(static_cast<int>(ld.type())));
            }
        } else {
            // 已注释：缺少 lyricsData 字段的诊断日志太噪
            // if (debugLog_) {
            //     Log("[WS] lyrics message has NO lyricsData field, data keys present: ");
            //     if (j.contains("data") && j["data"].is_object()) {
            //         for (auto& el : j["data"].items()) Log("%s ", el.key().c_str());
            //     }
            //     Log("\n");
            // }
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
                    // if (debugLog_) Log("[WS] lyrics has currentSong, type=%d null=%d\n",
                    //     cs.type(), cs.is_null() ? 1 : 0);
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
                                // if (debugLog_) Log("[WS] Extracted coverArtUrl from currentSong.%s: %s\n",
                                //     key, st.coverArtUrl.substr(0, 80).c_str());
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
