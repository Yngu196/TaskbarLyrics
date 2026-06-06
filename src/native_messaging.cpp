// SPDX-License-Identifier: GPL-2.0
// native_messaging.cpp - Chrome Native Messaging Host 实现
#include "native_messaging.h"

#include <iostream>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
// Windows 下需要设置 stdin/stdout 为二进制模式
static void SetBinaryMode() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}
#else
static void SetBinaryMode() {}
#endif

namespace moekoe {

NativeMessagingHost::NativeMessagingHost() {
    SetBinaryMode();
}

NativeMessagingHost::~NativeMessagingHost() = default;

void NativeMessagingHost::SetCommandHandler(CommandHandler handler) {
    handler_ = std::move(handler);
}

bool NativeMessagingHost::ReadMessage(NativeMessage& msg) {
    // Chrome Native Messaging 协议:
    // 前 4 字节是消息长度（little-endian uint32）
    // 后面是 JSON 字符串

    uint32_t length = 0;
    
    // 读取长度前缀
    if (std::cin.read(reinterpret_cast<char*>(&length), 4).gcount() != 4) {
        // EOF 或读取失败 → 端断开连接
        return false;
    }

    if (length == 0 || length > 1024 * 1024) {  // 限制最大 1MB
        return false;
    }

    // 读取 JSON 内容
    std::vector<char> buffer(length);
    if (std::cin.read(buffer.data(), length).gcount() != static_cast<std::streamsize>(length)) {
        return false;
    }

    std::string jsonStr(buffer.data(), length);

    try {
        auto j = nlohmann::json::parse(jsonStr);
        msg.command = j.value("command", "");
        msg.params = j.value("params", nlohmann::json::object());
        return true;
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
}

void NativeMessagingHost::SendResponse(const NativeResponse& response) {
    nlohmann::json j;
    j["success"] = response.success;
    j["message"] = response.message;
    if (!response.data.is_null()) {
        j["data"] = response.data;
    }

    std::string jsonStr = j.dump();

    // 写入长度前缀（4字节 little-endian uint32）
    uint32_t length = static_cast<uint32_t>(jsonStr.size());
    std::cout.write(reinterpret_cast<char*>(&length), 4);
    
    // 写入 JSON 内容
    std::cout.write(jsonStr.data(), length);
    std::cout.flush();
}

bool NativeMessagingHost::Run() {
    while (running_) {
        NativeMessage msg;
        if (!ReadMessage(msg)) {
            // stdin 关闭 → Chrome 断开连接
            running_ = false;
            return false;
        }

        if (msg.command == "shutdown" || msg.command == "stop") {
            SendResponse({true, "shutting down"});
            running_ = false;
            return false;
        }

        if (handler_) {
            NativeResponse resp = handler_(msg);
            SendResponse(resp);
        } else {
            SendResponse({false, "no handler registered"});
        }
    }
    return running_;
}

} // namespace moekoe