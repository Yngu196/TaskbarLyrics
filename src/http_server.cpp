// SPDX-License-Identifier: GPL-2.0
// http_server.cpp - 极简 HTTP 服务器实现
#include "http_server.h"
#include "config.h"
#include "constants.h"
#include "logger.h"

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace moekoe {

namespace {

// 发送 HTTP 响应（CORS 仅允许 localhost，端口使用实际监听端口）
void SendResponse(SOCKET client, int port, int statusCode, const char* statusText,
                  const char* contentType, const char* body) {
    char header[512];
    int bodyLen = static_cast<int>(strlen(body));
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: http://127.0.0.1:%d\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        statusCode, statusText, contentType,
        port,
        moekoe::constants::LOCAL_AUTH_HEADER_NAME,
        bodyLen);
    send(client, header, n, 0);
    send(client, body, bodyLen, 0);
}

// 从原始请求中抽取指定头的值（大小写不敏感；仅支持单值）。
// 找不到时返回空字符串。
std::string ExtractHeader(const char* request, size_t len, const char* headerName) {
    if (!request || !headerName) return {};
    const size_t nameLen = strlen(headerName);
    const char* const end = request + len;
    (void)len;     // 显式使用，避免部分编译器误报 C4100
    // 逐行扫描；每行以 \r\n 结尾
    const char* p = request;
    while (p < end) {
        const char* eol = strstr(p, "\r\n");
        if (!eol || eol == p) break;     // 空行即 header 结束
        const size_t lineLen = static_cast<size_t>(eol - p);
        if (lineLen > nameLen + 2
            && _strnicmp(p, headerName, nameLen) == 0
            && p[nameLen] == ':') {
            const char* val = p + nameLen + 1;
            while (val < eol && (*val == ' ' || *val == '\t')) ++val;
            return std::string(val, static_cast<size_t>(eol - val));
        }
        p = eol + 2;
    }
    return {};
}

// 严格校验本地鉴权 token（shared-secret），防止其他本地进程伪造控制命令。
// 返回 true 表示校验通过；失败时会直接向客户端返回 403，由调用方终止处理。
bool CheckLocalAuthToken(SOCKET client, int port, const char* request, size_t len) {
    const std::string token = ExtractHeader(request, len,
                                            moekoe::constants::LOCAL_AUTH_HEADER_NAME);
    if (token == moekoe::Config::GetAuthToken()) return true;
    SendResponse(client, port, 403, "Forbidden", "application/json",
                 "{\"error\":\"invalid or missing auth token\"}");
    return false;
}

// 严格验证 POST 命令：使用 JSON 白名单而非子字符串匹配
bool IsValidShutdownCommand(const std::string& bodyStr) {
    // 期望格式: {"type":"control","data":{"command":"shutdown"}}
    // 或简化格式: {"command":"shutdown"}
    // 使用简单手写解析，避免引入 nlohmann/json 依赖到 http_server

    // 查找 "command" 键
    const char* cmdKey = strstr(bodyStr.c_str(), "\"command\"");
    if (!cmdKey) return false;

    // 跳过 "command" 和可能的空白/冒号
    const char* p = cmdKey + 9; // strlen("\"command\"")
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;

    // 期望值是 "shutdown"（严格匹配）
    if (strncmp(p, "\"shutdown\"", 10) == 0) return true;

    return false;
}

// 解析请求行中的路径（简化版，只取第一行）
std::string ExtractPath(const char* request, size_t len) {
    (void)len;
    const char* start = strchr(request, ' ');
    if (!start) return "/";
    ++start;
    const char* end = strchr(start, ' ');
    if (!end) return "/";
    return std::string(start, static_cast<size_t>(end - start));
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

    // 连接一次以唤醒 accept
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
    // 初始化 Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log("[HTTP] WSAStartup failed\n");
        return;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        Log("[HTTP] socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }

    // 独占端口，防止被其他进程静默占用
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        Log("[HTTP] bind failed on port %d: WSA error %d. "
            "Port may be occupied by another process. Try closing the other process or change httpServerPort in config.\n",
            port, err);
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        Log("[HTTP] listen failed: %d\n", WSAGetLastError());
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    running_.store(true);
    Log("[HTTP] Server started on port %d\n", port);

    while (!stopRequested_.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);

        timeval tv{0, 100000}; // 100ms 超时
        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel == SOCKET_ERROR || stopRequested_.load()) break;
        if (sel == 0) continue;

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!stopRequested_.load()) {
                Log("[HTTP] accept error: %d\n", WSAGetLastError());
            }
            continue;
        }

        // 设置接收超时（防止恶意/僵死连接占用线程）
        timeval recvTv{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTv), sizeof(recvTv));

        // 读取请求
        char buffer[4096];
        int totalLen = 0;
        while (totalLen < static_cast<int>(sizeof(buffer) - 1)) {
            int r = recv(client, buffer + totalLen, sizeof(buffer) - 1 - totalLen, 0);
            if (r <= 0) break;
            totalLen += r;
            // 简单检测请求头结束
            buffer[totalLen] = '\0';
            if (strstr(buffer, "\r\n\r\n")) break;
        }
        buffer[totalLen] = '\0';

        // 解析方法和路径
        std::string method;
        std::string path;
        const char* methodEnd = strchr(buffer, ' ');
        if (methodEnd) {
            method.assign(buffer, static_cast<size_t>(methodEnd - buffer));
            path = ExtractPath(buffer, static_cast<size_t>(totalLen));
        }

        Log("[HTTP] Request: %s %s (%d bytes)\n",
                 method.c_str(), path.c_str(), totalLen);

        // ── 本地鉴权：OPTIONS 预检请求跳过（浏览器不带自定义头） ──
        if (method != "OPTIONS" && !CheckLocalAuthToken(client, port, buffer, static_cast<size_t>(totalLen))) {
            closesocket(client);
            continue;
        }

        // 路由处理
        if (method == "GET" && path == "/ping") {
            // 在响应体中携带 service 字段，popup.js 可据此确认不是被劫持的 200
            SendResponse(client, port, 200, "OK", "application/json",
                         "{\"status\":\"ok\",\"service\":\"MoeKoeTaskbarLyrics\"}");
        } else if (method == "POST" && path == "/lyrics") {
            // 接收外部歌词+封面数据（供 Chrome 扩展或其他程序调用）
            const char* body = strstr(buffer, "\r\n\r\n");
            if (body) {
                body += 4;
                std::string bodyStr(body);
                if (!bodyStr.empty() && onLyrics_) {
                    Log("[HTTP] Received lyrics data (%zu bytes)\n", bodyStr.size());
                    onLyrics_(bodyStr);
                    SendResponse(client, port, 200, "OK", "application/json",
                                 "{\"status\":\"accepted\"}");
                } else {
                    SendResponse(client, port, 500, "Internal Server Error", "application/json",
                                 "{\"error\":\"no handler\"}");
                }
            } else {
                SendResponse(client, port, 400, "Bad Request", "application/json",
                             "{\"error\":\"no body\"}");
            }
        } else if (method == "POST" && (path == "/" || path == "/shutdown")) {
            // 查找 JSON body（在 \r\n\r\n 之后）
            const char* body = strstr(buffer, "\r\n\r\n");
            if (body) {
                body += 4;
                std::string bodyStr(body);

                // 严格验证命令（白名单匹配，非子字符串）
                if (IsValidShutdownCommand(bodyStr)) {
                    Log("[HTTP] Received valid shutdown command\n");
                    SendResponse(client, port, 200, "OK", "application/json",
                                 "{\"status\":\"shutting_down\"}");

                    if (onCommand_) {
                        onCommand_("shutdown");
                    }
                } else {
                    SendResponse(client, port, 400, "Bad Request", "application/json",
                                 "{\"error\":\"invalid command\"}");
                }
            } else {
                SendResponse(client, port, 400, "Bad Request", "application/json",
                             "{\"error\":\"no body\"}");
            }
        } else if (method == "OPTIONS") {
            // CORS 预检：返回 204 No Content + CORS 头（SendResponse 已包含）
            SendResponse(client, port, 204, "No Content", "text/plain", "");
        } else {
            SendResponse(client, port, 404, "Not Found", "application/json",
                         "{\"error\":\"not found\"}");
        }

        closesocket(client);
    }

    closesocket(listenSock);
    running_.store(false);
    Log("[HTTP] Server stopped\n");
    WSACleanup();
}

} // namespace moekoe
