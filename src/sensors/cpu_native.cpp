// SPDX-License-Identifier: MIT
#include "sensors/cpu_native.h"

#include "config/catalog.h"
#include "sensors/cpu_info.h"

#include <windows.h>

#include <string>
#include <vector>

namespace wpm::sensors {
namespace {

// SystemProcessorPerformanceInformation —— 每核累计计数器（100 ns 单位）。
constexpr int kSystemProcessorPerformanceInformation = 8;

struct ProcessorPerfInfo {
    int64_t idleTime;
    int64_t kernelTime;
    int64_t userTime;
    int64_t dpcTime;
    int64_t interruptTime;
    uint32_t interruptCount;
};
static_assert(sizeof(ProcessorPerfInfo) == 48,
              "SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION 在 x64 上应为 48 字节");

int64_t BusyOf(const ProcessorPerfInfo& p) {
    // Windows 把空闲时间也计进 KernelTime，故 busy = kernel + user − idle
    const int64_t busy = p.kernelTime + p.userTime - p.idleTime;
    return busy > 0 ? busy : 0;
}

}  // namespace

double BusyPercentFromDelta(uint64_t idle0, uint64_t busy0, uint64_t idle1, uint64_t busy1) {
    if (idle1 < idle0 || busy1 < busy0) return -1.0;  // 回绕/重复读取
    const uint64_t dIdle = idle1 - idle0;
    const uint64_t dBusy = busy1 - busy0;
    const uint64_t total = dIdle + dBusy;
    if (total == 0) return -1.0;  // 两次采样之间时钟没走
    const double pct = 100.0 * static_cast<double>(dBusy) / static_cast<double>(total);
    return pct < 0.0 ? 0.0 : (pct > 100.0 ? 100.0 : pct);
}

struct CpuNativeProvider::Impl {
    using QueryFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

    QueryFn query = nullptr;
    std::vector<unsigned char> buffer;
    std::vector<uint64_t> prevIdle;
    std::vector<uint64_t> prevBusy;
    unsigned coreCount = 0;
};

CpuNativeProvider::CpuNativeProvider() = default;
CpuNativeProvider::~CpuNativeProvider() = default;

bool CpuNativeProvider::Init(std::string& detail) {
    for (size_t i = 0; i < config::MetricCatalog().size(); ++i) {
        const auto id = config::MetricCatalog()[i].id;
        if (id == "cpu.usage") usageSlot_ = i;
        else if (id == "cpu.freq.avg") freqSlot_ = i;
    }
    if (usageSlot_ == kNoSlotIndex) {
        detail = "指标目录里没有 cpu.usage";
        return false;
    }
    slots_.push_back(usageSlot_);

    baseClockMHz_ = ReadBaseClockMHz();
    cpuBrand_ = ReadCpuBrand();

    impl_ = std::make_unique<Impl>();
    // NtQuerySystemInformation 没有导入符号，运行时从 ntdll 取地址
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        // 先经 void* 中转：直接在两种函数指针类型间 reinterpret_cast 会触发
        // -Wcast-function-type（GetProcAddress 返回的 FARPROC 无参数信息）
        impl_->query = reinterpret_cast<Impl::QueryFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
    }
    if (impl_->query == nullptr) {
        detail = "无法从 ntdll 取到 NtQuerySystemInformation";
        impl_.reset();
        return false;
    }

    const unsigned cores = ReadLogicalCoreCount();
    if (cores == 0) {
        detail = "逻辑核数为 0";
        impl_.reset();
        return false;
    }
    impl_->coreCount = cores;
    impl_->buffer.resize(sizeof(ProcessorPerfInfo) * cores);
    impl_->prevIdle.assign(cores, 0);
    impl_->prevBusy.assign(cores, 0);

    // 试调一次：确认缓冲区够大且接口可用（失败即整条通道不可用）
    ULONG need = 0;
    const LONG st = impl_->query(kSystemProcessorPerformanceInformation, impl_->buffer.data(),
                                 static_cast<ULONG>(impl_->buffer.size()), &need);
    if (st < 0) {
        detail = "NtQuerySystemInformation(SystemProcessorPerformanceInformation) 返回 0x" +
                 std::to_string(static_cast<unsigned long>(st));
        impl_.reset();
        return false;
    }

    available_ = true;
    physicalCores_ = ReadPhysicalCoreCount();
    detail = std::to_string(cores) + " 逻辑核 / " + std::to_string(physicalCores_) +
             " 物理核，基频 " + std::to_string(static_cast<int>(baseClockMHz_ + 0.5)) +
             " MHz（系统调用采样，不加载性能计数器表）";
    return true;
}

void CpuNativeProvider::Sample(Snapshot& snap) {
    if (!available_ || !impl_) return;
    snap.baseClockMHz = baseClockMHz_;
    snap.cpuBrand = cpuBrand_;
    snap.logicalCores = impl_->coreCount;
    snap.physicalCores = physicalCores_;

    const ULONG bytes = static_cast<ULONG>(impl_->buffer.size());
    ULONG need = 0;
    LONG st = impl_->query(kSystemProcessorPerformanceInformation, impl_->buffer.data(), bytes,
                           &need);
    if (st < 0 && need > bytes) {  // 核数在运行中变化（罕见）：按需扩容后重试一次
        impl_->buffer.resize(need);
        st = impl_->query(kSystemProcessorPerformanceInformation, impl_->buffer.data(),
                          static_cast<ULONG>(impl_->buffer.size()), &need);
    }

    Reading& r = snap.metrics[usageSlot_];
    if (st < 0) {
        r.status = Status::Error;
        r.detail = "NtQuerySystemInformation 采样失败";
        return;
    }

    const auto* info = reinterpret_cast<const ProcessorPerfInfo*>(impl_->buffer.data());
    double raw = 0.0;
    bool measured = false;
    if (!firstSample_) {
        uint64_t dIdleSum = 0;
        uint64_t dBusySum = 0;
        for (unsigned i = 0; i < impl_->coreCount; ++i) {
            const uint64_t idle = static_cast<uint64_t>(info[i].idleTime);
            const uint64_t busy = static_cast<uint64_t>(BusyOf(info[i]));
            if (idle >= impl_->prevIdle[i] && busy >= impl_->prevBusy[i]) {
                dIdleSum += idle - impl_->prevIdle[i];
                dBusySum += busy - impl_->prevBusy[i];
            }
            impl_->prevIdle[i] = idle;
            impl_->prevBusy[i] = busy;
        }
        const uint64_t total = dIdleSum + dBusySum;
        if (total > 0) {
            raw = 100.0 * static_cast<double>(dBusySum) / static_cast<double>(total);
            measured = true;
        }
    } else {
        for (unsigned i = 0; i < impl_->coreCount; ++i) {
            impl_->prevIdle[i] = static_cast<uint64_t>(info[i].idleTime);
            impl_->prevBusy[i] = static_cast<uint64_t>(BusyOf(info[i]));
        }
        firstSample_ = false;
        r.status = Status::Stale;
        r.detail = "等待首次采样窗口";
        return;
    }

    if (!measured) {
        r.status = Status::Error;
        r.detail = "两次采样之间无有效增量";
        return;
    }
    rawUsage_ = raw;

    // 频率加权：仅在能拿到本轮可信频率时生效，否则如实退回未加权值
    double usage = raw;
    std::string how = "核心时间占比（未加权）";
    if (freqSlot_ != kNoSlotIndex && baseClockMHz_ > 0.0) {
        const Reading& fr = snap.metrics[freqSlot_];
        if (fr.status == Status::Ok && fr.value >= baseClockMHz_ * 0.5 &&
            fr.value <= baseClockMHz_ * 2.0) {
            usage = raw * (fr.value / baseClockMHz_);
            // 与任务管理器的 % Processor Utility 同思路，但只用一个"全核平均频率"因子，
            // 而 Utility 是逐核按各自频率加权 → 部分核心满载时本值偏低几个百分点
            // （本机实测 8/16 线程满载：本值 56–57%，PDH Utility 59–62%）。
            how = "核心时间占比 × 当前频率/基频（近似任务管理器口径：用全核平均频率加权）";
        } else if (fr.status == Status::Ok) {
            how = "核心时间占比（未加权：当前频率不可信，已忽略）";
        } else {
            how = "核心时间占比（未加权：本轮无频率来源）";
        }
    }
    if (usage > 100.0) usage = 100.0;
    r.value = usage;
    r.status = Status::Ok;
    r.detail = how;
}

}  // namespace wpm::sensors
