// SPDX-License-Identifier: GPL-2.0
// http_server.h - 极简 HTTP 服务器（用于 popup.js / 外部程序通信）
//
// 职责:
//   - 监听指定端口（默认 6521）
//   - GET  /ping       → 返回 {"status":"ok"}
//   - POST /lyrics     → 接收歌词+封面数据并回调
//   - POST /           → 解析 JSON，支持 "shutdown" 命令
//   - 运行在独立线程，不阻塞主线程
//
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "constants.h"

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
    bool Start(int port = moekoe::constants::HTTP_SERVER_PORT);

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
