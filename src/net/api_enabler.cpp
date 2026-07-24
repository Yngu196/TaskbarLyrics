// SPDX-License-Identifier: GPL-3.0
// api_enabler.cpp — 极简版：仅写 config.json 的 settings.apiMode
// 不检测进程、不 Kill、不重启（重启由用户在 MoeKoeMusic UI 中手动完成）
#include "net/api_enabler.h"
#include "util/logger.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <nlohmann/json.hpp>

namespace moekoe {

using json = nlohmann::json;

std::string ApiEnabler::GetConfigPath() {
    char appdata[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        Log("[API-ENABLER] SHGetFolderPathA failed\n");
        return "";
    }
    return std::string(appdata) + "\\moekoemusic\\config.json";
}

bool ApiEnabler::WriteApiMode(const std::string& configPath) {
    try {
        // 读取现有配置
        std::ifstream inFile(configPath);
        if (!inFile.is_open()) {
            Log("[API-ENABLER] Cannot open config for reading: %s\n", configPath.c_str());
            return false;
        }

        json j;
        inFile >> j;
        inFile.close();

        // 确保 settings 对象存在
        if (!j.contains("settings") || !j["settings"].is_object()) {
            j["settings"] = json::object();
        }

        const std::string previousMode = j["settings"].value("apiMode", "");
        if (previousMode == "on") {
            Log("[API-ENABLER] apiMode already 'on', skipping write\n");
            return true;
        }

        // 设置为 on
        j["settings"]["apiMode"] = "on";

        // 原子写入：先写 .tmp，再 MoveFileEx 替换
        const std::string tmpPath = configPath + ".tmp";
        {
            std::ofstream outFile(tmpPath, std::ios::trunc);
            if (!outFile.is_open()) {
                Log("[API-ENABLER] Cannot open tmp file for writing: %s\n", tmpPath.c_str());
                return false;
            }
            outFile << j.dump(2);
            outFile.close();
            if (outFile.fail()) {
                Log("[API-ENABLER] Write to tmp file failed\n");
                return false;
            }
        }

        if (!MoveFileExA(tmpPath.c_str(), configPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // 回退：直接写入
            Log("[API-ENABLER] MoveFileEx failed, fallback to direct write\n");
            std::ofstream outFinal(configPath, std::ios::trunc);
            if (!outFinal.is_open()) {
                Log("[API-ENABLER] Direct write failed\n");
                return false;
            }
            outFinal << j.dump(2);
            outFinal.close();
            return !outFinal.fail();
        }

        Log("[API-ENABLER] Successfully set apiMode='on' (was '%s')\n", previousMode.c_str());
        return true;
    } catch (const std::exception& e) {
        Log("[API-ENABLER] Exception: %s\n", e.what());
        return false;
    } catch (...) {
        Log("[API-ENABLER] Unknown exception\n");
        return false;
    }
}

} // namespace moekoe
