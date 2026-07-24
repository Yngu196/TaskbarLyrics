// SPDX-License-Identifier: GPL-3.0
// renderer_utils.h - 渲染器内部辅助函数（多翻译单元共享）
#pragma once

#include <string>
#include <windows.h>

namespace moekoe {
namespace renderer_utils {

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len);
    return out;
}

inline wchar_t FirstUtf8CharAsWide(const std::string& s) {
    if (s.empty()) return L'?';
    int len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), 4, nullptr, 0);
    if (len <= 0) return L'?';
    wchar_t ch = L'?';
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), 4, &ch, 1);
    return ch;
}

inline double GetCurrentTimeSeconds() {
    LARGE_INTEGER freq, counter;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
}

} // namespace renderer_utils
} // namespace moekoe
