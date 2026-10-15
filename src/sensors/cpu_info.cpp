// SPDX-License-Identifier: MIT
#include "sensors/cpu_info.h"

#include "core/text.h"

#include <windows.h>

#include <vector>

namespace wpm::sensors {
namespace {

constexpr wchar_t kCpuKey[] = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";

}  // namespace

double ReadBaseClockMHz() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCpuKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0.0;
    }
    DWORD mhz = 0;
    DWORD size = sizeof(mhz);
    DWORD type = 0;
    const LONG rc =
        RegQueryValueExW(key, L"~MHz", nullptr, &type, reinterpret_cast<LPBYTE>(&mhz), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return 0.0;
    return static_cast<double>(mhz);
}

std::string ReadCpuBrand() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCpuKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buf[128]{};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
    // 注册表里的品牌串常带尾随空格（本机为 "...w/ Radeon 780M  "）
    return std::string(text::trim(text::toUtf8(buf)));
}

unsigned ReadLogicalCoreCount() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

unsigned ReadPhysicalCoreCount() {
    // 用老版 GetLogicalProcessorInformation：Win10 1809+ 仍然有效，且比 Ex 版少一层
    // 结构体解析。代价是超过 64 个逻辑处理器时它会失败 —— 那种机器不是本项目的目标。
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len == 0) return 0;
    std::vector<unsigned char> buf(len);
    if (!GetLogicalProcessorInformation(
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(buf.data()), &len)) {
        return 0;
    }
    const size_t count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    unsigned cores = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto* p =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(buf.data()) + i;
        if (p->Relationship == RelationProcessorCore) ++cores;
    }
    return cores;
}

}  // namespace wpm::sensors
