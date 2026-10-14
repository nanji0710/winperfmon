// SPDX-License-Identifier: MIT
#include "sensors/cpu_pdh.h"

#include "config/catalog.h"
#include "core/text.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <string>
#include <vector>

namespace wpm::sensors {
namespace {

constexpr wchar_t kPerfPath[] =
    L"\\Processor Information(_Total)\\% Processor Performance";
constexpr wchar_t kUtilPath[] =
    L"\\Processor Information(_Total)\\% Processor Utility";
constexpr wchar_t kTimePath[] = L"\\Processor Information(_Total)\\% Processor Time";

double ReadRegistryBaseClockMHz() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) {
        return 0.0;
    }
    DWORD mhz = 0;
    DWORD size = sizeof(mhz);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, L"~MHz", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&mhz), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return 0.0;
    return static_cast<double>(mhz);
}

std::string ReadCpuBrand() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buf[128]{};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
    // 注册表里的品牌串常带尾随空格（本机 "AMD Ryzen 7 H 255 w/ Radeon 780M  "）
    return wpm::text::toUtf8(buf);
}

bool AddCounter(void* query, const wchar_t* path, void** out) {
    PDH_HCOUNTER h = nullptr;
    if (PdhAddEnglishCounterW(static_cast<PDH_HQUERY>(query), path, 0, &h) != ERROR_SUCCESS) {
        return false;
    }
    *out = h;
    return true;
}

// 读取一个已收集的计数器；返回 false 表示本次无有效值（首帧、计数器未就绪）
bool ReadCounter(void* counter, double& out) {
    PDH_FMT_COUNTERVALUE v{};
    const PDH_STATUS rc =
        PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(counter), PDH_FMT_DOUBLE, nullptr,
                                    &v);
    if (rc != ERROR_SUCCESS) return false;
    if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return false;
    out = v.doubleValue;
    return true;
}

}  // namespace

CpuPdhProvider::~CpuPdhProvider() {
    if (query_) PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
}

bool CpuPdhProvider::Init(std::string& detail) {
    for (size_t i = 0; i < config::MetricCatalog().size(); ++i) {
        const auto id = config::MetricCatalog()[i].id;
        if (id == "cpu.usage" || id == "cpu.freq.avg") slots_.push_back(i);
    }

    baseClockMHz_ = ReadRegistryBaseClockMHz();
    cpuBrand_ = ReadCpuBrand();

    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) {
        detail = "PdhOpenQuery 失败";
        return false;
    }
    query_ = q;

    // 基频拿不到也能给占用率，故频率计数器失败不算致命
    if (!AddCounter(query_, kPerfPath, &perfCounter_)) perfCounter_ = nullptr;
    if (!AddCounter(query_, kUtilPath, &utilCounter_)) utilCounter_ = nullptr;
    else haveUtil_ = true;
    if (!AddCounter(query_, kTimePath, &timeCounter_)) timeCounter_ = nullptr;

    if (!perfCounter_ && !timeCounter_) {
        PdhCloseQuery(q);
        query_ = nullptr;
        detail = "PDH 计数器不可用（Processor Information 对象缺失，可能被本地化策略禁用）";
        return false;
    }

    // 首次收集仅建立基线，值不可用
    PdhCollectQueryData(q);
    available_ = true;

    if (baseClockMHz_ <= 0.0) {
        detail = "PDH 就绪，但无法从注册表读取基频，cpu.freq.avg 将不可用";
    }
    return true;
}

void CpuPdhProvider::Sample(Snapshot& snap) {
    if (!available_) return;
    // 诊断字段：snapshot 里的基频与品牌串供 --probe 与 UI 使用
    snap.baseClockMHz = baseClockMHz_;
    snap.cpuBrand = cpuBrand_;
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    snap.logicalCores = si.dwNumberOfProcessors;
    PDH_HQUERY q = static_cast<PDH_HQUERY>(query_);
    PdhCollectQueryData(q);

    if (firstSample_) {
        // 第一帧只有基线，标记为暂不可用，下一帧起给出真实值
        firstSample_ = false;
        for (size_t s : slots_) {
            Reading& r = snap.metrics[s];
            r.status = Status::Stale;
            r.detail = "等待首次采样窗口";
        }
        return;
    }

    double usage = 0.0;
    bool haveUsage = false;
    if (haveUtil_ && ReadCounter(utilCounter_, usage)) {
        haveUsage = true;
    } else if (timeCounter_ && ReadCounter(timeCounter_, usage)) {
        haveUsage = true;
    }

    double perf = 0.0;
    const bool havePerf = perfCounter_ && ReadCounter(perfCounter_, perf);

    for (size_t s : slots_) {
        const auto id = config::MetricCatalog()[s].id;
        Reading& r = snap.metrics[s];
        if (id == "cpu.usage") {
            if (haveUsage) {
                r.value = usage > 100.0 ? 100.0 : (usage < 0.0 ? 0.0 : usage);
                r.status = Status::Ok;
                r.detail.clear();
            } else {
                r.status = Status::Error;
                r.detail = "PDH 取值失败";
            }
        } else if (id == "cpu.freq.avg") {
            if (havePerf && baseClockMHz_ > 0.0) {
                r.value = baseClockMHz_ * perf / 100.0;
                r.status = Status::Ok;
                r.detail.clear();
            } else if (baseClockMHz_ <= 0.0) {
                r.status = Status::Unavailable;
                r.detail = "基频未知（注册表 ~MHz 读取失败）";
            } else {
                r.status = Status::Error;
                r.detail = "PDH % Processor Performance 取值失败";
            }
        }
    }
    (void)snap;
}

}  // namespace wpm::sensors
