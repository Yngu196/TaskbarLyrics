// SPDX-License-Identifier: GPL-2.0
// tray_icon.cpp - 系统托盘图标实现
#include "tray_icon.h"

#include <shellapi.h>
#include <windows.h>

#include <cstring>
#include <utility>

#pragma comment(lib, "shell32.lib")

namespace moekoe {

namespace {

constexpr UINT kTrayIconId = 1;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len);
    return out;
}

} // namespace

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() {
    Shutdown();
}

HICON TrayIcon::LoadAppIcon() {
    // 1) 尝试 EXE 所在目录的 resources/icon.ico
    wchar_t exeDir[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';
    std::wstring iconPath = std::wstring(exeDir) + L"\\resources\\icon.ico";

    HICON hIcon = static_cast<HICON>(::LoadImageW(
        nullptr, iconPath.c_str(), IMAGE_ICON,
        16, 16, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    if (hIcon) return hIcon;

    // 2) 回退到系统默认应用图标
    hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    return hIcon;
}

bool TrayIcon::Initialize(HINSTANCE hInstance, HWND messageWnd, UINT callbackMsg) {
    hInstance_   = hInstance;
    messageWnd_  = messageWnd;
    callbackMsg_ = callbackMsg ? callbackMsg : (WM_USER + 0x200);

    RebuildMenu();

    HICON hIcon = LoadAppIcon();

    std::memset(&nid_, 0, sizeof(nid_));
    nid_.cbSize           = sizeof(nid_);
    nid_.hWnd             = messageWnd_;
    nid_.uID              = kTrayIconId;
    nid_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = callbackMsg_;
    nid_.hIcon            = hIcon ? hIcon : ::LoadIconW(nullptr, IDI_APPLICATION);
    const wchar_t* tip    = L"MoeKoe Taskbar Lyrics";
    std::wcsncpy(nid_.szTip, tip, ARRAYSIZE(nid_.szTip));
    nid_.szTip[ARRAYSIZE(nid_.szTip) - 1] = L'\0';

    added_ = ::Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    if (!added_) {
        // 尝试再次,使用 NIM_SETVERSION
        nid_.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &nid_);
        added_ = ::Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    }
    return added_;
}

void TrayIcon::Shutdown() {
    if (added_) {
        ::Shell_NotifyIconW(NIM_DELETE, &nid_);
        added_ = false;
    }
    DestroyMenu();
}

void TrayIcon::RebuildMenu() {
    DestroyMenu();
    hMenu_ = ::CreatePopupMenu();
    if (!hMenu_) return;

    // 当前歌词占位(灰显)
    ::AppendMenuW(hMenu_, MF_STRING | MF_GRAYED,
                  ID_TRAY_MENU_BASE, L"当前歌曲");

    ::AppendMenuW(hMenu_, MF_SEPARATOR, 0, nullptr);

    // 启用/禁用歌词
    UINT enableFlags = MF_STRING | (checkedEnable_ ? MF_CHECKED : MF_UNCHECKED);
    ::AppendMenuW(hMenu_, enableFlags, ID_MENU_ENABLE, labelEnable_.c_str());

    // 开机自启
    UINT autoFlags = MF_STRING | (checkedAutoStart_ ? MF_CHECKED : MF_UNCHECKED);
    ::AppendMenuW(hMenu_, autoFlags, ID_MENU_AUTOSTART, L"开机自动启动");

    ::AppendMenuW(hMenu_, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(hMenu_, MF_STRING, ID_MENU_RECONNECT, L"重新连接");

    ::AppendMenuW(hMenu_, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(hMenu_, MF_STRING, ID_MENU_EXIT, L"退出");
}

void TrayIcon::DestroyMenu() {
    if (hMenu_) {
        ::DestroyMenu(hMenu_);
        hMenu_ = nullptr;
    }
}

void TrayIcon::SetTooltip(const std::wstring& text) {
    if (!added_) return;
    std::wcsncpy(nid_.szTip, text.c_str(), ARRAYSIZE(nid_.szTip));
    nid_.szTip[ARRAYSIZE(nid_.szTip) - 1] = L'\0';
    nid_.uFlags = NIF_TIP;
    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::SetMenuCheckedEnable(bool checked) {
    if (checkedEnable_ == checked) return;
    checkedEnable_ = checked;
    if (hMenu_) RebuildMenu();
}

void TrayIcon::SetMenuCheckedAutoStart(bool checked) {
    if (checkedAutoStart_ == checked) return;
    checkedAutoStart_ = checked;
    if (hMenu_) RebuildMenu();
}

void TrayIcon::SetMenuLabelEnable(const std::wstring& label) {
    labelEnable_ = label;
    if (hMenu_) RebuildMenu();
}

void TrayIcon::ShowContextMenu(HWND hwnd) {
    if (!hMenu_) return;
    RebuildMenu();

    POINT pt{};
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(hwnd);
    ::TrackPopupMenuEx(
        hMenu_,
        TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_VERNEGANIMATION,
        pt.x, pt.y, hwnd, nullptr);
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
}

void TrayIcon::OnTrayMessage(HWND hwnd, WPARAM, LPARAM lParam) {
    const UINT msg = LOWORD(lParam);
    switch (msg) {
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowContextMenu(hwnd);
        break;
    case WM_LBUTTONDBLCLK:
        // 双击: 重新连接
        if (menuCallback_) menuCallback_(ID_MENU_RECONNECT);
        break;
    default:
        break;
    }
}

} // namespace moekoe
