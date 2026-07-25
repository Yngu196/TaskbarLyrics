// SPDX-License-Identifier: GPL-3.0
// environment_check.h - Windows 版本检测与运行环境校验
//
// 职责:
//   - 使用 RtlGetVersion 获取精确 Windows 版本号
//   - 检查运行环境依赖（Shell_TrayWnd / DWM / Explorer）
//   - 提供版本过低时的错误提示
//
#pragma once

#include <string>

namespace moekoe {

// Windows 版本信息
struct WindowsVersion {
    int  major{0};
    int  minor{0};
    int  build{0};
    std::string productName;      // "Windows 10 Pro" / "Windows 11 Home"
    std::string displayVersion;   // "22H2" / "23H2" (Win10 20H2+)
};

// 环境检查结果
struct EnvironmentStatus {
    WindowsVersion osVersion;
    bool versionSupported{false};      // >= 最低支持版本 (Build 19041)
    bool versionRecommended{false};    // >= 推荐版本 (Build 19045)
    bool shellTrayWndFound{false};
    bool dwmCompositionEnabled{false};
    bool explorerRunning{false};
};

// 最低支持版本: Windows 10 2004 (Build 19041)
constexpr int MIN_SUPPORTED_BUILD = 19041;

// 推荐版本: Windows 10 22H2 (Build 19045)
constexpr int RECOMMENDED_BUILD = 19045;

// 使用 RtlGetVersion (ntdll.dll) 获取精确 Windows 版本
// 比 GetVersionEx (Win8.1+ 谎报) 和 VerifyVersionInfo 更可靠
WindowsVersion GetWindowsVersion();

// 检查运行环境依赖
EnvironmentStatus CheckEnvironment();

// 将环境状态写入日志
// Info 级别输出正常项，Warn 级别输出异常项，Error 级别输出不支持版本
void LogEnvironmentStatus(const EnvironmentStatus& status);

} // namespace moekoe
