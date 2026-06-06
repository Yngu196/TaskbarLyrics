// SPDX-License-Identifier: GPL-2.0
// http_server.h - 极简 HTTP 服务器（用于 popup.js 通信）
//
// 职责:
//   - 监听指定端口（默认 6521）
//   - GET  /ping       → 返回 {"status":"ok"}
//   - POST /           → 解析 JSON，支持 "shutdown" 命令
//   - 运行在独立线程，不阻塞主线程
//
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace moekoe {

class HttpServer {
public:
    // 命令回调: 收到 shutdown 等命令时调用
    using CommandCallback = std::function<void(const std::string& command)>;

    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 启动服务器（异步，返回是否成功启动）
    bool Start(int port = 6521);

    // 停止服务器
    void Stop();

    // 注册命令回调
    void OnCommand(CommandCallback cb) { onCommand_ = std::move(cb); }

    // 是否正在运行
    bool IsRunning() const { return running_.load(); }

private:
    void ServerLoop(int port);

    std::thread serverThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    CommandCallback onCommand_;
};

} // namespace moekoe
