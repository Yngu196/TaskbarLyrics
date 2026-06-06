// SPDX-License-Identifier: GPL-2.0
// config_dialog.h - GUI 配置界面
//
// 职责:
//   - 提供外观配置（颜色、字体、卡拉OK、翻译）
//   - 提供高级配置（WebSocket 端口、刷新率）
//   - 实时预览颜色变化
//   - 保存后通知主程序刷新渲染器
//
#pragma once

#include "config.h"

#include <functional>
#include <windows.h>

namespace moekoe {

class ConfigDialog {
public:
    // 显示模态配置对话框，返回 true 表示用户点了"确定"并保存了配置
    // boundMode: 是否处于绑定模式（影响是否显示"解除绑定"按钮）
    // onUnbind: 解除绑定回调（由调用者提供退出逻辑）
    static bool Show(HINSTANCE hInstance, HWND parent, Config& config,
                     bool boundMode = false,
                     std::function<void()> onUnbind = nullptr);

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 初始化控件状态
    static void InitControls(HWND hwnd, const Config& config, bool boundMode);

    // 从控件读取配置
    static bool ReadControls(HWND hwnd, Config& config);

    // 颜色选择对话框
    static bool PickColor(HWND hwnd, HWND colorEdit);

    // 字体选择对话框
    static bool PickFont(HWND hwnd, HWND fontEdit);
};

} // namespace moekoe
