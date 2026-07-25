// SPDX-License-Identifier: GPL-3.0
// diagnostic_exporter.h - 兼容性诊断信息收集与导出
//
// 职责:
//   - 收集系统环境信息（OS版本/DPI/任务栏/DWM/GPU等）
//   - 收集运行时状态（WebSocket/进程/配置快照）
//   - 导出为文本文件，方便用户反馈问题时附上
//
#pragma once

#include "core/app_context.h"

#include <string>
#include <vector>

namespace moekoe {

struct DiagnosticInfo {
    // Windows 版本
    std::string osVersion;         // "10.0.19045"
    std::string osBuild;           // "19045"
    std::string osProductName;     // "Windows 10 Pro"
    std::string osDisplayVersion;  // "22H2"
    std::string osArchitecture;    // "x64" / "x86" / "ARM64"

    // DPI
    std::string dpiScaling;        // "150% (144 DPI)"

    // 任务栏
    bool        taskbarFound{false};
    std::string taskbarPosition;   // "BOTTOM" / "TOP" / "LEFT" / "RIGHT" / "UNKNOWN"
    bool        taskbarAutoHide{false};
    bool        taskbarVertical{false};

    // DWM
    bool        dwmCompositionEnabled{false};

    // 进程
    bool        explorerRunning{false};

    // WebSocket
    bool        wsConnected{false};

    // GPU
    std::string gpuDescription;    // "NVIDIA GeForce RTX 3080"

    // 配置快照（敏感字段已遮蔽）
    std::string configSnapshot;

    // 插件版本
    std::string pluginVersion;

    // 日志路径
    std::string logFilePath;

    // 第三方 Shell 修改工具
    std::vector<std::string> shellModifications;

    // 日志尾部（最后 N 行）
    std::string logTail;
};

// 收集系统诊断信息
DiagnosticInfo CollectDiagnostics(const AppContext& app);

// 将诊断信息格式化为可读文本
std::string FormatDiagnosticText(const DiagnosticInfo& info);

// 导出诊断信息到用户指定文件（使用 IFileSaveDialog）
// 返回 true 表示成功导出
bool ExportDiagnosticFile(const AppContext& app, HWND ownerWnd);

} // namespace moekoe
