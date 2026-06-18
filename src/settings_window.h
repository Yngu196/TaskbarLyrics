// SPDX-License-Identifier: GPL-2.0
// settings_window.h - WebView2 设置界面
//
// 职责:
//   - 创建 WebView2 宿主窗口，加载 resources/settings.html
//   - 通过 PostWebMessageAsJson 与 JS 双向通信
//   - 接收 JS 发来的配置变更并实时应用到渲染器
//
#pragma once

#include "config.h"

#include <functional>
#include <string>
#include <windows.h>

namespace moekoe {

class SettingsWindow {
public:
    using ConfigChangedCallback = std::function<void(const Config&)>;
    using SwitchModeCallback    = std::function<void(const std::string& mode)>; // "webview" / "d2d"

    SettingsWindow();
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // 显示设置窗口（非模态）
    // 返回 true 表示成功创建（WebView2 可能在异步初始化中）
    // 返回 false 表示完全失败（应回退到其他方案）
    bool Show(HINSTANCE hInstance, HWND parent, const Config& currentConfig);

    // WebView2 是否已成功初始化并加载了页面
    bool IsWebViewReady() const { return webView2_ != nullptr; }
    bool IsWebViewInitFailed() const { return webViewInitFailed_; }

    // 注册配置变更回调（JS 点"应用并保存"时触发）
    void OnConfigChanged(ConfigChangedCallback cb) { onConfigChanged_ = std::move(cb); }

    // 注册 UI 模式切换回调（JS 点"切换到原生设置"时触发）
    void OnSwitchMode(SwitchModeCallback cb) { onSwitchMode_ = std::move(cb); }

    // 标记 WebView 初始化失败（由 NavigationCompleted 调用）
    void SetWebViewInitFailed() { webViewInitFailed_ = true; }

    // 获取窗口句柄（供 COM 回调类使用）
    HWND GetHwnd() const { return hwnd_; }

    // 是否正在显示
    bool IsVisible() const;

    // 关闭窗口
    void Close();

    // 获取当前配置（供 NavigationCompletedHandler 发送到 WebView）
    const Config& GetCurrentConfig() const { return currentConfig_; }

    // WebView2 异步回调（由 COM 回调类调用）
    void OnEnvironmentReady(void* env);
    void OnControllerReady(void* controller);
    void OnWebMessageReceived(const std::string& json);
    void SendConfigToWebView(const Config& cfg);
    void ApplyConfigFromJson(void* jsonPtr);
    void PickFont();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND  hwnd_{nullptr};
    HINSTANCE hInstance_{nullptr};

    // WebView2 接口（void* 以避免头文件暴露，在 .cpp 中强转）
    void* webView2Environment_{nullptr};      // ICoreWebView2Environment*
    void* webView2Controller_{nullptr};        // ICoreWebView2Controller*
    void* webView2_{nullptr};                  // ICoreWebView2*

    std::wstring settingsUrl_;
    Config currentConfig_;
    ConfigChangedCallback onConfigChanged_;
    SwitchModeCallback    onSwitchMode_;
    bool webViewInitFailed_{false};  // WebView2 初始化是否失败（导航/加载等）

    static constexpr const wchar_t* kWindowClass = L"MoeKoeTaskbarLyricsSettingsClass";
};

} // namespace moekoe
