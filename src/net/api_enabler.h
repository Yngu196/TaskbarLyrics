// SPDX-License-Identifier: GPL-3.0
// api_enabler.h — 极简版：仅通过 Native Messaging 写 config.json（由 content.js 触发）
#pragma once

#include <string>

namespace moekoe {

class ApiEnabler {
public:
    // 获取 MoeKoeMusic electron-store 配置文件路径
    // 返回 %APPDATA%\moekoemusic\config.json，失败返回空字符串
    static std::string GetConfigPath();

    // 读-改-写 config.json，将 settings.apiMode 设置为 "on"
    // 原子写入（先写 .tmp 再 MoveFileEx），保留其他配置项不变
    // 返回 true 表示写入成功
    static bool WriteApiMode(const std::string& configPath);
};

} // namespace moekoe
