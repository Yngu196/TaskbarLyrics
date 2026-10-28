// SPDX-License-Identifier: GPL-3.0
// websocket_client.cpp - WebSocket 客户端实现
#include "net/websocket_client.h"

#include "core/constants.h"
#include "lyrics/krc_parser.h"
#include "util/logger.h"

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

WebSocketClient::WebSocketClient() {
    aliveToken_ = std::make_shared<std::atomic<bool>>(true);
}

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

    // client_ 统一由 ReconnectLoop 创建与重建；
    // 此处预创建会与重连线程的重建构成无锁 unique_ptr 数据竞争，故移除。

    // 启动后台重连循环（幂等）
    if (!reconnectThread_.joinable()) {
        // 无重连线程：存活令牌可能已被上一次 Disconnect 置 false，重建之
        if (!aliveToken_ || !aliveToken_->load()) {
            aliveToken_ = std::make_shared<std::atomic<bool>>(true);
        }
        reconnectThread_ = std::thread([this] { ReconnectLoop(); });
    } else {
        reconnectNow_.store(true);
    }
    return true;
}

void WebSocketClient::Disconnect() {
    stopRequested_.store(true);
    reconnectNow_.store(false);

    // 存活令牌置 false：分离清理线程的 stop() 可能继续触发晚到回调，
    // 回调见令牌失效立即返回，不再访问可能已析构的 WebSocketClient 成员
    if (aliveToken_) aliveToken_->store(false);

    // 先等待 reconnectThread 退出，避免 ix::WebSocket::stop() 与 reconnectThread 死锁
    if (reconnectThread_.joinable()) {
        // ReconnectLoop 的等待粒度为 100ms，stopRequested_ 可快速打断；
        // 必须 join，不能让线程继续使用 this。
        reconnectThread_.join();
    }

    // reconnectThread 已退出后再关闭 client。
    // ix::WebSocket::stop() 在等待关闭握手时可能无限阻塞，
    // 且与回调线程可能产生死锁。
    // 解决方案：锁内移出所有权，在独立线程中异步清理，且不在 UI 退出路径等待关闭握手。
    // 闭包独占 wsPtr，不访问 WebSocketClient；即使库内部 stop() 卡住，
    // 也不会阻塞主线程或在对象析构后访问悬空状态。
    std::unique_ptr<ix::WebSocket> wsPtr;
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        wsPtr = std::move(client_);  // 取走所有权
    }
    if (wsPtr) {
        std::thread([wsPtr = std::move(wsPtr)]() mutable {
            try { wsPtr->stop(); } catch (const std::exception& e) {
                LogError("[WS] Exception during async stop: %s\n", e.what());
            } catch (...) {
                LogError("[WS] Unknown exception during async stop\n");
            }
            // wsPtr 离开作用域自动析构
        }).detach();
        Log("[WS] WebSocket cleanup dispatched\n");
    }

    if (connected_.exchange(false)) {
        if (onStatus_) onStatus_(false);
    }
}

bool WebSocketClient::SendControl(const std::string& command) {
    if (!connected_.load()) return false;
    json j;
    j["type"] = "control";
    j["data"] = {{"command", command}};
    // client_ 可能正被 ReconnectLoop 并发重建/销毁，读取与发送必须持锁
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (!client_) return false;
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

        // 配置客户端（重建 + 绑定回调 + start 全程持锁，
        // 避免与 SendControl 的无锁读取/发送构成数据竞争）
        try {
            std::lock_guard<std::mutex> lock(clientMutex_);
            client_ = std::make_unique<ix::WebSocket>();
            client_->setUrl(urlCopy);

            // 绑定消息回调
            // 按值捕获存活令牌：分离清理线程的 stop() 可能继续触发晚到回调，
            // 回调先检查令牌，失效立即返回、不再访问 this。
            // （原注释声称使用 weak_ptr 与实际实现不符，已更正）
            auto token = aliveToken_;
            client_->setOnMessageCallback(
                [this, token](const ix::WebSocketMessagePtr& msg) {
                    if (!token || !token->load()) return;
                    // 如果已请求停止，忽略所有回调
                    if (stopRequested_.load()) return;
                    if (msg->type == ix::WebSocketMessageType::Open) {
                        Log("WS: opened");
                        connected_.store(true);
                        if (onStatus_) onStatus_(true);
                    } else if (msg->type == ix::WebSocketMessageType::Close) {
                        Log("WS: closed");
                        connected_.store(false);
                        if (onStatus_) onStatus_(false);
                    } else if (msg->type == ix::WebSocketMessageType::Message) {
                        if (!msg->str.empty()) {
                            try {
                                DispatchWsMessage(msg->str);
                            } catch (const std::exception& e) {
                                Log("WS: Dispatch exception: " + std::string(e.what()));
                            } catch (...) {
                                Log("WS: Dispatch unknown exception");
                            }
                        }
                    } else if (msg->type == ix::WebSocketMessageType::Error) {
                        Log("WS: ERROR - " + msg->errorInfo.reason);
                        connected_.store(false);
                        if (onStatus_) onStatus_(false);
                    }
                });

            // 启动（同步）—— ix::WebSocket::start 内部会启动线程
            client_->start();
        } catch (...) {
            Log("Reconnect: exception creating WebSocket");
            continue;
        }

        // 等到连接成功 / 失败 / 停止
        for (int i = 0; i < constants::WS_CONNECT_TIMEOUT_ITERATIONS && !stopRequested_.load(); ++i) { // 5s 连接窗口
            if (connected_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(constants::RECONNECT_WAIT_GRANULARITY_MS));
        }
        if (stopRequested_.load()) break;

        if (!connected_.load()) {
            Log("Reconnect: connection failed after 5s");
            // 启动失败:锁内移出旧 client，stop() 在锁外执行——
            // 它可能长时间阻塞，不能拖住等待 clientMutex_ 的 SendControl/Disconnect
            std::unique_ptr<ix::WebSocket> deadClient;
            {
                std::lock_guard<std::mutex> lock(clientMutex_);
                deadClient = std::move(client_);
                client_.reset();
            }
            if (deadClient) {
                try { deadClient->stop(); } catch (const std::exception& e) {
                    LogError("[WS] Exception during reconnect stop: %s\n", e.what());
                } catch (...) {
                    LogError("[WS] Unknown exception during reconnect stop\n");
                }
                deadClient.reset();
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
                // 兼容：lyricsData 字符串可能是 JSON 字符串化的数组
                // （不同 MoeKoeMusic 版本发送格式不同），也可能是 KuGou KRC 歌词文本。
                // 先尝试按 JSON 数组解析，失败再走 KRC，避免真实歌词被丢弃导致显示频谱。
                json maybeArray = json::parse(ldStr, nullptr, false);
                if (!maybeArray.is_discarded() && maybeArray.is_array()) {
                    lyricsArray = maybeArray;
                    hasLD = true;
                    if (debugLog_) Log("[WS] lyricsData string parsed as JSON array, count=%zu\n",
                                       lyricsArray.size());
                } else {
                    data = ParseKrcString(ldStr);
                    hasLD = data.valid;
                    if (debugLog_ && !hasLD)
                        Log("[WS] lyricsData string is neither JSON array nor valid KRC (len=%zu)\n",
                            ldStr.size());
                }
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
                        // object 格式: {name, author, pic, ...}（MoeKoeMusic 使用 author）
                        if (cs.contains("name") && cs["name"].is_string()) {
                            st.songTitle = cs["name"].get<std::string>();
                            st.songName = cs["name"].get<std::string>();
                        }
                        for (const auto& key : {"artist", "author", "author_name", "singer", "singer_name"}) {
                            if (cs.contains(key) && cs[key].is_string()) {
                                st.artist = cs[key].get<std::string>();
                                if (!st.songTitle.empty()) {
                                    st.songTitle += " - " + st.artist;
                                }
                                break;
                            }
                        }
                        // 提取专辑封面 URL（尝试已知字段名）
                        for (const auto& key : {"pic", "cover", "albumArt", "image", "poster", "img", "album_pic"}) {
                            if (cs.contains(key) && cs[key].is_string()) {
                                st.coverArtUrl = cs[key].get<std::string>();
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
                // 兼容缺少 text 字段、仅含 characters 时间轴的歌词行：
                // 由字符级数据拼接出整行文本，否则会被上层误判为无歌词/纯音乐
                if (line.text.empty()) {
                    for (const auto& c : line.characters) {
                        line.text += c.ch;
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
            // 提取歌手名称（悬停时展示）
            for (const auto& key : {"artist", "author", "author_name", "singer", "singer_name"}) {
                if (d.contains(key) && d[key].is_string()) {
                    st.artist = d[key].get<std::string>();
                    break;
                }
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
                for (const auto& key : {"artist", "author", "author_name", "singer", "singer_name"}) {
                    if (st.artist.empty() && cs.contains(key) && cs[key].is_string()) {
                        st.artist = cs[key].get<std::string>();
                        break;
                    }
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
