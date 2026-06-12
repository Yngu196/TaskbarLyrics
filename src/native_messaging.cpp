// SPDX-License-Identifier: GPL-2.0
// native_messaging.cpp - MoeKoeMusic Native Host 实现（JSON Lines 协议）
//
// 与旧版 Chrome Native Messaging 的区别:
//   - 旧: [4字节 uint32 LE 长度前缀][JSON]  (二进制模式)
//   - 新: {JSON}\n                       (纯文本 UTF-8 行模式)
//   - 旧: stdin EOF → Chrome 断开连接
//   - 新: {"type":"shutdown"} → MoeKoeMusic 要求退出
//   - 独立运行时 stdin 无写入者，读到 EOF 不退出（非托管模式）
#include "native_messaging.h"
#include "logger.h"

#include <iostream>
#include <string>
#include <thread>
#include <mutex>

namespace moekoe {

NativeMessagingHost::NativeMessagingHost() = default;

NativeMessagingHost::~NativeMessagingHost() = default;

void NativeMessagingHost::SetMessageHandler(MessageHandler handler) {
    handler_ = std::move(handler);
}

bool NativeMessagingHost::Run() {
    // JSON Lines 循环：逐行读取 stdin
    std::string line;
    while (running_ && std::getline(std::cin, line)) {
        // 跳过空行
        if (line.empty()) continue;

        try {
            auto j = nlohmann::json::parse(line);

            NativeHostMessage msg;
            msg.type = j.value("type", "");
            msg.payload = j.value("payload", nlohmann::json::object());

            if (msg.type == "shutdown") {
                Log("[NATIVE-HOST] Received shutdown command\n");
                running_ = false;

                // 回复 shutdown 确认（让主程序知道我们收到了）
                nlohmann::json ack;
                ack["type"] = "message";
                ack["payload"] = { {"event", "shutdown_ack"} };
                std::cout << ack.dump() << '\n' << std::flush;

                return false;
            }

            if (handler_) {
                handler_(msg);
            }
        } catch (const nlohmann::json::parse_error& e) {
            Log("[NATIVE-HOST] JSON parse error: %s\n", e.what());
            // 单行解析失败不终止循环，跳过继续
            continue;
        }
    }

    // stdin EOF（管道关闭或无数据）
    // 独立运行模式下这是正常的（没有托管者写 stdin），不应退出
    if (std::cin.eof()) {
        Log("[NATIVE-HOST] stdin EOF (standalone mode or pipe closed)\n");
        return true;  // 返回 true 表示「不是 shutdown」，调用者决定是否退出
    }

    // 其他读取错误
    Log("[NATIVE-HOST] stdin read error\n");
    running_ = false;
    return false;
}

void NativeMessagingHost::SendEvent(const NativeHostEvent& event) {
    nlohmann::json j;
    j["type"] = "message";
    j["payload"] = event.payload;

    std::cout << j.dump() << '\n' << std::flush;
}

void NativeMessagingHost::SendPayloadEvent(const nlohmann::json& payload) {
    SendEvent({ payload });
}

} // namespace moekoe
