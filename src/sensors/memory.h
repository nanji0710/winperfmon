// SPDX-License-Identifier: MIT
// 内存提供程序：免驱可用（GlobalMemoryStatusEx）。
// 负责指标槽位：mem.usage / mem.used
#pragma once

#include "sensors/sensor.h"

namespace wpm::sensors {

class MemoryProvider final : public ISensorProvider {
public:
    [[nodiscard]] const char* name() const noexcept override { return "memory"; }
    bool Init(std::string& detail) override;
    void Sample(Snapshot& snap) override;
    [[nodiscard]] const std::vector<size_t>& Slots() const noexcept override { return slots_; }
    [[nodiscard]] bool Available() const noexcept override { return available_; }

private:
    bool available_ = false;
    std::vector<size_t> slots_;
};

}  // namespace wpm::sensors
