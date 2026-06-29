// SPDX-License-Identifier: GPL-2.0
// http_server.cpp - HTTP server implementation (based on cpp-httplib)
//
// Replaces the original hand-rolled strstr HTTP parsing with httplib, providing:
//   - Full HTTP/1.1 semantics (chunked encoding, header folding, keep-alive)
//   - Built-in timeout handling, request smuggling protection
//   - Unified CORS headers, simplified JSON responses
#include "http_server.h"
#include "config.h"
#include "constants.h"
#include "logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// cpp-httplib header-only (no OpenSSL - local HTTP API only, no HTTPS needed)
#include <httplib.h>

namespace moekoe {

namespace {

// Validate shutdown command from JSON body
// Compatible with: {"command":"shutdown"} and {"type":"control","data":{"command":"shutdown"}}
bool IsValidShutdownCommand(const std::string& bodyStr) {
    const char* cmdKey = strstr(bodyStr.c_str(), "\"command\"");
    if (!cmdKey) return false;
    const char* p = cmdKey + 9;
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    return (strncmp(p, "\"shutdown\"", 10) == 0);
}

} // namespace

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Start(int port) {
    if (running_.load()) return true;
    stopRequested_.store(false);
    port_ = port;
    serverThread_ = std::thread([this, port]() { ServerLoop(port); });
    return true;
}

void HttpServer::Stop() {
    if (!running_.load()) return;
    stopRequested_.store(true);

    // Poke the listening socket to wake up accept()
    SOCKET tmp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tmp != INVALID_SOCKET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port_));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(tmp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        closesocket(tmp);
    }

    if (serverThread_.joinable()) {
        DWORD waitResult = ::WaitForSingleObject(
            serverThread_.native_handle(),
            moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
        if (waitResult == WAIT_TIMEOUT) {
            moekoe::Log("[SERVER] Thread join timed out (%d ms), forcing exit\n",
                       moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
            serverThread_.detach();
            ::ExitProcess(2);
        } else {
            serverThread_.join();
        }
    }
}

void HttpServer::ServerLoop(int port) {
    httplib::Server svr;

    // ===========================================
    // Middleware: CORS headers + auth check
    // ===========================================
    svr.set_pre_routing_handler([port](const httplib::Request& req, httplib::Response& res) {
        // CORS headers (localhost-only origin)
        char origin[64];
        snprintf(origin, sizeof(origin), "http://127.0.0.1:%d", port);
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        char allowHeaders[128];
        snprintf(allowHeaders, sizeof(allowHeaders), "Content-Type, %s",
                 moekoe::constants::LOCAL_AUTH_HEADER_NAME);
        res.set_header("Access-Control-Allow-Headers", allowHeaders);

        // OPTIONS preflight - skip auth check
        if (req.method == "OPTIONS") {
            res.status = 204;
            res.set_content("", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }

        // Auth check: verify X-MoeKoe-Token header
        if (!req.has_header(moekoe::constants::LOCAL_AUTH_HEADER_NAME)) {
            res.status = 403;
            res.set_content("{\"error\":\"missing auth token\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        const std::string token = req.get_header_value(moekoe::constants::LOCAL_AUTH_HEADER_NAME);
        if (token != moekoe::Config::GetAuthToken()) {
            res.status = 403;
            res.set_content("{\"error\":\"invalid auth token\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ===========================================
    // GET /ping - health check
    // ===========================================
    svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\",\"service\":\"MoeKoeTaskbarLyrics\"}", "application/json");
    });

    // ===========================================
    // POST /lyrics - receive lyrics/cover data
    // ===========================================
    svr.Post("/lyrics", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.body.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"no body\"}", "application/json");
            return;
        }
        if (!onLyrics_) {
            res.status = 500;
            res.set_content("{\"error\":\"no handler\"}", "application/json");
            return;
        }
        Log("[HTTP] Received lyrics data (%zu bytes)\n", req.body.size());
        onLyrics_(req.body);
        res.set_content("{\"status\":\"accepted\"}", "application/json");
    });

    // ===========================================
    // POST / and /shutdown - command endpoints
    // ===========================================
    auto shutdownHandler = [this](const httplib::Request& req, httplib::Response& res) {
        if (req.body.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"no body\"}", "application/json");
            return;
        }
        if (IsValidShutdownCommand(req.body)) {
            Log("[HTTP] Received valid shutdown command\n");
            res.set_content("{\"status\":\"shutting_down\"}", "application/json");
            if (onCommand_) onCommand_("shutdown");
        } else {
            res.status = 400;
            res.set_content("{\"error\":\"invalid command\"}", "application/json");
        }
    };
    svr.Post("/", shutdownHandler);
    svr.Post("/shutdown", shutdownHandler);

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

    // Stopper thread: polls stopRequested_ and calls svr.stop()
    std::thread stopper([this, &svr]() {
        while (!stopRequested_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        svr.stop();
    });

    if (!svr.listen("127.0.0.1", port)) {
        int err = WSAGetLastError();
        Log("[HTTP] listen failed on port %d: WSA error %d\n", port, err);
        running_.store(false);
        if (stopper.joinable()) stopper.join();
        return;
    }

    stopper.join();
    running_.store(false);
    Log("[HTTP] Server stopped\n");
}

} // namespace moekoe
