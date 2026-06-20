// SPDX-License-Identifier: GPL-2.0
// api_enabler.cpp - MoeKoeMusic API 模式自动检测与开启实现
#include "api_enabler.h"
#include "logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shellapi.h>

#include <fstream>
#include <vector>
#include <nlohmann/json.hpp>

namespace moekoe {

using json = nlohmann::json;

namespace {

// 防重复标记：本次运行周期内只尝试一次自动开启
static bool s_attempted = false;

} // namespace

// ═══════════════════════════════
// 公开接口
// ═══════════════════════════════

ApiEnableResult ApiEnabler::TryEnableApi() {
    // 防重复：每个运行周期只尝试一次（直到插件重启或用户手动触发）
    if (s_attempted) {
        return ApiEnableResult::AlreadyAttempted;
    }

    Log("TryEnableApi: starting check");

    // 1. 检测 MoeKoeMusic 进程
    if (!IsMoeKoeMusicRunning()) {
        Log("TryEnableApi: MoeKoeMusic process not found");
        return ApiEnableResult::ProcessNotFound;
    }

    // 2. 获取配置文件路径
    const std::string configPath = GetConfigPath();
    if (configPath.empty()) {
        Log("TryEnableApi: cannot determine config path");
        return ApiEnableResult::ConfigNotFound;
    }

    // 检查文件是否存在
    {
        DWORD attrs = GetFileAttributesA(configPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            Log("TryEnableApi: config file not found at " + configPath);
            return ApiEnableResult::ConfigNotFound;
        }
    }

    // 3. 读取当前 API 模式状态
    const std::string currentMode = ReadApiMode(configPath);
    if (currentMode.empty()) {
        Log("TryEnableApi: failed to read apiMode from config");
        return ApiEnableResult::ConfigReadError;
    }

    if (currentMode == "on") {
        Log("TryEnableApi: API mode is already ON");
        return ApiEnableResult::AlreadyOn;
    }

    // 4. 当前为 off → 尝试修改为 on
    Log("TryEnableApi: API mode is OFF, attempting to enable...");
    s_attempted = true;  // 标记已尝试

    if (!WriteApiMode(configPath)) {
        Log("TryEnableApi: failed to write config file");
        return ApiEnableResult::ConfigWriteError;
    }

    Log("TryEnableApi: successfully set apiMode to 'on' in config");

    // 5. 尝试重启 MoeKoeMusic 使配置立即生效
    bool restarted = RestartMoeKoeMusic();
    if (restarted) {
        Log("TryEnableApi: MoeKoeMusic restart triggered");
        return ApiEnableResult::EnabledAndRestarted;
    }

    Log("TryEnableApi: enabled but could not restart (user needs manual restart)");
    return ApiEnableResult::Enabled;
}

std::string ApiEnabler::ResultToString(ApiEnableResult result) {
    switch (result) {
    case ApiEnableResult::AlreadyOn:            return "API mode already enabled";
    case ApiEnableResult::Enabled:              return "API mode enabled (restart required)";
    case ApiEnableResult::EnabledAndRestarted:   return "API mode enabled & MoeKoeMusic restarting";
    case ApiEnableResult::ProcessNotFound:      return "MoeKoeMusic not running";
    case ApiEnableResult::ConfigNotFound:       return "Config file not found";
    case ApiEnableResult::ConfigReadError:      return "Failed to read config";
    case ApiEnableResult::ConfigWriteError:     return "Failed to write config";
    case ApiEnableResult::AlreadyAttempted:     return "Already attempted this session";
    default:                                   return "Unknown result";
    }
}

// ═══════════════════════════════
// 内部实现
// ═══════════════════════════════

bool ApiEnabler::IsMoeKoeMusicRunning() {
    // 方法1：通过窗口类名检测（最快）
    HWND h = FindWindowW(L"Chrome_WidgetWin_1", nullptr);  // Electron 窗口类
    if (!h) return false;

    // 进一步确认是 MoeKoeMusic：检查进程名
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return false;

    char path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    QueryFullProcessImageNameA(hProc, 0, path, &size);
    CloseHandle(hProc);

    // 检查可执行文件名是否包含 moekoemusic（不区分大小写）
    std::string exePath(path);
    for (auto& c : exePath) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // 可能的 exe 名: MoeKoe Music.exe, moekoemusic.exe 等
    if (exePath.find("moekoemusic") != std::string::npos ||
        exePath.find("moejue") != std::string::npos ||
        exePath.find("moekoe music") != std::string::npos) {
        Log("IsMoeKoeMusicRunning: found process PID=" + std::to_string(pid) + " path=" + path);
        return true;
    }

    // 方法2：通过 CreateToolhelp32Snapshot 遍历进程列表
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring exeName(pe.szExeFile);
            // 统一转小写比较
            for (auto& c : exeName)
                c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
            if (exeName.find(L"moekoemusic") != std::wstring::npos ||
                exeName.find(L"moejue") != std::wstring::npos ||
                exeName.find(L"moekoe music") != std::wstring::npos) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (found) {
        Log("IsMoeKoeMusicRunning: found via snapshot");
    }
    return found;
}

std::string ApiEnabler::GetConfigPath() {
    char appdata[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        return "";
    }
    // electron-store 默认路径: %APPDATA%/<name>/config.json
    // name 来自 package.json 的 "name": "moekoemusic"
    return std::string(appdata) + "\\moekoemusic\\config.json";
}

std::string ApiEnabler::ReadApiMode(const std::string& configPath) {
    try {
        std::ifstream f(configPath);
        if (!f.is_open()) return "";

        json j;
        f >> j;

        if (j.contains("settings") && j["settings"].is_object()) {
            const auto& settings = j["settings"];
            if (settings.contains("apiMode") && settings["apiMode"].is_string()) {
                return settings["apiMode"].get<std::string>();
            }
        }
        return "";  // 字段不存在
    } catch (const std::exception& e) {
        Log("ReadApiMode exception: " + std::string(e.what()));
        return "";
    } catch (...) {
        return "";
    }
}

bool ApiEnabler::WriteApiMode(const std::string& configPath) {
    try {
        std::ifstream inFile(configPath);
        if (!inFile.is_open()) return false;

        json j;
        inFile >> j;
        inFile.close();

        // 确保 settings 对象存在
        if (!j.contains("settings") || !j["settings"].is_object()) {
            j["settings"] = json::object();
        }

        // 设置 apiMode 为 on
        j["settings"]["apiMode"] = "on";

        // 原子写入：先写临时文件，再重命名（防止写入中途崩溃导致损坏）
        const std::string tmpPath = configPath + ".tmp";
        {
            std::ofstream outFile(tmpPath, std::ios::trunc);
            if (!outFile.is_open()) return false;
            outFile << j.dump(2);  // 格式化输出便于调试
            outFile.close();
            if (outFile.fail()) return false;
        }

        // 替换原文件
        if (!MoveFileExA(tmpPath.c_str(), configPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // 回退：直接写入
            std::ofstream outFinal(configPath, std::ios::trunc);
            if (!outFinal.is_open()) return false;
            outFinal << j.dump(2);
            outFinal.close();
            return !outFinal.fail();
        }

        return true;
    } catch (const std::exception& e) {
        Log("WriteApiMode exception: " + std::string(e.what()));
        return false;
    } catch (...) {
        return false;
    }
}

bool ApiEnabler::RestartMoeKoeMusic() {
    // 通过快照找到所有 MoeKoeMusic 进程
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    std::wstring exePath;
    std::vector<DWORD> pids;  // 收集所有 MoeKoeMusic 进程 PID

    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name(pe.szExeFile);
            for (auto& c : name)
                c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
            if (name.find(L"moekoemusic") != std::wstring::npos ||
                name.find(L"moejue") != std::wstring::npos ||
                name.find(L"moekoe music") != std::wstring::npos) {

                // 获取完整 exe 路径（只需一次）
                if (exePath.empty()) {
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                               FALSE, pe.th32ProcessID);
                    if (hProc) {
                        wchar_t buf[MAX_PATH] = {};
                        DWORD sz = MAX_PATH;
                        if (QueryFullProcessImageNameW(hProc, 0, buf, &sz)) {
                            exePath = buf;
                        }
                        CloseHandle(hProc);
                    }
                }
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (exePath.empty() || pids.empty()) {
        Log("RestartMoeKoeMusic: could not find executable path or processes");
        return false;
    }

    Log("RestartMoeKoeMusic: found " + std::to_string(pids.size()) +
        " process(es), exe=" + std::string(exePath.begin(), exePath.end()));

    // 1. 先终止所有旧实例（否则新实例因单实例锁立即退出，读不到新配置）
    for (DWORD pid : pids) {
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (hProc) {
            TerminateProcess(hProc, 0);
            DWORD waitResult = WaitForSingleObject(hProc, 5000);  // 等待进程完全退出
            if (waitResult == WAIT_TIMEOUT) {
                // 超时后检测进程是否仍存在
                HANDLE hCheck = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
                if (hCheck) {
                    CloseHandle(hCheck);
                    Log("RestartMoeKoeMusic: process PID=" + std::to_string(pid) +
                        " still alive after TerminateProcess timeout. "
                        "Skipping restart — user must manually close MoeKoeMusic before the new API settings take effect.");
                    CloseHandle(hProc);
                    return false;
                }
                Log("RestartMoeKoeMusic: process PID=" + std::to_string(pid) +
                    " terminated but handle not yet signaled");
            }
            CloseHandle(hProc);
            Log("RestartMoeKoeMusic: terminated old process PID=" + std::to_string(pid));
        }
    }

    // 2. 短暂等待单实例锁释放
    Sleep(500);

    // 3. 启动新实例（此时无旧实例，新实例读取 apiMode=on 的配置文件，正常启动 API）
    HINSTANCE result = ShellExecuteW(
        nullptr, L"open", exePath.c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);

    bool launched = (reinterpret_cast<intptr_t>(result) > 32);
    if (!launched) {
        Log("RestartMoeKoeMusic: ShellExecute failed");
        return false;
    }

    Log("RestartMoeKoeMusic: launched new instance successfully");
    return true;
}

} // namespace moekoe
