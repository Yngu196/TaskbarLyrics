// SPDX-License-Identifier: GPL-3.0
// environment_check.cpp - Windows 版本检测与运行环境校验实现
#include "util/environment_check.h"

#include "util/logger.h"

#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>

#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

namespace moekoe {

namespace {

// 通过进程快照检测指定进程是否运行
bool IsProcessRunning(const wchar_t* processName) {
    HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    BOOL ok = ::Process32FirstW(hSnap, &pe);
    while (ok) {
        if (_wcsicmp(pe.szExeFile, processName) == 0) {
            ::CloseHandle(hSnap);
            return true;
        }
        ok = ::Process32NextW(hSnap, &pe);
    }
    ::CloseHandle(hSnap);
    return false;
}

// 从注册表读取字符串值
std::string ReadRegistryString(HKEY root, const char* subKey, const char* valueName) {
    HKEY hKey = nullptr;
    // 使用 KEY_WOW64_64KEY 确保 64 位系统上读取正确的注册表视图
    LONG ret = ::RegOpenKeyExA(root, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &hKey);
    if (ret != ERROR_SUCCESS) return {};

    DWORD type = 0;
    DWORD size = 0;
    ret = ::RegQueryValueExA(hKey, valueName, nullptr, &type, nullptr, &size);
    if (ret != ERROR_SUCCESS || type != REG_SZ || size == 0) {
        ::RegCloseKey(hKey);
        return {};
    }

    std::string result(size, '\0');
    ret = ::RegQueryValueExA(hKey, valueName, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&result[0]), &size);
    ::RegCloseKey(hKey);

    if (ret != ERROR_SUCCESS) return {};

    // 移除末尾的 null terminator
    while (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

} // namespace

WindowsVersion GetWindowsVersion() {
    WindowsVersion result{};

    // 方法 1: RtlGetVersion (ntdll.dll) — 最可靠的版本获取方式
    // GetVersionEx 在 Win8.1+ 会谎报版本（除非应用程序声明了兼容清单），
    // RtlGetVersion 始终返回真实版本号。
    HMODULE hNtDll = ::GetModuleHandleW(L"ntdll.dll");
    if (hNtDll) {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        auto pRtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            ::GetProcAddress(hNtDll, "RtlGetVersion"));
        if (pRtlGetVersion) {
            RTL_OSVERSIONINFOW osvi{};
            osvi.dwOSVersionInfoSize = sizeof(osvi);
            if (pRtlGetVersion(&osvi) == 0) {
                result.major = static_cast<int>(osvi.dwMajorVersion);
                result.minor = static_cast<int>(osvi.dwMinorVersion);
                result.build = static_cast<int>(osvi.dwBuildNumber);
            }
        }
    }

    // 方法 2: 从注册表读取产品名称和显示版本
    // HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion
    //   ProductName    = "Windows 10 Pro" / "Windows 11 Home"
    //   DisplayVersion = "22H2" (仅 Win10 20H2+)
    result.productName = ReadRegistryString(
        HKEY_LOCAL_MACHINE,
        R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)",
        "ProductName");
    result.displayVersion = ReadRegistryString(
        HKEY_LOCAL_MACHINE,
        R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)",
        "DisplayVersion");

    return result;
}

EnvironmentStatus CheckEnvironment() {
    EnvironmentStatus status{};

    // Windows 版本
    status.osVersion = GetWindowsVersion();
    status.versionSupported = (status.osVersion.build >= MIN_SUPPORTED_BUILD);
    status.versionRecommended = (status.osVersion.build >= RECOMMENDED_BUILD);

    // Shell_TrayWnd 是否存在
    status.shellTrayWndFound = (::FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr);

    // DWM 合成是否启用
    BOOL dwmEnabled = FALSE;
    ::DwmIsCompositionEnabled(&dwmEnabled);
    status.dwmCompositionEnabled = (dwmEnabled == TRUE);

    // Explorer.exe 是否运行
    status.explorerRunning = IsProcessRunning(L"explorer.exe");

    return status;
}

void LogEnvironmentStatus(const EnvironmentStatus& status) {
    const auto& v = status.osVersion;

    // 基本信息
    char versionStr[64];
    std::snprintf(versionStr, sizeof(versionStr), "%d.%d.%d", v.major, v.minor, v.build);

    std::string fullName;
    if (!v.productName.empty()) {
        fullName = v.productName;
        if (!v.displayVersion.empty()) {
            fullName += " (" + v.displayVersion + ")";
        }
    } else {
        fullName = "Windows (version unknown)";
    }

    Log("[ENV] %s %s\n", fullName.c_str(), versionStr);

    // 版本支持状态
    if (status.versionSupported) {
        if (status.versionRecommended) {
            Log("[ENV] Build %d: supported=YES, recommended=YES\n", v.build);
        } else {
            LogWarn("[ENV] Build %d: supported but below recommended (%d), some features may not work\n",
                    v.build, RECOMMENDED_BUILD);
        }
    } else {
        LogError("[ENV] Build %d: below minimum supported (%d), application may not function correctly\n",
                 v.build, MIN_SUPPORTED_BUILD);
    }

    // 环境依赖
    if (status.shellTrayWndFound && status.dwmCompositionEnabled && status.explorerRunning) {
        Log("[ENV] Shell_TrayWnd: found | DWM: enabled | Explorer: running\n");
    } else {
        if (!status.shellTrayWndFound) {
            LogWarn("[ENV] Shell_TrayWnd: NOT found\n");
        }
        if (!status.dwmCompositionEnabled) {
            LogWarn("[ENV] DWM composition: disabled\n");
        }
        if (!status.explorerRunning) {
            LogWarn("[ENV] Explorer.exe: NOT running\n");
        }
    }
}

} // namespace moekoe
