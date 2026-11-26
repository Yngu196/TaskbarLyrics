// SPDX-License-Identifier: Apache-2.0
// string_utils.h - UTF-8 与宽字符转换的唯一实现
#pragma once

#include <windows.h>

#include <string>

namespace moekoe {

// UTF-8 → 宽字符。按字节长度转换，保留内嵌 NUL。
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int len = ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len);
    return out;
}

// 宽字符 → UTF-8。使用 c_str() 的终止符语义，返回串不含结尾 NUL。
inline std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int n = ::WideCharToMultiByte(
        CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

// UTF-8 → 宽字符，限制输出宽字符数（Windows Tooltip 上限 127）。
inline std::wstring Utf8ToWideTruncated(const std::string& s, size_t maxChars) {
    std::wstring out = Utf8ToWide(s);
    if (out.size() > maxChars) out.resize(maxChars);
    return out;
}

} // namespace moekoe
