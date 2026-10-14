// SPDX-License-Identifier: MIT
#include "core/text.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace wpm::text {

std::wstring toWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), out.data(), need);
    return out;
}

std::string toUtf8(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(),
                                         static_cast<int>(utf16.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(),
                        static_cast<int>(utf16.size()), out.data(), need, nullptr, nullptr);
    return out;
}

std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string formatDouble(double v, int precision) {
    char buf[64];
    if (precision < 0) {
        std::snprintf(buf, sizeof(buf), "%g", v);
    } else {
        if (precision > 6) precision = 6;
        std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    }
    return buf;
}

size_t utf8Length(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((c & 0x80) == 0x00) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        i += step;
        ++n;
    }
    return n;
}

std::string utf8Truncate(std::string_view s, size_t maxChars) {
    size_t i = 0;
    size_t n = 0;
    while (i < s.size() && n < maxChars) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((c & 0x80) == 0x00) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (i + step > s.size()) break;
        i += step;
        ++n;
    }
    return std::string(s.substr(0, i));
}

}  // namespace wpm::text
