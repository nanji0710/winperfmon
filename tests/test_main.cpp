// SPDX-License-Identifier: MIT
#include "test.h"

#include <cstdio>
#include <string>

namespace wpmtest {

std::vector<Case>& Registry() {
    static std::vector<Case> reg;
    return reg;
}

Registrar::Registrar(std::string name, std::function<void()> fn) {
    Registry().push_back(Case{std::move(name), std::move(fn)});
}

namespace {
int g_failuresInCase = 0;
bool g_caseFailed = false;
std::string g_firstFailure;
}  // namespace

void ReportFailure(const char* file, int line, const std::string& message) {
    ++g_failuresInCase;
    g_caseFailed = true;
    if (g_firstFailure.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s:%d: ", file, line);
        g_firstFailure = std::string(buf) + message;
    }
}

void ReportPass() {}

int RunAll() {
    int passed = 0;
    int failed = 0;
    for (auto& c : Registry()) {
        g_failuresInCase = 0;
        g_caseFailed = false;
        g_firstFailure.clear();
        c.fn();
        if (g_caseFailed) {
            ++failed;
            std::printf("[FAIL] %s\n       %s\n", c.name.c_str(), g_firstFailure.c_str());
        } else {
            ++passed;
            std::printf("[ ok ] %s\n", c.name.c_str());
        }
    }
    std::printf("\n%d passed, %d failed, %d total\n", passed, failed, passed + failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace wpmtest

int main() { return wpmtest::RunAll(); }
