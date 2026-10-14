// SPDX-License-Identifier: MIT
// 极简单测框架（零第三方依赖，release-github.md §6 单测范围）。
// 不用 gtest：仓库红线是零第三方依赖，且只需断言与计数。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace wpmtest {

struct Case {
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& Registry();

struct Registrar {
    Registrar(std::string name, std::function<void()> fn);
};

// 抛出式失败（框架内部把失败计数交回给 main）
void ReportFailure(const char* file, int line, const std::string& message);
void ReportPass();

int RunAll();

}  // namespace wpmtest

#define WPM_TEST(test_name)                                              \
    static void test_name();                                             \
    static ::wpmtest::Registrar wpm_reg_##test_name(#test_name, test_name); \
    static void test_name()

#define WPM_CHECK(cond)                                                          \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ::wpmtest::ReportFailure(__FILE__, __LINE__, "断言失败: " #cond);    \
            return;                                                              \
        }                                                                        \
    } while (false)

#define WPM_CHECK_EQ(actual, expected)                                            \
    do {                                                                          \
        const auto wpm_a_ = (actual);                                              \
        const auto wpm_e_ = (expected);                                            \
        if (!(wpm_a_ == wpm_e_)) {                                                 \
            ::wpmtest::ReportFailure(__FILE__, __LINE__,                           \
                                     std::string("不相等: " #actual " != " #expected)); \
            return;                                                               \
        }                                                                         \
    } while (false)

#define WPM_CHECK_NEAR(actual, expected, eps)                                      \
    do {                                                                           \
        const double wpm_a_ = static_cast<double>(actual);                          \
        const double wpm_e_ = static_cast<double>(expected);                        \
        const double wpm_d_ = wpm_a_ > wpm_e_ ? wpm_a_ - wpm_e_ : wpm_e_ - wpm_a_;  \
        if (!(wpm_d_ <= (eps))) {                                                   \
            ::wpmtest::ReportFailure(__FILE__, __LINE__,                            \
                                     std::string("超出容差: " #actual " vs " #expected)); \
            return;                                                                 \
        }                                                                           \
    } while (false)
