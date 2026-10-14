// SPDX-License-Identifier: MIT
// CPU 提供程序（免驱通道）：PDH 占用率与有效频率。
//
// 主方案 §3.1 / §6：
//   - 免驱频率 = "\Processor Information(_Total)\% Processor Performance" × 基频
//     该计数器在 AMD 上同样可用（本机实测 96~104% 随负载真实跟踪），
//     与 Intel 的 APERF/MPERF 差分同源。
//   - 基频优先取注册表 HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0 "~MHz"，
//     该值即 BCLK × 倍频（本机 3793 MHz）。
//   - Ring0 到位后（M1）每核频率改走 MSR/PM 表，本提供程序降级为兜底。
//
// 注意：MinGW 下必须显式 #include <pdh.h>，<windows.h> 不会带入 PDH 声明。
#pragma once

#include "sensors/sensor.h"
#include <string>

namespace wpm::sensors {

class CpuPdhProvider final : public ISensorProvider {
public:
    ~CpuPdhProvider() override;
    [[nodiscard]] const char* name() const noexcept override { return "cpu.pdh"; }
    bool Init(std::string& detail) override;
    void Sample(Snapshot& snap) override;
    [[nodiscard]] const std::vector<size_t>& Slots() const noexcept override { return slots_; }
    [[nodiscard]] bool Available() const noexcept override { return available_; }

private:
    void* query_ = nullptr;        // PDH_HQUERY（避免头文件泄漏到调用方）
    void* perfCounter_ = nullptr;  // PDH_HCOUNTER：% Processor Performance
    void* utilCounter_ = nullptr;  // PDH_HCOUNTER：% Processor Utility（可选）
    void* timeCounter_ = nullptr;  // PDH_HCOUNTER：% Processor Time（兜底）
    bool available_ = false;
    bool haveUtil_ = false;
    bool firstSample_ = true;
    double baseClockMHz_ = 0.0;
    std::string cpuBrand_;
    std::vector<size_t> slots_;
};

}  // namespace wpm::sensors
