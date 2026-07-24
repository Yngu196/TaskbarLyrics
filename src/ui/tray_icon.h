// SPDX-License-Identifier: GPL-3.0
// tray_icon.h - 系统托盘图标
//
// 职责:
//   - 注册 Shell_NotifyIconW 托盘图标
//   - 提供右键菜单(启用/禁用、开机自启、重新连接、退出)
//   - 更新 Tooltip / 菜单勾选状态
//
#pragma once

#include <windows.h>

#include "core/constants.h"

#include <functional>
#include <string>

namespace moekoe {

// 菜单命令 ID
enum TrayMenuId : UINT {
    ID_TRAY_MENU_BASE = 1000,
    ID_MENU_ENABLE    = ID_TRAY_MENU_BASE + 1,
    ID_MENU_AUTOSTART = ID_TRAY_MENU_BASE + 2,
    ID_MENU_RECONNECT = ID_TRAY_MENU_BASE + 3,
    ID_MENU_EXIT      = ID_TRAY_MENU_BASE + 4,
    ID_MENU_SETTINGS  = ID_TRAY_MENU_BASE + 5,
    ID_MENU_UNBIND    = ID_TRAY_MENU_BASE + 6,
    ID_MENU_LOCK_POS  = ID_TRAY_MENU_BASE + 7,
    ID_MENU_LOCK_FULL = ID_TRAY_MENU_BASE + 8,
    ID_MENU_OPEN_MOEKOE       = ID_TRAY_MENU_BASE + 9,
    ID_MENU_TOGGLE_TRANSLATION = ID_TRAY_MENU_BASE + 10,
    ID_MENU_TRANSLATION_MODE   = ID_TRAY_MENU_BASE + 11, // 子菜单基址
    ID_MENU_CARD_TRANS_MODE   = ID_TRAY_MENU_BASE + 12,  // 卡片翻译模式：原/译 或 原-译
    ID_MENU_EXPORT_LOG        = ID_TRAY_MENU_BASE + 14,  // 导出日志文件
};

class TrayIcon {
public:
    using MenuCallback = std::function<void(UINT menuId)>;

    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // 初始化并显示托盘图标
    bool Initialize(HINSTANCE hInstance, HWND messageWnd = nullptr,
                    UINT callbackMsg = WM_TRAY_CALLBACK);

    // 反初始化
    void Shutdown();

    // 设置右键菜单命令回调
    void SetMenuCallback(MenuCallback cb) { menuCallback_ = std::move(cb); }

    // 更新 Tooltip(显示当前歌词)
    void SetTooltip(const std::wstring& text);

    // 更新菜单勾选状态
    void SetMenuCheckedAutoStart(bool checked);
    void SetMenuCheckedLockPos(bool checked);
    void SetMenuCheckedLockFull(bool checked);
    void SetMenuLabelEnable(const std::wstring& label);

    // 显示气泡通知（用于自启状态变更等反馈）
    void ShowBalloon(const std::wstring& title, const std::wstring& msg);

    // 设置绑定模式（影响是否显示"解除绑定"菜单项）
    void SetBoundMode(bool bound) { boundMode_ = bound; if (hMenu_) RebuildMenu(); }

    // 设置显示模式和卡片翻译模式（影响右键菜单翻译选项）
    void SetDisplayMode(const std::string& mode) { displayMode_ = mode; if (hMenu_) RebuildMenu(); }
    void SetCardTranslationMode(const std::string& mode) { cardTranslationMode_ = mode; if (hMenu_) RebuildMenu(); }

    // 弹出右键菜单
    void ShowContextMenu(HWND hwnd);

    // 消息循环: 处理托盘回调(由主循环调用)
    void OnTrayMessage(HWND hwnd, WPARAM wParam, LPARAM lParam);

    // 加载图标(优先从 resources/icon.ico)
    HICON LoadAppIcon();

private:
    void RebuildMenu();
    void DestroyMenu();

    // 状态
    HINSTANCE    hInstance_{nullptr};
    HWND         messageWnd_{nullptr};
    UINT         callbackMsg_{0};
    HMENU        hMenu_{nullptr};
    NOTIFYICONDATAW nid_{};
    bool         added_{false};

    bool checkedAutoStart_{true};
    bool checkedLockPos_{false};
    bool checkedLockFull_{false};
    bool boundMode_{false};
    std::string displayMode_{"karaoke"};
    std::string cardTranslationMode_{"dual"};
    std::wstring labelEnable_{L"启用歌词显示"};
    std::wstring lastTooltip_;  // 缓存上一次 tooltip，避免每帧 NIM_MODIFY 导致闪烁

    MenuCallback menuCallback_;
};

} // namespace moekoe
