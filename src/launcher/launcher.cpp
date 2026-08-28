// SPDX-License-Identifier: GPL-3.0
// launcher.cpp - 双架构（x64 / ARM64）架构感知启动器
//
// MoeKoeMusic 通过 native host 启动插件根目录的 MoeKoeTaskbarLyrics.exe。
// 本启动器【固定编译为 x64】：x64 程序可在 ARM64 Windows 上经 x64 仿真运行，
// 而 ARM64 程序无法在 x64 上运行，因此启动器不可能是 ARM64。
//
// 启动时检测系统【原生】架构，拉起对应架构子目录下的真实程序：
//   <插件根>/x64/MoeKoeTaskbarLyrics.exe    (x64 系统)
//   <插件根>/arm64/MoeKoeTaskbarLyrics.exe  (ARM64 系统)
//
// 关键点：
//   1. 启动器继承自身的 stdin/stdout（MoeKoeMusic 的 native messaging 管道），
//      通过 CreateProcess(bInheritHandles=TRUE) 透传给子进程。
//   2. 启动器【等待】子进程退出后再退出，使 MoeKoeMusic 观察到的进程生命周期
//      与真实程序完全一致（避免"MoeKoeMusic 探测到进程退出即断开"的边界情况）。
//   3. 启动器不读写管道，stdio 的读写完全由子进程负责，无竞争。
#include <windows.h>

#include <string>

namespace {

// 检测系统原生架构（返回 IMAGE_FILE_MACHINE_* 常量）。
// 优先级：IsWow64Process2（Win10 1709+，返回原生机器类型）→ GetNativeSystemInfo。
DWORD DetectNativeMachine() {
    USHORT processMachine = 0;
    USHORT nativeMachine = 0;
    if (::IsWow64Process2(::GetCurrentProcess(), &processMachine, &nativeMachine)) {
        if (nativeMachine != IMAGE_FILE_MACHINE_UNKNOWN) {
            return nativeMachine;
        }
    }

    // 回退路径（旧系统 / IsWow64Process2 不可用）
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_ARM64: return IMAGE_FILE_MACHINE_ARM64;
        case PROCESSOR_ARCHITECTURE_AMD64: return IMAGE_FILE_MACHINE_AMD64;
        case PROCESSOR_ARCHITECTURE_ARM:   return IMAGE_FILE_MACHINE_ARMNT;
        case PROCESSOR_ARCHITECTURE_INTEL: return IMAGE_FILE_MACHINE_I386;
        default:                           return IMAGE_FILE_MACHINE_AMD64;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // 1. 定位自身所在目录（即插件根目录）
    wchar_t self[MAX_PATH] = {0};
    const DWORD selfLen = ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (selfLen == 0 || selfLen >= MAX_PATH) {
        return 2;
    }
    std::wstring rootDir(self);
    const size_t slash = rootDir.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return 2;
    }
    rootDir = rootDir.substr(0, slash);

    // 2. 按系统原生架构选择子目录
    const DWORD machine = DetectNativeMachine();
    const wchar_t* subdir = (machine == IMAGE_FILE_MACHINE_ARM64) ? L"arm64" : L"x64";

    // 3. 组装真实程序路径；目标架构子程序缺失时【直接明确报错】，不回退尝试另一架构。
    //    跨架构 fallback 会掩盖打包错误（如 ARM64 包缺少 arm64/ 时静默运行 x64 子程序，
    //    导致 ARM64 原生版本是否真正安装/生效无从判断），故必须失败显式化。
    const std::wstring childPath = rootDir + L"\\" + subdir + L"\\MoeKoeTaskbarLyrics.exe";
    if (::GetFileAttributesW(childPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const wchar_t* archName = (machine == IMAGE_FILE_MACHINE_ARM64) ? L"ARM64" : L"x64";
        const std::wstring msg =
            L"未找到 " + std::wstring(archName) + L" 架构的 MoeKoeTaskbarLyrics 程序文件：\n" +
            childPath + L"\n\n请重新安装插件。";
        ::MessageBoxW(nullptr, msg.c_str(), L"MoeKoe Taskbar Lyrics", MB_OK | MB_ICONERROR);
        return 3;
    }

    // 4. 继承 stdio 启动子进程（native messaging 管道透传）
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = ::GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    // lpApplicationName 显式指定子程序全路径；lpCommandLine 透传原命令行
    // （CreateProcess 以 lpApplicationName 为准，命令行开头的模块名会被忽略）。
    const BOOL ok = ::CreateProcessW(
        childPath.c_str(),
        ::GetCommandLineW(),
        nullptr, nullptr,
        TRUE,   // bInheritHandles：子进程继承 stdio 句柄
        0,      // 无特殊创建标志
        nullptr,
        nullptr,
        &si, &pi);

    if (!ok) {
        ::MessageBoxW(nullptr, L"启动 MoeKoeTaskbarLyrics 失败。",
                      L"MoeKoe Taskbar Lyrics", MB_OK | MB_ICONERROR);
        return 4;
    }

    // 5. 等待子进程退出，保持生命周期与 MoeKoeMusic 预期一致
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return 0;
}
