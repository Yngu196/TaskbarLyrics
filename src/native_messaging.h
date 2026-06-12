// SPDX-License-Identifier: GPL-2.0
// native_messaging.h - MoeKoeMusic Native Host 协议处理（JSON Lines）
//
// 新协议（MoeKoeMusic 托管式 Native Host）:
//   stdin:  每行一条 JSON，末尾带 \n
//   stdout: 每行一条 JSON，末尾带 \n，写后立即 flush
//
// 消息格式:
//   收到: {"type":"message","payload":{...}}   业务消息
//   收到: {"type":"shutdown"}                   关闭指令
//   发出: {"type":"message","payload":{...}}   上报事件
#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>

namespace moekoe {

// Native Host 输入消息结构
struct NativeHostMessage {
    std::string type;           // "message" | "shutdown"
    nlohmann::json payload;     // 业务数据（当 type == "message" 时）
};

// Native Host 输出事件结构
struct NativeHostEvent {
    nlohmann::json payload;     // 要发回给插件的业务事件数据
};

// Native Host 处理器（JSON Lines 协议）
class NativeMessagingHost {
public:
    using MessageHandler = std::function<void(const NativeHostMessage&)>;

    NativeMessagingHost();
    ~NativeMessagingHost();

    // 设置消息处理回调（在独立线程中调用）
    void SetMessageHandler(MessageHandler handler);

    // 运行 stdin 读取循环（阻塞，直到 EOF 或 shutdown）
    // 返回 false 表示收到 shutdown 或读取错误，应退出程序
    bool Run();

    // 向 stdout 发送事件（JSON Lines 格式，立即 flush）
    void SendEvent(const NativeHostEvent& event);

    // 向 stdout 发送任意 payload 事件（便捷方法）
    void SendPayloadEvent(const nlohmann::json& payload);

    // 是否已收到 shutdown 指令
    bool IsShutdown() const { return !running_; }

private:
    MessageHandler handler_;
    bool running_ = true;
};

} // namespace moekoe
