// SPDX-License-Identifier: GPL-2.0
// native_messaging.h - Chrome Native Messaging Host 协议处理
#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>

namespace moekoe {

// Native Messaging 消息结构
struct NativeMessage {
    std::string command;     // "start", "stop", "status", "ping"
    nlohmann::json params;   // 可选参数
};

// Native Messaging 响应结构
struct NativeResponse {
    bool success = false;
    std::string message;
    nlohmann::json data;
};

// Native Messaging Host 处理器
class NativeMessagingHost {
public:
    using CommandHandler = std::function<NativeResponse(const NativeMessage&)>;

    NativeMessagingHost();
    ~NativeMessagingHost();

    // 设置命令处理回调
    void SetCommandHandler(CommandHandler handler);

    // 运行 Native Messaging 循环（阻塞，从 stdin 读取消息）
    // 返回 false 表示应该退出程序
    bool Run();

    // 发送响应到 stdout（4字节长度前缀 + JSON）
    void SendResponse(const NativeResponse& response);

    // 从 stdin 读取消息（4字节长度前缀 + JSON）
    bool ReadMessage(NativeMessage& msg);

private:
    CommandHandler handler_;
    bool running_ = true;
};

} // namespace moekoe