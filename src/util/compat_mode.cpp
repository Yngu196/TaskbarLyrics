// SPDX-License-Identifier: GPL-3.0
// compat_mode.cpp - 兼容模式检测实现
#include "util/compat_mode.h"
#include "util/environment_check.h"
#include "util/logger.h"

namespace moekoe {

unsigned DetectCompatMode() {
    unsigned flags = CompatNone;

    const auto mods = DetectShellModifications();
    if (mods.empty()) return CompatNone;

    for (const auto& name : mods) {
        if (name == "StartAllBack" || name == "ExplorerPatcher") {
            // 这两个工具替换了任务栏内部结构：
            // - UIA 枚举可能返回异常数据
            // - 帧锁定双采样容易误判
            // - WinEvent 事件更频繁
            flags |= CompatNoFrameLock | CompatNoUIA | CompatSlowReposition;
            Log("[COMPAT] %s detected: enabling NoFrameLock + NoUIA + SlowReposition\n",
                name.c_str());
        } else if (name == "Windhawk") {
            // Windhawk 可安装各种 mod，不确定影响范围，保守禁用双采样
            flags |= CompatNoFrameLock;
            Log("[COMPAT] Windhawk detected: enabling NoFrameLock\n");
        }
        // TranslucentTB 仅修改任务栏透明度，不影响结构，无需降级
    }

    return flags;
}

const char* CompatFlagName(unsigned flag) {
    switch (flag) {
    case CompatNoFrameLock:    return "NoFrameLock";
    case CompatNoUIA:          return "NoUIA";
    case CompatSlowReposition: return "SlowReposition";
    default:                   return "Unknown";
    }
}

} // namespace moekoe
