// SPDX-License-Identifier: GPL-2.0
// settings_window.cpp - WebView2 设置界面实现
//
// 使用 WebView2 SDK（Windows 10/11 自带或 Edge Runtime）
// 通过 PostWebMessageAsJson 与 JS 双向通信
#include "settings_window.h"
#include "logger.h"

#include <shlobj.h>
#include <windows.h>

// 确保 LF_FACESIZE 定义（如果 wingdi.h 没定义的话）
#ifndef LF_FACESIZE
#define LF_FACESIZE 32
#endif

#include <WebView2.h>

#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <wrl/client.h>  // 仅用于 ComPtr

namespace moekoe {

using json = nlohmann::json;
using Microsoft::WRL::ComPtr;

constexpr UINT WM_PICK_FONT = WM_USER + 100;

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    int written = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    if (written <= 0) return {};
    // 因为 len 是不包括 null 终止符的字符数，所以我们不需要再处理，std::wstring 已经正确构造了
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    int written = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len, nullptr, nullptr);
    if (written <= 0) return {};
    return out;
}

} // namespace

// ============================================================
// 原生 COM 回调实现（不依赖 WRL RuntimeClass）
// ============================================================

class EnvironmentCompletedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    EnvironmentCompletedHandler(SettingsWindow* owner) : owner_(owner) { refCount_ = 1; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c <= 0) delete this;
        return static_cast<ULONG>(c > 0 ? c : 0);
    }

    // ICoreWebView2CreateEnvironmentCompletedHandler
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        try {
            moekoe::Log("[SETTINGS] Env created, hr=0x%08lX\n", result);
            if (SUCCEEDED(result) && owner_ && env) {
                owner_->OnEnvironmentReady(env);
            }
            return S_OK;
        } catch (...) {
            moekoe::Log("[SETTINGS] Env callback threw exception\n");
            return E_FAIL;
        }
    }

private:
    SettingsWindow* owner_;
    volatile LONG refCount_;
};

class ControllerCompletedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    ControllerCompletedHandler(SettingsWindow* owner) : owner_(owner) { refCount_ = 1; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c <= 0) delete this;
        return static_cast<ULONG>(c > 0 ? c : 0);
    }

    // ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        try {
            moekoe::Log("[SETTINGS] Controller created, hr=0x%08lX\n", result);
            if (SUCCEEDED(result) && owner_ && controller) {
                owner_->OnControllerReady(controller);
            }
            return S_OK;
        } catch (...) {
            moekoe::Log("[SETTINGS] Controller callback threw exception\n");
            return E_FAIL;
        }
    }

private:
    SettingsWindow* owner_;
    volatile LONG refCount_;
};

class WebMessageReceivedHandler : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    WebMessageReceivedHandler(SettingsWindow* owner) : owner_(owner) { refCount_ = 1; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler)) {
            *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c <= 0) delete this;
        return static_cast<ULONG>(c > 0 ? c : 0);
    }

    // ICoreWebView2WebMessageReceivedEventHandler
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override {
        try {
            LPWSTR msg = nullptr;
            HRESULT hr = args->TryGetWebMessageAsString(&msg);
            if (SUCCEEDED(hr) && msg && owner_) {
                int len = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    std::string utf8(static_cast<size_t>(len), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, msg, -1, &utf8[0], len, nullptr, nullptr);
                    utf8.pop_back();
                    owner_->OnWebMessageReceived(utf8);
                }
                CoTaskMemFree(msg);
            }
            return S_OK;
        } catch (...) {
            moekoe::Log("[SETTINGS] WebMessage callback threw exception\n");
            return E_FAIL;
        }
    }

private:
    SettingsWindow* owner_;
    volatile LONG refCount_;
};

class NavigationCompletedHandler : public ICoreWebView2NavigationCompletedEventHandler {
public:
    NavigationCompletedHandler(SettingsWindow* owner) : owner_(owner) { refCount_ = 1; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2NavigationCompletedEventHandler)) {
            *ppv = static_cast<ICoreWebView2NavigationCompletedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c <= 0) delete this;
        return static_cast<ULONG>(c > 0 ? c : 0);
    }

    // ICoreWebView2NavigationCompletedEventHandler
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2NavigationCompletedEventArgs* args) override {
        try {
            BOOL isSuccess = FALSE;
            COREWEBVIEW2_WEB_ERROR_STATUS webErrorStatus = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
            args->get_IsSuccess(&isSuccess);
            args->get_WebErrorStatus(&webErrorStatus);

            moekoe::Log("[SETTINGS] NavigationCompleted: success=%d, error=%d\n",
                     static_cast<int>(isSuccess), static_cast<int>(webErrorStatus));

            if (!isSuccess) {
                moekoe::Log("[SETTINGS] Navigation FAILED! Error code: %d\n", webErrorStatus);
                if (owner_) {
                    owner_->SetWebViewInitFailed();
                    MessageBoxW(owner_->GetHwnd(),
                        L"设置页面加载失败。\n将回退到基础设置界面。",
                        L"MoeKoe Taskbar Lyrics", MB_OK | MB_ICONWARNING);
                }
            } else {
                moekoe::Log("[SETTINGS] Navigation OK - settings.html loaded successfully\n");
                if (owner_) {
                    owner_->SendConfigToWebView(owner_->GetCurrentConfig());
                }
            }

            return S_OK;
        } catch (...) {
            moekoe::Log("[SETTINGS] Navigation callback threw exception\n");
            return E_FAIL;
        }
    }

private:
    SettingsWindow* owner_;
    volatile LONG refCount_;
};

// ============================================================
// SettingsWindow 实现
// ============================================================

SettingsWindow::SettingsWindow() = default;

SettingsWindow::~SettingsWindow() { Close(); }

bool SettingsWindow::IsVisible() const {
    return hwnd_ && ::IsWindowVisible(hwnd_);
}

void SettingsWindow::Close() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<SettingsWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<SettingsWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE; // 必须返回 TRUE 才能继续创建窗口！
    }
    case WM_PICK_FONT: {
        if (self) self->PickFont();
        return 0;
    }
    case WM_SIZE: {
        if (self && self->webView2Controller_) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            static_cast<ICoreWebView2Controller*>(self->webView2Controller_)->put_Bounds(rc);
        }
        return 0;
    }
    case WM_DESTROY:
        if (self) {
            if (self->webView2_) {
                static_cast<ICoreWebView2*>(self->webView2_)->Release();
                self->webView2_ = nullptr;
            }
            if (self->webView2Controller_) {
                static_cast<ICoreWebView2Controller*>(self->webView2Controller_)->Release();
                self->webView2Controller_ = nullptr;
            }
            if (self->webView2Environment_) {
                static_cast<ICoreWebView2Environment*>(self->webView2Environment_)->Release();
                self->webView2Environment_ = nullptr;
            }
            self->hwnd_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool SettingsWindow::Show(HINSTANCE hInstance, HWND /*parent*/, const Config& currentConfig) {
    if (hwnd_) {
        ::SetForegroundWindow(hwnd_);
        return true;
    }

    hInstance_ = hInstance;
    currentConfig_ = currentConfig;

    // 加载图标
    HICON hIcon = nullptr;
    wchar_t exeDir[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';
    std::wstring iconPath = std::wstring(exeDir) + L"\\resources\\icon.ico";
    hIcon = static_cast<HICON>(::LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    if (!hIcon) {
        hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = hIcon;
    wc.hIconSm = hIcon;
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(
        0, kWindowClass, L"任务栏歌词 - 设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 620,
        nullptr, nullptr, hInstance, this);

    if (!hwnd_) {
        if (hIcon) ::DestroyIcon(hIcon);
        return false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    // 居中显示窗口（相对于桌面）
    RECT desktopRect{}, windowRect{};
    GetWindowRect(GetDesktopWindow(), &desktopRect);
    GetWindowRect(hwnd_, &windowRect);
    int x = (desktopRect.right - desktopRect.left - (windowRect.right - windowRect.left)) / 2;
    int y = (desktopRect.bottom - desktopRect.top - (windowRect.bottom - windowRect.top)) / 3;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    // 构建 file:// URL（复用前面获取的 exeDir）
    settingsUrl_ = std::wstring(L"file:///") + exeDir + L"\\resources\\settings.html";
    for (auto& c : settingsUrl_) { if (c == L'\\') c = L'/'; }

    moekoe::Log("[SETTINGS] URL=%ls\n", settingsUrl_.c_str());

    // 创建 WebView2 环境
    auto* envHandler = new EnvironmentCompletedHandler(this);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr, envHandler);
    envHandler->Release();

    if (FAILED(hr)) {
        moekoe::Log("[SETTINGS] CreateCoreWebView2EnvironmentWithOptions failed: 0x%08lX\n", hr);
        webViewInitFailed_ = true;
        MessageBoxW(hwnd_,
            L"无法创建 WebView2 环境。\n请确保已安装 Microsoft Edge WebView2 Runtime。\n将回退到基础设置界面。",
            L"MoeKoe Taskbar Lyrics", MB_OK | MB_ICONWARNING);
        // 窗口已创建但 WebView2 不可用，返回 false 让调用方回退
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // Env 创建成功（异步初始化中），窗口已显示
    return true;
}

void SettingsWindow::OnEnvironmentReady(void* env) {
    webView2Environment_ = env;
    static_cast<ICoreWebView2Environment*>(env)->AddRef();

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    auto* ctrlHandler = new ControllerCompletedHandler(this);
    HRESULT hr = static_cast<ICoreWebView2Environment*>(env)->CreateCoreWebView2Controller(
        hwnd_, ctrlHandler);
    ctrlHandler->Release();

    if (FAILED(hr)) {
        moekoe::Log("[SETTINGS] CreateCoreWebView2Controller failed: 0x%08lX\n", hr);
    }
}

void SettingsWindow::OnControllerReady(void* controller) {
    webView2Controller_ = controller;
    static_cast<ICoreWebView2Controller*>(controller)->AddRef();

    ComPtr<ICoreWebView2> webview;
    static_cast<ICoreWebView2Controller*>(controller)->get_CoreWebView2(webview.GetAddressOf());
    if (webview) {
        webView2_ = webview.Detach();
        auto* wv = static_cast<ICoreWebView2*>(webView2_);
        auto* wvController = static_cast<ICoreWebView2Controller*>(webView2Controller_);

        // 0. 设置 WebView 的 Bounds 和可见性（必须做！否则窗口空白）
        RECT clientRect{};
        GetClientRect(hwnd_, &clientRect);
        wvController->put_Bounds(clientRect);
        wvController->put_IsVisible(TRUE);

        // 1. 验证 settings.html 是否存在
        wchar_t htmlPath[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, htmlPath, MAX_PATH);
        wchar_t* slash = wcsrchr(htmlPath, L'\\');
        if (slash) *slash = L'\0';
        wcscat_s(htmlPath, L"\\resources\\settings.html");

        DWORD attr = GetFileAttributesW(htmlPath);
        bool htmlExists = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
        moekoe::Log("[SETTINGS] HTML path=%ls exists=%d\n", htmlPath, (int)htmlExists);

        if (!htmlExists) {
            moekoe::Log("[SETTINGS] ERROR: settings.html NOT FOUND!\n");
            SetWebViewInitFailed();
            MessageBoxW(hwnd_,
                L"设置页面文件不存在。\n请确保 resources\\settings.html 在 exe 同级目录。\n将回退到基础设置界面。",
                L"MoeKoe Taskbar Lyrics", MB_OK | MB_ICONWARNING);
            return;
        }

        // 2. 注册 NavigationCompleted 事件
        EventRegistrationToken navToken{};
        auto* navHandler = new NavigationCompletedHandler(this);
        wv->add_NavigationCompleted(navHandler, &navToken);
        navHandler->Release();

        // 3. 导航到 settings.html
        wv->Navigate(settingsUrl_.c_str());

        // 4. 注册 WebMessageReceived 事件
        EventRegistrationToken msgToken{};
        auto* msgHandler = new WebMessageReceivedHandler(this);
        wv->add_WebMessageReceived(msgHandler, &msgToken);
        msgHandler->Release();

        moekoe::Log("[SETTINGS] WebView2 ready, navigating to settings.html...\n");
    }
}

void SettingsWindow::OnWebMessageReceived(const std::string& jsonStr) {
    try {
        json msg = json::parse(jsonStr);
        std::string type = msg.value("type", "");

        moekoe::Log("[SETTINGS] recv: type=%s\n", type.c_str());

        if (type == "getConfig") {
        SendConfigToWebView(currentConfig_);
    } else if (type == "saveConfig") {
        if (msg.contains("config")) {
            ApplyConfigFromJson(reinterpret_cast<void*>(&msg["config"]));
            currentConfig_.Save();
            if (onConfigChanged_) onConfigChanged_(currentConfig_);
            Close();
        }
    } else if (type == "pickFont") {
        ::PostMessage(hwnd_, WM_PICK_FONT, 0, 0);
    } else if (type == "close") {
        Close();
    } else if (type == "switchMode") {
        std::string mode = msg.value("mode", "");
        moekoe::Log("[SETTINGS] switchMode: %s\n", mode.c_str());
        // 保存模式到配置
        currentConfig_.MutableAdvanced().settingsUiMode = mode;
        currentConfig_.Save();
        // 通知主程序（由主程序决定是否关闭并打开新模式窗口）
        if (onSwitchMode_) onSwitchMode_(mode);
    }
    } catch (const std::exception& e) {
        moekoe::Log("[SETTINGS] OnWebMessage error: %s\n", e.what());
    }
}

void SettingsWindow::PickFont() {
    LOGFONTW lf{};
    // 安全初始化lf.lfFaceName，截断过长的字符串
    std::wstring wFont = Utf8ToWide(currentConfig_.Appearance().fontFamily);
    if (wFont.size() >= LF_FACESIZE) {
        wFont.resize(LF_FACESIZE - 1);
    }
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, wFont.c_str());
    lf.lfHeight = -14;

    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hwnd_;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;

    if (::ChooseFontW(&cf)) {
        // 更新SettingsWindow的currentConfig_
        currentConfig_.MutableAppearance().fontFamily = WideToUtf8(lf.lfFaceName);
        // 把选中的字体发给WebView
        json j{};
        j["type"] = "fontSelected";
        j["fontFamily"] = currentConfig_.Appearance().fontFamily;
        std::wstring jsonW = Utf8ToWide(j.dump());
        static_cast<ICoreWebView2*>(webView2_)->PostWebMessageAsJson(jsonW.c_str());
    }
}

void SettingsWindow::ApplyConfigFromJson(void* jsonPtr) {
    const json& c = *reinterpret_cast<const json*>(jsonPtr);

    if (c.contains("appearance")) {
        const auto& a = c["appearance"];
        currentConfig_.MutableAppearance().highlightColor =
            a.value("highlight_color", currentConfig_.Appearance().highlightColor);
        currentConfig_.MutableAppearance().normalColor =
            a.value("normal_color", currentConfig_.Appearance().normalColor);
        currentConfig_.MutableAppearance().normalOpacity =
            a.value("normal_opacity", currentConfig_.Appearance().normalOpacity);
        currentConfig_.MutableAppearance().fontFamily =
            a.value("font_family", currentConfig_.Appearance().fontFamily);
        currentConfig_.MutableAppearance().fontSize =
            a.value("font_size", currentConfig_.Appearance().fontSize);
        currentConfig_.MutableAppearance().enableKaraoke =
            a.value("enable_karaoke", currentConfig_.Appearance().enableKaraoke);
        currentConfig_.MutableAppearance().enableTranslation =
            a.value("enable_translation", currentConfig_.Appearance().enableTranslation);
        currentConfig_.MutableAppearance().enableMarquee =
            a.value("enable_marquee", currentConfig_.Appearance().enableMarquee);
        currentConfig_.MutableAppearance().marqueeMode =
            a.value("marquee_mode", currentConfig_.Appearance().marqueeMode);
        currentConfig_.MutableAppearance().marqueeDelayMs =
            a.value("marquee_delay_ms", currentConfig_.Appearance().marqueeDelayMs);
        currentConfig_.MutableAppearance().marqueePauseMs =
            a.value("marquee_pause_ms", currentConfig_.Appearance().marqueePauseMs);
        currentConfig_.MutableAppearance().marqueeSpeedPxPerSec =
            static_cast<float>(a.value("marquee_speed_px_per_sec", static_cast<double>(currentConfig_.Appearance().marqueeSpeedPxPerSec)));
        currentConfig_.MutableAppearance().displayMode =
            a.value("display_mode", currentConfig_.Appearance().displayMode);
        currentConfig_.MutableAppearance().cardFontSizeCurrent =
            a.value("card_font_size_current", currentConfig_.Appearance().cardFontSizeCurrent);
        currentConfig_.MutableAppearance().cardFontSizeNext =
            a.value("card_font_size_next", currentConfig_.Appearance().cardFontSizeNext);
        currentConfig_.MutableAppearance().cardCurrentColor =
            a.value("card_current_color", currentConfig_.Appearance().cardCurrentColor);
        currentConfig_.MutableAppearance().cardNextColor =
            a.value("card_next_color", currentConfig_.Appearance().cardNextColor);
    }
    if (c.contains("position")) {
        const auto& p = c["position"];
        currentConfig_.MutablePosition().offsetX = p.value("offset_x", currentConfig_.Position().offsetX);
        currentConfig_.MutablePosition().offsetY = p.value("offset_y", currentConfig_.Position().offsetY);
        // 仅当 JSON 中明确提供时才覆盖锁定状态，避免每次保存都重置为 false
        if (p.contains("lock_position")) currentConfig_.MutablePosition().lockPosition = p["lock_position"].get<bool>();
        if (p.contains("lock_fully")) currentConfig_.MutablePosition().lockFully = p["lock_fully"].get<bool>();
    }
    if (c.contains("advanced")) {
        const auto& a = c["advanced"];
        currentConfig_.MutableAdvanced().websocketPort =
            a.value("websocket_port", currentConfig_.Advanced().websocketPort);
        currentConfig_.MutableAdvanced().refreshRateHz =
            a.value("refresh_rate_hz", currentConfig_.Advanced().refreshRateHz);
        currentConfig_.MutableAdvanced().debugLog =
            a.value("debug_log", currentConfig_.Advanced().debugLog);
    }
    // 通用开关（需要同步注册表）
    if (c.contains("enabled")) {
        currentConfig_.SetEnabled(c["enabled"].get<bool>());
    }
    if (c.contains("auto_start")) {
        currentConfig_.SetAutoStart(c["auto_start"].get<bool>());
    }
}

void SettingsWindow::SendConfigToWebView(const Config& cfg) {
    if (!webView2_) return;

    json j;
    j["type"] = "initConfig";
    j["config"] = {
        {"enabled", cfg.IsEnabled()},
        {"auto_start", cfg.IsAutoStart()},
        {"appearance", {
            {"font_family", cfg.Appearance().fontFamily},
            {"font_size", cfg.Appearance().fontSize},
            {"normal_color", cfg.Appearance().normalColor},
            {"highlight_color", cfg.Appearance().highlightColor},
            {"normal_opacity", cfg.Appearance().normalOpacity},
            {"enable_karaoke", cfg.Appearance().enableKaraoke},
            {"enable_translation", cfg.Appearance().enableTranslation},
            {"enable_marquee", cfg.Appearance().enableMarquee},
            {"marquee_mode", cfg.Appearance().marqueeMode},
            {"marquee_delay_ms", cfg.Appearance().marqueeDelayMs},
            {"marquee_pause_ms", cfg.Appearance().marqueePauseMs},
            {"marquee_speed_px_per_sec", cfg.Appearance().marqueeSpeedPxPerSec},
            {"display_mode", cfg.Appearance().displayMode},
            {"card_font_size_current", cfg.Appearance().cardFontSizeCurrent},
            {"card_font_size_next", cfg.Appearance().cardFontSizeNext},
            {"card_current_color", cfg.Appearance().cardCurrentColor},
            {"card_next_color", cfg.Appearance().cardNextColor},
        }},
        {"position", {
            {"offset_x", cfg.Position().offsetX},
            {"offset_y", cfg.Position().offsetY},
            {"lock_position", cfg.Position().lockPosition},
            {"lock_fully", cfg.Position().lockFully},
        }},
        {"advanced", {
            {"websocket_port", cfg.Advanced().websocketPort},
            {"refresh_rate_hz", cfg.Advanced().refreshRateHz},
            {"debug_log", cfg.Advanced().debugLog},
        }},
    };

    std::string jsonStr = j.dump();
    std::wstring wStr = Utf8ToWide(jsonStr);
    static_cast<ICoreWebView2*>(webView2_)->PostWebMessageAsJson(wStr.c_str());
    moekoe::Log("[SETTINGS] sent initConfig\n");
}

} // namespace moekoe
