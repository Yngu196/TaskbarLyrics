// SPDX-License-Identifier: GPL-3.0
// websocket_client.h - WebSocket 客户端封装
//
// 职责:
//   - 连接 MoeKoeMusic WebSocket 服务（默认 ws://127.0.0.1:6520）
//   - 解析 "lyrics" / "playerState" 消息并回调
//   - 自动重连:1s → 2s → 4s → 8s → 15s(上限)
//
#pragma once

#include "lyrics_data.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// 前向声明 ix::WebSocket,避免在头文件中暴露第三方头文件
namespace ix {
class WebSocket;
}

namespace moekoe {

class WebSocketClient {
public:
    using LyricsCallback = std::function<void(const LyricsData&)>;
    using StateCallback  = std::function<void(const PlayerState&)>;
    using StatusCallback = std::function<void(bool connected)>;

    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // 启动连接（异步）
    bool Connect(const std::string& url = "ws://127.0.0.1:6520");

    // 主动断开（不会自动重连）
    void Disconnect();

    // 注册回调
    void OnLyrics(LyricsCallback cb)        { onLyrics_ = std::move(cb); }
    void OnPlayerState(StateCallback cb)    { onState_  = std::move(cb); }
    void OnConnectionStatus(StatusCallback cb) { onStatus_ = std::move(cb); }

    // 发送控制指令（可选功能）: toggle / next / prev
    bool SendControl(const std::string& command);

    // 状态查询
    bool IsConnected() const { return connected_.load(); }

    // 调试日志开关（由 config.debugLog 控制）
    void SetDebugLog(bool enabled) { debugLog_ = enabled; }

    // 让外部驱动重连（不阻塞）
    void RequestReconnect();

    // 公共静态方法：解析 KRC 格式歌词字符串（供 HTTP API 等外部调用）
    static LyricsData ParseKrcString(const std::string& krcText);

private:
    void ReconnectLoop();
    void DispatchWsMessage(const std::string& raw);

    // ixwebsocket 实例（pimpl）
    std::unique_ptr<ix::WebSocket> client_;

    // 状态
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> reconnectNow_{false};

    // 配置
    std::string url_{"ws://127.0.0.1:6520"};
    bool        debugLog_{false};

    // 回调
    LyricsCallback onLyrics_;
    StateCallback  onState_;
    StatusCallback onStatus_;

    // 重连线程
    std::thread reconnectThread_;
    std::mutex   stateMutex_;
};

} // namespace moekoe
