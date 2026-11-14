// SPDX-License-Identifier: GPL-3.0
// http_server.cpp - HTTP server implementation (based on cpp-httplib)
//
// Replaces the original hand-rolled strstr HTTP parsing with httplib, providing:
//   - Full HTTP/1.1 semantics (chunked encoding, header folding, keep-alive)
//   - Built-in timeout handling, request smuggling protection
//   - Unified CORS headers, simplified JSON responses
#include "net/http_server.h"
#include "config/config.h"
#include "core/constants.h"
#include "util/logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// cpp-httplib header-only (no OpenSSL - local HTTP API only, no HTTPS needed)
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace moekoe {

namespace {

// 恒定时间字符串比较：逐字节异或累加，防止本机其他进程通过计时差异爆破 Token。
bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    int diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

} // namespace

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Start(int port) {
    std::unique_lock<std::mutex> lifecycleLock(lifecycleMutex_);

    // serverThread_ 在 ServerLoop 返回后仍然是 joinable；先回收已经结束的
    // 线程，避免下一次赋值给 joinable std::thread 触发 std::terminate。
    // join 必须放在锁外：ServerLoop 收尾阶段如果需要 lifecycleMutex_，
    // 持锁等待它退出就是锁序反转死锁。
    while (serverThread_.joinable()) {
        if (running_.load()) return true;  // 仍在监听，重复 Start 幂等成功
        std::thread finished = std::move(serverThread_);
        lifecycleLock.unlock();
        finished.join();
        lifecycleLock.lock();
    }

    // 拒绝在 fallback token 下启动 HTTP 服务：
    // 此时鉴权形同虚设，任何本机进程都能用固定字符串访问所有接口。
    if (Config::IsUsingFallbackToken()) {
        Log("[SERVER] Refusing to start: fallback token active (auth unsafe)\n");
        return false;
    }

    stopRequested_.store(false);
    // Mark the lifecycle active before launching the worker.  This makes a
    // second Start() during the tiny startup window idempotent instead of
    // trying to join a thread that is still entering listen().
    running_.store(true);
    serverThread_ = std::thread([this, port]() { ServerLoop(port); });
    return true;
}

void HttpServer::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
        // running_ 只在 ServerLoop 开始监听后才置 true；如果 Stop 紧跟在
        // Start 后调用，必须仍依据 joinable 回收启动中的线程。
        if (!serverThread_.joinable()) {
            running_.store(false);
            return;
        }
        stopRequested_.store(true);
        // 把线程所有权移出临界区后再 join，绝不在持锁状态下等待工作线程。
        worker = std::move(serverThread_);
    }

    // 停止由 ServerLoop 内的 stopper 线程负责：它看到 stopRequested_ 后会
    // 反复调用 svr.stop() 直到 listen() 返回，因此这里只需等待 worker 结束。
    // 不再从主线程跨线程持有指向 ServerLoop 栈上 httplib::Server 的回调，
    // 也不再用 connect() 去「戳」端口——前者有悬垂引用风险，后者可能误连
    // 到同端口的其他进程或在受限网络栈上阻塞。
    worker.join();
    running_.store(false);
}

void HttpServer::SetPendingSeek(double position, double duration) {
    std::lock_guard<std::mutex> lock(seekMutex_);
    seekPosition_  = position;
    seekDuration_ = duration;
    seekPending_  = true;
    seekSeq_++;
}

void HttpServer::ServerLoop(int port) {
    httplib::Server svr;

    // 这里刻意不获取 lifecycleMutex_：Stop()/Start() 可能正持有它等待本线程
    // 结束，工作线程在启动或收尾阶段去拿同一把锁就会形成锁序反转死锁。

    // 限制请求体大小，防御异常大 payload（本机场景正常歌词数据远小于此）
    svr.set_payload_max_length(4 * 1024 * 1024); // 4 MB

    // ===========================================
    // Middleware: CORS headers + auth check
    // ===========================================
    svr.set_pre_routing_handler([this, port](const httplib::Request& req, httplib::Response& res) {
        // CORS headers (localhost-only origin)
        std::string origin = "http://127.0.0.1:" + std::to_string(port);
        res.set_header("Access-Control-Allow-Origin", origin.c_str());
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        std::string allowHeaders = std::string("Content-Type, ") + moekoe::constants::LOCAL_AUTH_HEADER_NAME;
        res.set_header("Access-Control-Allow-Headers", allowHeaders.c_str());

        // OPTIONS preflight - skip auth check
        if (req.method == "OPTIONS") {
            res.status = 204;
            res.set_content("", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }

        // GET /seek/pending - 免鉴权的只读 seek 轮询端点（读后即清）。
        //
        // 为什么免鉴权：该端点是 seek 的兜底投递通道，必须在「EXE 非宿主托管
        // 启动（stdout 无接收方）、扩展 SW 休眠、storage 通道失效」等所有
        // 异常场景下仍可用。调用方是运行在 file:// 页面里的 content script，
        // 无法携带本机注册表 token（file:// 非安全上下文，crypto.subtle 不可用，
        // 无法解密 storage 中的密文 token）。
        //
        // 威胁模型权衡：本端点仅暴露「播放位置 + 总时长」（数字，无歌曲名/
        // 歌词等敏感信息），且宿主 MoeKoeMusic 自身的 WS API（6520 端口）完全
        // 无鉴权地对外广播完整歌词与播放状态——本端点的信息暴露面严格小于
        // 宿主已有行为。恶意页面的最坏影响是抢先消费 pending seek（导致
        // 一次拖动失效），不影响播放本身。
        if (req.method == "GET" && req.path == "/seek/pending") {
            res.set_header("Access-Control-Allow-Origin", "*");
            nlohmann::json j;
            {
                std::lock_guard<std::mutex> lock(seekMutex_);
                j["pending"] = seekPending_;
                if (seekPending_) {
                    j["position"] = seekPosition_;
                    j["duration"]  = seekDuration_;
                    j["seq"]       = seekSeq_;
                    seekPending_ = false;  // 读后即清
                }
            }
            res.set_content(j.dump(), "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        // GET /ping - 免鉴权的存活探测端点。
        //
        // 为什么免鉴权：popup 的状态回退通道。首选的 bridge 查询（native-host:status）
        // 在 MV3 Service Worker 被回收、客户端重启后桥接页未重连等场景会失效，
        // 而鉴权 token 恰恰只能通过同一条桥接链路同步——若 /ping 也要求 token，
        // 两条通道会同时失效，popup 永远显示"未运行"（实际 EXE 活着）。
        //
        // 威胁模型权衡：响应只有存活布尔与服务名，无任何敏感数据；
        // 探测者至多得知"本机运行着任务栏歌词插件"。
        if (req.method == "GET" && req.path == "/ping") {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"ok\",\"service\":\"MoeKoeTaskbarLyrics\"}",
                            "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        // Auth check: verify X-MoeKoe-Token header
        if (!req.has_header(moekoe::constants::LOCAL_AUTH_HEADER_NAME)) {
            res.status = 403;
            res.set_content("{\"error\":\"missing auth token\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        const std::string token = req.get_header_value(moekoe::constants::LOCAL_AUTH_HEADER_NAME);
        if (!ConstantTimeEquals(token, moekoe::Config::GetAuthToken())) {
            res.status = 403;
            res.set_content("{\"error\":\"invalid auth token\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ===========================================
    // GET /ping - health check（在 pre_routing_handler 中免鉴权处理，见上）
    // ===========================================

    // ===========================================
    // 404 fallback
    // ===========================================
    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"error\":\"not found\"}", "application/json");
    });

    // ===========================================
    // Socket options + timeouts
    // ===========================================
    svr.set_socket_options([](socket_t sock) {
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
    });
    svr.set_read_timeout(std::chrono::seconds(5));
    svr.set_write_timeout(std::chrono::seconds(5));

    running_.store(true);
    Log("[HTTP] Server starting on port %d (httplib)\n", port);

    // Stopper thread: 只要 listen() 还没返回就持续观察停止请求。
    // stop() 在 listen_internal() 尚未进入时是 no-op（is_running_ == false），
    // 因此必须反复调用而不是只调一次，否则早期 Stop() 会被丢掉，accept()
    // 会永远阻塞。退出条件只能是 listenDone，不能是 stopRequested_：
    // listen() 也可能因端口占用等原因自行返回，此时 stopper 若仍在
    // "等停止请求" 的循环里，下面的 join() 就会永久挂起。
    std::atomic<bool> listenDone{false};
    std::thread stopper([this, &svr, &listenDone]() {
        while (!listenDone.load(std::memory_order_acquire)) {
            if (stopRequested_.load()) svr.stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Stop() may race with thread startup, before listen() has entered its
    // accept loop.  In that case do not enter listen at all; otherwise the
    // stop signal could be lost and the lifecycle join would block.
    if (stopRequested_.load()) {
        listenDone.store(true, std::memory_order_release);
        if (stopper.joinable()) stopper.join();
        running_.store(false);
        return;
    }

    if (!svr.listen("127.0.0.1", port)) {
        int err = WSAGetLastError();
        Log("[HTTP] listen failed on port %d: WSA error %d\n", port, err);
        running_.store(false);
        listenDone.store(true, std::memory_order_release);
        if (stopper.joinable()) stopper.join();
        return;
    }

    listenDone.store(true, std::memory_order_release);
    stopper.join();
    running_.store(false);
    Log("[HTTP] Server stopped\n");
}

} // namespace moekoe
