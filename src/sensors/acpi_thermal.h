// SPDX-License-Identifier: MIT
// ACPI 热区域提供程序（免驱、免管理员）。
//
// 数据来源：PDH 计数器集 "Thermal Zone Information"，由 ACPI 驱动
// (ACPI.sys → thermal zone driver) 导出，不需要 Ring0、不需要提权。
// 实测（本机 HP ZHAN66 14 GBA / Ryzen 7 H 255 / Win11 26300，非管理员）：
//   \Thermal Zone Information(\_TZ.CPUZ)\High Precision Temperature
//   空闲 70 °C → 满载 77 °C → 冷却 66 °C，1 Hz 跟随负载，值以 1 °C 为步进。
//
// 该计数器集在 Windows 10 1809+ 均存在，但不是每台机器的每个热区都接了
// 真实传感器：本机 9 个热区里只有 CPUZ 随负载变化，GFXZ 恒定 25.1 °C
// （ACPI 表里存在但底层没接），因此**必须**做"是否随负载变化"的可用性判断，
// 而不是有热区就当温度源。
//
// 口径与陷阱（见 docs/laptop-telemetry-facts.md §T4）：
//   * 原始值是 0.1 K（十分之一开尔文）：°C = v / 10 − 273.15
//   * 2732（= 0.0 °C）与 0 是"无数据"哨兵，不是温度，必须判为不可用
//   * 分辨率约 1 °C，精度与响应速度都不如 MSR/DTS，UI 上要按"估算"呈现
#pragma once

#include "sensors/sensor.h"

#include <string>
#include <vector>

namespace wpm::sensors {

class AcpiThermalProvider final : public ISensorProvider {
public:
    ~AcpiThermalProvider() override;

    [[nodiscard]] const char* name() const noexcept override { return "acpi-thermal"; }
    bool Init(std::string& detail) override;
    void Sample(Snapshot& snap) override;
    [[nodiscard]] const std::vector<size_t>& Slots() const noexcept override { return slots_; }
    [[nodiscard]] bool Available() const noexcept override { return available_; }

    // 供 --probe 使用：本机发现的热区清单（实例名 → 角色）。
    // 只报"分类"，不报"可用"：热区是否真接了传感器要按每一次读数判（哨兵值 / 合理区间），
    // 在枚举阶段无法判定——枚举只做一次 PdhExpandWildCardPathW，不读任何值。
    struct ZoneInfo {
        std::string instance;  // 例如 "_TZ.CPUZ"
        std::string role;      // "cpu" / "gpu" / "other"
    };
    [[nodiscard]] const std::vector<ZoneInfo>& Zones() const noexcept { return zones_; }
    // 诊断用：--probe 需要看到热区清单，因此允许它主动触发一次枚举。
    // 常驻路径（--watch）绝不主动调用——枚举本身要花 4.3 MB。
    void ProbeZones() const { EnsureZones(); }

private:
    // 按需枚举热区。注意：**仅 PdhExpandWildCardPathW 一次调用**就会把性能计数器表
    // 映射进进程（本机实测 +4.3 MB 私有提交，且不归还），所以连"有哪些热区"都必须
    // 推迟到真的要读温度时才知道。const 是因为它只是惰性填充缓存（成员标 mutable）。
    bool EnsureZones() const;
    // 按需建立 PDH 查询（同上，PdhAddEnglishCounterW 也有代价）。
    bool EnsureQuery();

    void* query_ = nullptr;
    void* cpuCounter_ = nullptr;  // \Thermal Zone Information(\_TZ.CPUZ)\...
    void* gpuCounter_ = nullptr;
    // 以下为惰性缓存：只由 EnsureZones() 填充
    mutable std::wstring cpuPath_;  // 待加入查询的 CPU 热区计数器路径
    mutable std::wstring gpuPath_;
    mutable std::string probeNote_;  // 枚举失败/无热区时的原因，写进 Reading.detail
    mutable bool zonesReady_ = false;
    mutable std::vector<ZoneInfo> zones_;
    int cpuSlot_ = -1;
    int gpuSlot_ = -1;
    bool available_ = false;
    bool firstSample_ = true;
    double cpuLast_ = 0.0;  // 上一轮读数，用于"是否在动"判断
    bool cpuMoved_ = false;
    std::vector<size_t> slots_;
};

}  // namespace wpm::sensors
