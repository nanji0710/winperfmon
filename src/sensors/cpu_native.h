// SPDX-License-Identifier: MIT
// CPU 占用率（原生通道）：NtQuerySystemInformation(SystemProcessorPerformanceInformation)。
//
// 为什么走原生而不是 PDH：本机实测 PdhAddEnglishCounterW 会把计数器表映射进进程，
// 一次 +4.3 MB 私有提交、约 +4.6 MB 工作集，且 PdhCloseQuery 不归还；
// 原生通道实测内存增量 ≈0、单步耗时 ≈0.06 ms（PDH 0.36~0.72 ms）。
// 见 docs/winperfmon-dev-plan.md 的"冷启动与常驻内存"一节。
//
// 口径（重要，必须显式标注）：原生读数是"核心时间占比" busy/(busy+idle)，不加权频率；
// Windows 任务管理器显示的是 % Processor Utility（频率加权）。本项目在能拿到可信当前
// 频率时按 raw × 当前频率 / 基频 折算，使两者可比，并把本轮口径写进 Reading::detail；
// 频率缺失或不可信时退回未加权值并如实标注，绝不静默混用来源。
#pragma once

#include "sensors/sensor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wpm::sensors {

/// 槽位下标无效值（与 adl2.cpp 内部约定一致）。
inline constexpr size_t kNoSlotIndex = static_cast<size_t>(-1);

/// 由两次相邻采样的每核累计计数（100 ns 单位）算核心时间占比（0..100）。
/// busy 计数定义为 KernelTime + UserTime − IdleTime：Windows 把空闲时间也计进内核时间。
/// 计数器回绕、重复读取、时钟未前进等无有效增量时返回 -1（调用方据此判为读不到）。
double BusyPercentFromDelta(uint64_t idle0, uint64_t busy0, uint64_t idle1, uint64_t busy1);

class CpuNativeProvider final : public ISensorProvider {
public:
    CpuNativeProvider();
    ~CpuNativeProvider() override;

    [[nodiscard]] const char* name() const noexcept override { return "cpu.native"; }
    bool Init(std::string& detail) override;
    void Sample(Snapshot& snap) override;
    [[nodiscard]] const std::vector<size_t>& Slots() const noexcept override { return slots_; }
    [[nodiscard]] bool Available() const noexcept override { return available_; }

    /// 最近一轮的原生核心时间占比（未加权），供 --probe 与单测观察。
    [[nodiscard]] double RawUsagePercent() const noexcept { return rawUsage_; }

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
    std::vector<size_t> slots_;
    size_t usageSlot_ = kNoSlotIndex;
    size_t freqSlot_ = kNoSlotIndex;
    std::string cpuBrand_;
    double baseClockMHz_ = 0.0;
    unsigned physicalCores_ = 0;
    double rawUsage_ = 0.0;
    bool firstSample_ = true;
    bool available_ = false;
};

}  // namespace wpm::sensors
