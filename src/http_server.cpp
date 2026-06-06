// SPDX-License-Identifier: GPL-2.0
// http_server.cpp - 极简 HTTP 服务器实现
#include "http_server.h"

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace moekoe {

namespace {
void DebugLog(const char* fmt, ...) {
    // 复用 main.cpp 的日志路径
    const char* logPath = "D:\\MoeKoeMusic-plugin\\MoeKoeMusic-TaskbarLyrics\\debug.log";
    FILE* f = fopen(logPath, "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

// 发送 HTTP 响应
void SendResponse(SOCKET client, int statusCode, const char* statusText,
                  const char* contentType, const char* body) {
    char header[512];
    int bodyLen = static_cast<int>(strlen(body));
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        statusCode, statusText, contentType, bodyLen);
    send(client, header, n, 0);
    send(client, body, bodyLen, 0);
}

// 解析请求行中的路径（简化版，只取第一行）
std::string ExtractPath(const char* request, size_t len) {
    // 找到第一个空格后的路径
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
        addr.sin_port = htons(6521); // 使用固定端口
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(tmp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        closesocket(tmp);
    }

    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}

void HttpServer::ServerLoop(int port) {
    // 初始化 Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        DebugLog("[HTTP] WSAStartup failed\n");
        return;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        DebugLog("[HTTP] socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }

    // 允许端口复用（方便重启）
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        DebugLog("[HTTP] bind failed on port %d: %d\n", port, WSAGetLastError());
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        DebugLog("[HTTP] listen failed: %d\n", WSAGetLastError());
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    running_.store(true);
    DebugLog("[HTTP] Server started on port %d\n", port);

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
                DebugLog("[HTTP] accept error: %d\n", WSAGetLastError());
            }
            continue;
        }

        // 设置接收超时
        timeval recvTv{2, 0};
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

        DebugLog("[HTTP] Request: %s %s (%d bytes)\n",
                 method.c_str(), path.c_str(), totalLen);

        // 路由处理
        if (method == "OPTIONS") {
            // CORS 预检
            SendResponse(client, 200, "OK", "text/plain", "");
        } else if (method == "GET" && path == "/ping") {
            SendResponse(client, 200, "OK", "application/json",
                         "{\"status\":\"ok\",\"service\":\"MoeKoeTaskbarLyrics\"}");
        } else if (method == "POST" && (path == "/" || path == "/shutdown")) {
            // 查找 JSON body（在 \r\n\r\n 之后）
            const char* body = strstr(buffer, "\r\n\r\n");
            if (body) {
                body += 4;
                DebugLog("[HTTP] POST body: %.200s\n", body);

                // 简单解析：查找 "command":"shutdown" 或类似模式
                std::string bodyStr(body);
                if (bodyStr.find("\"shutdown\"") != std::string::npos ||
                    bodyStr.find("shutdown") != std::string::npos) {
                    DebugLog("[HTTP] Received shutdown command\n");
                    SendResponse(client, 200, "OK", "application/json",
                                 "{\"status\":\"shutting_down\"}");

                    if (onCommand_) {
                        onCommand_("shutdown");
                    }
                } else {
                    SendResponse(client, 200, "OK", "application/json",
                                 "{\"status\":\"received\"}");
                }
            } else {
                SendResponse(client, 400, "Bad Request", "application/json",
                             "{\"error\":\"no body\"}");
            }
        } else {
            SendResponse(client, 404, "Not Found", "application/json",
                         "{\"error\":\"not found\"}");
        }

        closesocket(client);
    }

    closesocket(listenSock);
    running_.store(false);
    DebugLog("[HTTP] Server stopped\n");
    WSACleanup();
}

} // namespace moekoe
