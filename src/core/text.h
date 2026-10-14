// SPDX-License-Identifier: MIT
// 文本工具：UTF-8 ↔ UTF-16 转换与格式化（零第三方依赖，只用 Win32）
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace wpm::text {

// UTF-8 → UTF-16。失败返回空串。
[[nodiscard]] std::wstring toWide(std::string_view utf8);

// UTF-16 → UTF-8。失败返回空串。
[[nodiscard]] std::string toUtf8(std::wstring_view utf16);

// 去除首尾空白（空格、制表、换行、\r）
[[nodiscard]] std::string_view trim(std::string_view s);

// 大小写不敏感比较（ASCII；指标 id 规范为 ASCII）
[[nodiscard]] bool iequals(std::string_view a, std::string_view b);

// 转小写（ASCII）
[[nodiscard]] std::string toLowerAscii(std::string_view s);

// 数值格式化：按 precision 位小数；precision<0 时取最短表示
[[nodiscard]] std::string formatDouble(double v, int precision);

// UTF-8 字符串按"字符"（码点）计数，用于 label 长度校验
[[nodiscard]] size_t utf8Length(std::string_view s);

// 截断到最多 maxChars 个码点（不产生半个码点）
[[nodiscard]] std::string utf8Truncate(std::string_view s, size_t maxChars);

}  // namespace wpm::text
