// SPDX-License-Identifier: GPL-3.0
// tray_commands.cpp - 托盘/歌词窗口菜单命令处理实现
//
// 从 main.cpp 提取，包含 ID_MENU_* 命令的完整分支逻辑。
//
#include "tray_commands.h"

#include "api_enabler.h"
#include "config_dialog.h"
#include "constants.h"
#include "d2d_settings_window.h"
#include "logger.h"

#include <shobjidl.h>
#include <psapi.h>

#include <algorithm>
#include <string>

namespace moekoe {

using namespace moekoe::constants;

// 日志快捷方式（使用统一日志系统）
using moekoe::Log;

// 菜单命令处理
void OnTrayCommand(AppContext& app, UINT menuId) {
    switch (menuId) {
    case ID_MENU_AUTOSTART: {
        const bool newState = !app.config->IsAutoStart();
        const bool regOk = app.config->SetAutoStart(newState);
        app.config->Save();
        if (app.tray) {
            app.tray->SetMenuCheckedAutoStart(newState);
        }
        // 用 MessageBox 直接弹模态对话框反馈结果（气泡通知在 Win10/11 常被禁用）
        const std::wstring title = L"开机自启";
        std::wstring msg;
        if (regOk) {
            if (newState) {
                msg = L"已启用开机自启。\n\n"
                      L"程序会尝试以下三种方式（按顺序，自动跳过失败的）：\n"
                      L"1) 注册表 Run 键（可能被杀毒软件拦截）\n"
                      L"2) 任务计划程序（推荐）\n"
                      L"3) 启动文件夹快捷方式\n\n"
                      L"查看实际生效方式：\n"
                      L"注册表: reg query HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\n"
                      L"任务计划: schtasks /Query /TN MoeKoeTaskbarLyrics_AutoStart\n"
                      L"启动文件夹: %APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
            } else {
                msg = L"已禁用开机自启，所有方式均已清理。";
            }
        } else {
            msg = L"开机自启设置失败！\n\n"
                  L"三种方式都未生效：\n"
                  L"1. 注册表 Run 键（最可能被拦截）\n"
                  L"2. 任务计划程序 schtasks\n"
                  L"3. 启动文件夹 PowerShell 快捷方式\n\n"
                  L"请查看 debug.log 获取详细错误。";
        }
        ::MessageBoxW(app.hwnd, msg.c_str(), title.c_str(),
            regOk ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
        break;
    }
    case ID_MENU_RECONNECT: {
        if (app.wsClient) app.wsClient->RequestReconnect();
        break;
    }
    case ID_MENU_SETTINGS: {
        // D2D 原生自绘设置界面
        if (!app.d2dSettingsWindow) {
            app.d2dSettingsWindow = std::make_unique<D2DSettingsWindow>();
            app.d2dSettingsWindow->OnConfigChanged([&](const Config& cfg) {
                const auto savedPos = app.config->Position();
                *app.config = cfg;
                app.config->MutablePosition() = savedPos;
                ApplyRendererSettings(app);
                if (app.taskbarWindow) {
                    app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                    app.taskbarWindow->Reposition();
                }
                if (app.tray) {
                    app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                }
                Log("[SETTINGS] D2D config applied and saved\n");
            });
        }

        if (app.d2dSettingsWindow->Show(app.hInstance, app.hwnd, *app.config)) {
            break;
        }
        app.d2dSettingsWindow.reset();
        app.d2dSettingsWindow = nullptr;

        // ═══════ 回退：Win32 原生对话框 ═══════
        if (ConfigDialog::Show(app.hInstance, app.hwnd, *app.config,
                               /*boundMode*/ false,
                               [&app]() {
                                   // 同 ID_MENU_EXIT：ConfigDialog 的模态循环
                                   // 嵌套在 TrackPopupMenuEx 内，WM_QUIT 会被吞掉
                                   app.running = false;
                               })) {
            ApplyRendererSettings(app);
            if (app.taskbarWindow) {
                app.taskbarWindow->SetDragOffset(
                    app.config->Position().offsetX, app.config->Position().offsetY);
                app.taskbarWindow->Reposition();
            }
        }
        break;
    }
    case ID_MENU_UNBIND: {
        // 保留兼容：退出程序
        int ret = ::MessageBoxW(app.hwnd,
            L"确定要退出吗？",
            L"退出", MB_YESNO | MB_ICONQUESTION);
        if (ret == IDYES) {
            // 同 ID_MENU_EXIT：当前仍在 TrackPopupMenuEx 模态循环内，
            // PostQuitMessage 发出的 WM_QUIT 会被吞掉，仅设 running=false
            app.running = false;
        }
        break;
    }
    case ID_MENU_LOCK_POS: {
        const bool newState = !app.config->Position().lockPosition;
        app.config->MutablePosition().lockPosition = newState;
        // 完全锁定隐含位置锁定，取消位置锁定时不影响完全锁定
        if (newState) {
            app.config->MutablePosition().lockFully = false;
        }
        app.config->Save();
        if (app.taskbarWindow) {
            app.taskbarWindow->SetPositionLocked(newState);
            app.taskbarWindow->SetFullyLocked(false);
        }
        if (app.tray) {
            app.tray->SetMenuCheckedLockPos(newState);
            app.tray->SetMenuCheckedLockFull(false);
        }
        break;
    }
    case ID_MENU_LOCK_FULL: {
        const bool newState = !app.config->Position().lockFully;
        app.config->MutablePosition().lockFully = newState;
        // 完全锁定隐含位置锁定
        if (newState) {
            app.config->MutablePosition().lockPosition = true;
        }
        app.config->Save();
        if (app.taskbarWindow) {
            app.taskbarWindow->SetFullyLocked(newState);
            app.taskbarWindow->SetPositionLocked(newState);
        }
        if (app.tray) {
            app.tray->SetMenuCheckedLockFull(newState);
            app.tray->SetMenuCheckedLockPos(newState);
        }
        break;
    }
    case ID_MENU_EXIT: {
        // 不能在此调用 PostQuitMessage(0)：当前处在 TrackPopupMenuEx 的
        // 模态消息循环中，WM_QUIT 会被其内部 GetMessage 吞掉，导致回到
        // 主消息循环后 GetMessage 永久阻塞，程序卡死。
        // 仅设置 running=false，由主循环的 while 条件 (app.running && GetMessage)
        // 短路跳出即可。
        app.running = false;
        break;
    }
    case ID_MENU_OPEN_MOEKOE: {
        // 尝试查找并激活 MoeKoeMusic 主窗口
        // MoeKoeMusic 是 Electron 应用，窗口类名为 Chrome_WidgetWin_1
        HWND hMoeKoe = ::FindWindowW(L"Chrome_WidgetWin_1", nullptr);
        if (hMoeKoe) {
            // 检查进程名是否为 MoeKoeMusic
            DWORD pid = 0;
            ::GetWindowThreadProcessId(hMoeKoe, &pid);
            bool isMoeKoe = false;
            HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                wchar_t procName[MAX_PATH] = {};
                if (::GetModuleFileNameExW(hProc, nullptr, procName, MAX_PATH)) {
                    std::wstring name(procName);
                    // 不区分大小写检查进程名是否包含 MoeKoeMusic
                    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                    isMoeKoe = (name.find(L"moekoemusic") != std::wstring::npos);
                }
                ::CloseHandle(hProc);
            }
            if (isMoeKoe) {
                if (::IsIconic(hMoeKoe)) {
                    ::ShowWindow(hMoeKoe, SW_RESTORE);
                }
                ::SetForegroundWindow(hMoeKoe);
            } else {
                // 找到的不是 MoeKoeMusic，继续枚举
                hMoeKoe = nullptr;
                // 使用 EnumWindows 枚举所有 Chrome_WidgetWin_1 窗口
                struct EnumData { HWND result; } data{nullptr};
                ::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                    auto* d = reinterpret_cast<EnumData*>(lParam);
                    wchar_t cls[256] = {};
                    ::GetClassNameW(hwnd, cls, 256);
                    if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0) return TRUE;
                    if (!::IsWindowVisible(hwnd)) return TRUE;
                    DWORD pid2 = 0;
                    ::GetWindowThreadProcessId(hwnd, &pid2);
                    HANDLE hP = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid2);
                    if (hP) {
                        wchar_t pn[MAX_PATH] = {};
                        if (::GetModuleFileNameExW(hP, nullptr, pn, MAX_PATH)) {
                            std::wstring nm(pn);
                            std::transform(nm.begin(), nm.end(), nm.begin(), ::towlower);
                            if (nm.find(L"moekoemusic") != std::wstring::npos) {
                                d->result = hwnd;
                                ::CloseHandle(hP);
                                return FALSE;
                            }
                        }
                        ::CloseHandle(hP);
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&data));
                if (data.result) {
                    if (::IsIconic(data.result)) ::ShowWindow(data.result, SW_RESTORE);
                    ::SetForegroundWindow(data.result);
                } else {
                    Log("[OPEN-MOEKOE] MoeKoeMusic window not found\n");
                }
            }
        } else {
            Log("[OPEN-MOEKOE] MoeKoeMusic window not found\n");
        }
        break;
    }
    case ID_MENU_EXPORT_LOG: {
        // 生成默认文件名：MoeKoeTaskbarLyrics_debug_YYYYMMDD_HHMMSS.log
        SYSTEMTIME st;
        ::GetLocalTime(&st);
        wchar_t defaultName[128];
        _snwprintf_s(defaultName, _TRUNCATE,
            L"MoeKoeTaskbarLyrics_debug_%04d%02d%02d_%02d%02d%02d.log",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        // 获取 debug.log 路径
        std::string logPath = GetLogPath();
        if (logPath.empty()) {
            ::MessageBoxW(app.hwnd,
                L"日志文件路径未知，无法导出。",
                L"导出日志", MB_OK | MB_ICONWARNING);
            break;
        }

        // 检查 debug.log 是否存在
        DWORD attr = ::GetFileAttributesA(logPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            ::MessageBoxW(app.hwnd,
                L"debug.log 文件不存在，可能尚未产生日志。",
                L"导出日志", MB_OK | MB_ICONWARNING);
            break;
        }

        // 使用 IFileSaveDialog 弹出"另存为"对话框
        IFileSaveDialog* pSaveDlg = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_FileSaveDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pSaveDlg));
        if (FAILED(hr) || !pSaveDlg) {
            ::MessageBoxW(app.hwnd,
                L"无法创建文件保存对话框。",
                L"导出日志", MB_OK | MB_ICONERROR);
            break;
        }

        pSaveDlg->SetFileName(defaultName);
        pSaveDlg->SetDefaultExtension(L"log");

        COMDLG_FILTERSPEC rgSpec[] = {
            {L"日志文件 (*.log)", L"*.log"},
            {L"所有文件 (*.*)", L"*.*"}
        };
        pSaveDlg->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
        pSaveDlg->SetFileTypeIndex(0);

        hr = pSaveDlg->Show(app.hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem* pItem = nullptr;
            hr = pSaveDlg->GetResult(&pItem);
            if (SUCCEEDED(hr) && pItem) {
                PWSTR pszPath = nullptr;
                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                if (SUCCEEDED(hr) && pszPath) {
                    // UTF-8 → 宽字符，供 CopyFileW 使用
                    int srcLen = ::MultiByteToWideChar(CP_UTF8, 0,
                        logPath.c_str(), -1, nullptr, 0);
                    std::wstring srcWide(static_cast<size_t>(srcLen), L'\0');
                    ::MultiByteToWideChar(CP_UTF8, 0,
                        logPath.c_str(), -1, &srcWide[0], srcLen);

                    if (::CopyFileW(srcWide.c_str(), pszPath, FALSE)) {
                        std::wstring msg = L"日志已成功导出到：\n";
                        msg += pszPath;
                        ::MessageBoxW(app.hwnd, msg.c_str(),
                            L"导出日志", MB_OK | MB_ICONINFORMATION);
                        Log("[EXPORT] Log exported to: %ls\n", pszPath);
                    } else {
                        wchar_t errBuf[320];
                        _snwprintf_s(errBuf, _TRUNCATE,
                            L"导出失败（错误码：%lu）。\n目标路径：%s",
                            ::GetLastError(), pszPath);
                        ::MessageBoxW(app.hwnd, errBuf,
                            L"导出日志", MB_OK | MB_ICONERROR);
                        Log("[EXPORT] CopyFile failed: error=%lu\n", ::GetLastError());
                    }
                    ::CoTaskMemFree(pszPath);
                }
                pItem->Release();
            }
        }
        // 用户取消无需提示
        pSaveDlg->Release();
        break;
    }
    default:
        break;
    }
}

} // namespace moekoe
