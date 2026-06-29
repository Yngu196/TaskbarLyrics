// SPDX-License-Identifier: GPL-2.0
// http_server.h - HTTP 服务器（基于 cpp-httplib 提供完整 HTTP/1.1 语义）
//
// 职责:
//   - 监听指定端口（默认 6523），仅绑定 127.0.0.1
//   - GET  /ping       → 返回 {"status":"ok","service":"MoeKoeTaskbarLyrics"}
//   - POST /lyrics     → 接收歌词+封面数据并回调
//   - POST / /shutdown → 解析 JSON，支持 "shutdown" 命令
//   - 运行在独立线程，不阻塞主线程
//   - 通过 cpp-httplib 获得完整的 chunked encoding / header 折叠 / keep-alive 等 HTTP/1.1 语义
//
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
} // namespace httplib

namespace moekoe {

class HttpServer {
public:
    // 命令回调: 收到 shutdown 等命令时调用
    using CommandCallback = std::function<void(const std::string& command)>;

    // 歌词数据回调: 收到外部歌词+封面数据时调用
    // 参数为原始 JSON 字符串，由调用方解析
    using LyricsCallback = std::function<void(const std::string& jsonBody)>;

    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 启动服务器（异步，返回是否成功启动）
    bool Start(int port = 6523);

    // 停止服务器
    void Stop();

    // 注册命令回调
    void OnCommand(CommandCallback cb) { onCommand_ = std::move(cb); }

    // 注册歌词数据回调
    void OnLyrics(LyricsCallback cb) { onLyrics_ = std::move(cb); }

    // 是否正在运行
    bool IsRunning() const { return running_.load(); }

private:
    void ServerLoop(int port);

    std::thread serverThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    int port_{0};
    CommandCallback onCommand_;
    LyricsCallback onLyrics_;
};

} // namespace moekoe
