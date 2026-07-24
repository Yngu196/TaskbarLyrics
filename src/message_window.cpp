// SPDX-License-Identifier: GPL-3.0
// message_window.cpp - 隐式消息窗口实现
//
// 从 main.cpp 提取：
//   - MsgWndProc（瘦分发器，委托给 HandleFrameTick / HandleRenderUpdate / OnTrayCommand）
//   - RegisterMessageClass
//
#include "message_window.h"

#include "app_context.h"
#include "constants.h"
#include "taskbar_window.h"
#include "tray_commands.h"
#include "tray_icon.h"

namespace moekoe {

// 隐式消息窗口过程(用于接收托盘消息 + WM_FRAME_TICK)
LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppContext* app = reinterpret_cast<AppContext*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CLOSE:
        app->running = false;
        ::PostQuitMessage(0);
        return 0;

    case WM_TRAY_CALLBACK: { // 托盘回调
        if (app->tray) app->tray->OnTrayMessage(hwnd, wParam, lParam);
        return 0;
    }

    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        // 翻译模式子菜单命令（根据显示模式区分处理）
        if (id >= ID_MENU_TRANSLATION_MODE && id <= ID_MENU_TRANSLATION_MODE + 2) {
            const int idx = static_cast<int>(id - ID_MENU_TRANSLATION_MODE);
            const auto& dispMode = app->config->Appearance().displayMode;
            if (dispMode == "card") {
                // 卡片模式：ID+0=原(off), ID+1=译(replace), ID+2=原-译(dual)
                const char* modes[] = {"off", "replace", "dual"};
                app->config->MutableAppearance().cardTranslationMode = modes[idx];
                app->config->MutableAppearance().enableTranslation = (idx != 0);
            } else {
                // 卡拉OK模式：ID+0=原(off), ID+1=译(replace)
                const char* modes[] = {"off", "replace"};
                app->config->MutableAppearance().translationMode = modes[idx];
                app->config->MutableAppearance().enableTranslation = (idx == 1);
            }
            app->config->Save();
            ApplyRendererSettings(*app);
            // 刷新托盘菜单以反映翻译模式变更
            app->tray->SetDisplayMode(dispMode);
            app->tray->SetCardTranslationMode(app->config->Appearance().cardTranslationMode);
            return 0;
        }
        OnTrayCommand(*app, id);
        return 0;
    }

    case TaskbarWindow::WM_FRAME_TICK:
    case WM_TIMER: {
        HandleFrameTick(*app);
        return 0;
    }

    case WM_RENDER_UPDATE: {
        HandleRenderUpdate(*app);
        return 0;
    }

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 注册一个不显示的 message-only 类
bool RegisterMessageClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &MsgWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"MoeKoeTaskbarLyricsMsg";
    return ::RegisterClassExW(&wc) != 0;
}

} // namespace moekoe
