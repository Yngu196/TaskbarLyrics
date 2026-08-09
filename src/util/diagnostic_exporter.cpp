// SPDX-License-Identifier: GPL-3.0
// diagnostic_exporter.cpp - 兼容性诊断信息收集与导出实现
#include "util/diagnostic_exporter.h"

#include "core/constants.h"
#include "util/environment_check.h"
#include "util/logger.h"
#include "net/websocket_client.h"
#include "taskbar/taskbar_window.h"

#include <dwmapi.h>
#include <dxgi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace moekoe {

namespace {

// 系统架构检测
std::string GetSystemArchitecture() {
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
    case PROCESSOR_ARCHITECTURE_ARM:   return "ARM";
    default: return "Unknown";
    }
}

// DPI 信息（主显示器）
std::string GetDpiInfo() {
    // GetDpiForSystem 需 Win10 1607+
    HMODULE hUser32 = ::GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        using GetDpiForSystemFn = UINT(WINAPI*)();
        auto pGetDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(
            ::GetProcAddress(hUser32, "GetDpiForSystem"));
        if (pGetDpiForSystem) {
            UINT dpi = pGetDpiForSystem();
            int pct = ::MulDiv(100, dpi, 96);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d%% (%u DPI)", pct, dpi);
            return buf;
        }
    }
    // 回退：用 DC 计算
    HDC hdc = ::GetDC(nullptr);
    int dpi = ::GetDeviceCaps(hdc, LOGPIXELSX);
    ::ReleaseDC(nullptr, hdc);
    int pct = ::MulDiv(100, dpi, 96);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d%% (%d DPI)", pct, dpi);
    return buf;
}

// GPU 信息
std::string GetGpuDescription() {
    // 使用 DXGI 获取适配器描述
    HMODULE hDxgi = ::LoadLibraryW(L"dxgi.dll");
    if (!hDxgi) return "(DXGI unavailable)";

    using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    auto pCreateFactory = reinterpret_cast<CreateDXGIFactoryFn>(
        ::GetProcAddress(hDxgi, "CreateDXGIFactory"));
    if (!pCreateFactory) {
        ::FreeLibrary(hDxgi);
        return "(CreateDXGIFactory not found)";
    }

    IDXGIFactory* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) {
        ::FreeLibrary(hDxgi);
        return "(DXGI factory creation failed)";
    }

    IDXGIAdapter* pAdapter = nullptr;
    std::string result;
    hr = pFactory->EnumAdapters(0, &pAdapter);
    if (SUCCEEDED(hr) && pAdapter) {
        DXGI_ADAPTER_DESC desc{};
        pAdapter->GetDesc(&desc);
        // 转换宽字符为 UTF-8
        char utf8Desc[256] = {};
        ::WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                              utf8Desc, sizeof(utf8Desc) - 1, nullptr, nullptr);
        result = utf8Desc;
        pAdapter->Release();
    }

    pFactory->Release();
    ::FreeLibrary(hDxgi);
    return result.empty() ? "(no adapter found)" : result;
}

// 任务栏位置枚举转字符串
const char* TaskbarPositionToString(TaskbarPosition pos) {
    switch (pos) {
    case TaskbarPosition::BOTTOM: return "BOTTOM";
    case TaskbarPosition::TOP:    return "TOP";
    case TaskbarPosition::LEFT:   return "LEFT";
    case TaskbarPosition::RIGHT:  return "RIGHT";
    default:                      return "UNKNOWN";
    }
}

// 配置快照（遮蔽敏感字段）
std::string BuildConfigSnapshot(const Config& config) {
    std::ostringstream ss;
    ss << "=== Appearance ===\n";
    ss << "  highlightColor: " << config.Appearance().highlightColor << "\n";
    ss << "  normalColor: " << config.Appearance().normalColor << "\n";
    ss << "  normalOpacity: " << config.Appearance().normalOpacity << "\n";
    ss << "  fontFamily: " << config.Appearance().fontFamily << "\n";
    ss << "  fontSize: " << config.Appearance().fontSize << "\n";
    ss << "  enableKaraoke: " << (config.Appearance().enableKaraoke ? "true" : "false") << "\n";
    ss << "  enableTranslation: " << (config.Appearance().enableTranslation ? "true" : "false") << "\n";
    ss << "  translationMode: " << config.Appearance().translationMode << "\n";
    ss << "  displayMode: " << config.Appearance().displayMode << "\n";
    ss << "  enableMarquee: " << (config.Appearance().enableMarquee ? "true" : "false") << "\n";
    ss << "  marqueeMode: " << config.Appearance().marqueeMode << "\n";
    ss << "  cardDynamicWidth: " << (config.Appearance().cardDynamicWidth ? "true" : "false") << "\n";
    ss << "  windowWidthOverride: " << config.Appearance().windowWidthOverride << "\n";

    ss << "\n=== Advanced ===\n";
    ss << "  websocketPort: " << config.Advanced().websocketPort << "\n";
    ss << "  httpServerPort: " << config.Advanced().httpServerPort << "\n";
    ss << "  refreshRateHz: " << config.Advanced().refreshRateHz << "\n";
    ss << "  debugLog: " << (config.Advanced().debugLog ? "true" : "false") << "\n";
    ss << "  enableFullscreenHide: " << (config.Advanced().enableFullscreenHide ? "true" : "false") << "\n";

    ss << "\n=== Position ===\n";
    ss << "  offsetX: " << config.Position().offsetX << "\n";
    ss << "  offsetY: " << config.Position().offsetY << "\n";
    ss << "  lockPosition: " << (config.Position().lockPosition ? "true" : "false") << "\n";
    ss << "  lockFully: " << (config.Position().lockFully ? "true" : "false") << "\n";

    ss << "\n=== Other ===\n";
    ss << "  autoStart: " << (config.IsAutoStart() ? "true" : "false") << "\n";
    ss << "  authToken: ***\n";  // 敏感字段遮蔽

    return ss.str();
}

// 读取日志文件尾部（最后 maxLines 行）
std::string ReadLogTail(const std::string& path, int maxLines) {
    if (path.empty()) return "(日志路径未初始化)";

    // 优先尝试 debug.log，若不存在则尝试 debug.1.log（轮转备份）
    std::string tryPath = path;
    std::ifstream f(tryPath, std::ios::binary);
    if (!f.is_open()) {
        // 尝试轮转备份
        size_t dotPos = tryPath.rfind('.');
        if (dotPos != std::string::npos) {
            std::string backup = tryPath;
            backup.insert(dotPos, ".1");
            f.open(backup, std::ios::binary);
        }
    }
    if (!f.is_open()) return "(日志文件不存在)";

    // 读取文件尾部：先定位到文件末尾，向前扫描换行符
    f.seekg(0, std::ios::end);
    const auto fileSize = static_cast<long long>(f.tellg());
    if (fileSize <= 0) return "(日志文件为空)";

    // 从文件末尾向前读取，最多 256KB
    const auto readSize = static_cast<std::streamsize>(
        std::min(static_cast<long long>(256 * 1024), fileSize));
    f.seekg(-readSize, std::ios::end);

    std::string content(readSize, '\0');
    f.read(&content[0], readSize);
    f.close();

    // 统计换行符，截取最后 maxLines 行
    int lineCount = 0;
    auto it = content.end();
    while (it != content.begin()) {
        --it;
        if (*it == '\n') {
            ++lineCount;
            if (lineCount >= maxLines) {
                ++it;  // 跳过当前换行符
                break;
            }
        }
    }
    std::string tail(it, content.end());
    // 去掉末尾可能的空行
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) {
        tail.pop_back();
    }
    return tail;
}

} // namespace

DiagnosticInfo CollectDiagnostics(const AppContext& app) {
    DiagnosticInfo info{};

    // ── Windows 版本 + 环境 ──
    auto envStatus = CheckEnvironment();
    const auto& wv = envStatus.osVersion;

    char verBuf[32];
    std::snprintf(verBuf, sizeof(verBuf), "%d.%d.%d", wv.major, wv.minor, wv.build);
    info.osVersion = verBuf;
    info.osBuild = std::to_string(wv.build);
    info.osProductName = wv.productName;
    info.osDisplayVersion = wv.displayVersion;
    info.osArchitecture = GetSystemArchitecture();

    // ── DPI ──
    info.dpiScaling = GetDpiInfo();

    // ── 任务栏 ──
    if (app.taskbarWindow) {
        info.taskbarFound = true;
        auto tbInfo = app.taskbarWindow->GetTaskbarInfo();
        info.taskbarPosition = TaskbarPositionToString(tbInfo.position);
        info.taskbarAutoHide = tbInfo.autoHide;
        info.taskbarVertical = app.taskbarWindow->IsVerticalTaskbar();
    } else {
        // 窗口未创建时尝试直接查找
        info.taskbarFound = (::FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr);
    }

    // ── DWM ──
    info.dwmCompositionEnabled = envStatus.dwmCompositionEnabled;

    // ── 进程 ──
    info.explorerRunning = envStatus.explorerRunning;

    // ── 第三方 Shell 修改工具 ──
    info.shellModifications = envStatus.shellModifications;

    // ── WebSocket ──
    info.wsConnected = app.wsClient ? app.wsClient->IsConnected() : false;

    // ── GPU ──
    info.gpuDescription = GetGpuDescription();

    // ── 配置快照 ──
    if (app.config) {
        info.configSnapshot = BuildConfigSnapshot(*app.config);
    }

    // ── 版本 ──
    info.pluginVersion = constants::PLUGIN_VERSION;

    // ── 日志路径 ──
    info.logFilePath = GetLogPath();

    // ── 日志尾部 ──
    info.logTail = ReadLogTail(info.logFilePath, 200);

    return info;
}

std::string FormatDiagnosticText(const DiagnosticInfo& info) {
    std::ostringstream ss;

    ss << "╔══════════════════════════════════════════════╗\n";
    ss << "║  MoeKoe Taskbar Lyrics — 诊断信息            ║\n";
    ss << "╚══════════════════════════════════════════════╝\n\n";

    ss << "── 操作系统 ──────────────────────────────────\n";
    ss << "  产品: " << info.osProductName << "\n";
    if (!info.osDisplayVersion.empty()) {
        ss << "  版本: " << info.osDisplayVersion << "\n";
    }
    ss << "  内核: " << info.osVersion << "\n";
    ss << "  构建: " << info.osBuild << "\n";
    ss << "  架构: " << info.osArchitecture << "\n\n";

    ss << "── 显示 ──────────────────────────────────────\n";
    ss << "  DPI: " << info.dpiScaling << "\n";
    ss << "  GPU: " << info.gpuDescription << "\n\n";

    ss << "── 任务栏 ────────────────────────────────────\n";
    ss << "  检测: " << (info.taskbarFound ? "已找到" : "未找到") << "\n";
    ss << "  位置: " << info.taskbarPosition << "\n";
    ss << "  自动隐藏: " << (info.taskbarAutoHide ? "是" : "否") << "\n";
    ss << "  垂直方向: " << (info.taskbarVertical ? "是" : "否") << "\n\n";

    ss << "── 环境依赖 ──────────────────────────────────\n";
    ss << "  DWM 合成: " << (info.dwmCompositionEnabled ? "已启用" : "已禁用") << "\n";
    ss << "  Explorer.exe: " << (info.explorerRunning ? "运行中" : "未运行") << "\n";
    ss << "  Shell 修改工具: ";
    if (info.shellModifications.empty()) {
        ss << "无\n";
    } else {
        for (size_t i = 0; i < info.shellModifications.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << info.shellModifications[i];
        }
        ss << "\n";
    }
    ss << "\n";

    ss << "── 连接 ──────────────────────────────────────\n";
    ss << "  WebSocket: " << (info.wsConnected ? "已连接" : "未连接") << "\n\n";

    ss << "── 插件 ──────────────────────────────────────\n";
    ss << "  版本: " << info.pluginVersion << "\n";
    ss << "  日志: " << info.logFilePath << "\n\n";

    ss << "── 配置快照 ──────────────────────────────────\n";
    ss << info.configSnapshot << "\n";

    ss << "── 版本支持 ──────────────────────────────────\n";
    const int build = std::atoi(info.osBuild.c_str());
    ss << "  最低支持: Build " << MIN_SUPPORTED_BUILD << " (Win10 2004)\n";
    ss << "  推荐版本: Build " << RECOMMENDED_BUILD << " (Win10 22H2)\n";
    ss << "  当前状态: ";
    if (build >= RECOMMENDED_BUILD) {
        ss << "✅ 推荐版本及以上\n";
    } else if (build >= MIN_SUPPORTED_BUILD) {
        ss << "⚠️ 支持但低于推荐版本\n";
    } else {
        ss << "❌ 低于最低支持版本\n";
    }

    // 日志尾部
    if (!info.logTail.empty()) {
        ss << "\n── 日志尾部（最近 200 行）────────────────────\n";
        ss << info.logTail << "\n";
    }

    return ss.str();
}

bool ExportDiagnosticFile(const AppContext& app, HWND ownerWnd) {
    Log("[DIAG] Collecting diagnostics...\n");

    // 收集诊断信息
    DiagnosticInfo info = CollectDiagnostics(app);

    // 格式化为文本
    std::string text = FormatDiagnosticText(info);

    // 生成默认文件名
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t defaultName[128];
    _snwprintf_s(defaultName, _TRUNCATE,
        L"MoeKoeTaskbarLyrics_Diagnostic_%04d%02d%02d_%02d%02d%02d.txt",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // 使用 IFileSaveDialog 选择保存路径
    IFileSaveDialog* pSaveDlg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileSaveDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pSaveDlg));
    if (FAILED(hr) || !pSaveDlg) {
        // 回退：直接写入 EXE 同级目录
        wchar_t exeDir[MAX_PATH] = {0};
        ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        wchar_t* slash = wcsrchr(exeDir, L'\\');
        if (slash) *slash = L'\0';
        std::wstring path = std::wstring(exeDir) + L"\\" + defaultName;

        FILE* f = fopen(
            std::string(path.begin(), path.end()).c_str(), "w");
        if (f) {
            fwrite(text.c_str(), 1, text.size(), f);
            fclose(f);
            std::wstring msg = L"诊断信息已导出到：\n" + path;
            ::MessageBoxW(ownerWnd, msg.c_str(),
                L"导出诊断信息", MB_OK | MB_ICONINFORMATION);
            Log("[DIAG] Exported to: %ls\n", path.c_str());
            return true;
        }
        ::MessageBoxW(ownerWnd,
            L"无法创建文件保存对话框，且直接写入也失败。",
            L"导出诊断信息", MB_OK | MB_ICONERROR);
        return false;
    }

    pSaveDlg->SetFileName(defaultName);
    pSaveDlg->SetDefaultExtension(L"txt");

    COMDLG_FILTERSPEC rgSpec[] = {
        {L"文本文件 (*.txt)", L"*.txt"},
        {L"所有文件 (*.*)", L"*.*"}
    };
    pSaveDlg->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
    pSaveDlg->SetFileTypeIndex(0);

    hr = pSaveDlg->Show(ownerWnd);
    if (SUCCEEDED(hr)) {
        IShellItem* pItem = nullptr;
        hr = pSaveDlg->GetResult(&pItem);
        if (SUCCEEDED(hr) && pItem) {
            PWSTR pszPath = nullptr;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
            if (SUCCEEDED(hr) && pszPath) {
                // 写入文本文件
                int pathLen = ::WideCharToMultiByte(CP_UTF8, 0,
                    pszPath, -1, nullptr, 0, nullptr, nullptr);
                std::string pathUtf8(pathLen, '\0');
                ::WideCharToMultiByte(CP_UTF8, 0,
                    pszPath, -1, &pathUtf8[0], pathLen, nullptr, nullptr);
                // 去掉末尾的 null
                while (!pathUtf8.empty() && pathUtf8.back() == '\0') pathUtf8.pop_back();

                FILE* f = fopen(pathUtf8.c_str(), "wb");
                if (f) {
                    // 写入 UTF-8 BOM
                    unsigned char bom[] = {0xEF, 0xBB, 0xBF};
                    fwrite(bom, 1, 3, f);
                    fwrite(text.c_str(), 1, text.size(), f);
                    fclose(f);

                    std::wstring msg = L"诊断信息已导出到：\n";
                    msg += pszPath;
                    ::MessageBoxW(ownerWnd, msg.c_str(),
                        L"导出诊断信息", MB_OK | MB_ICONINFORMATION);
                    Log("[DIAG] Exported to: %ls\n", pszPath);
                } else {
                    wchar_t errBuf[320];
                    _snwprintf_s(errBuf, _TRUNCATE,
                        L"写入失败（错误码：%lu）。\n目标路径：%s",
                        ::GetLastError(), pszPath);
                    ::MessageBoxW(ownerWnd, errBuf,
                        L"导出诊断信息", MB_OK | MB_ICONERROR);
                    Log("[DIAG] Write failed: error=%lu\n", ::GetLastError());
                }
                ::CoTaskMemFree(pszPath);
            }
            pItem->Release();
        }
    }
    // 用户取消无需提示
    pSaveDlg->Release();
    return SUCCEEDED(hr);
}

} // namespace moekoe
