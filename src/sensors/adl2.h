// SPDX-License-Identifier: MIT
// AMD 免驱遥测：运行时动态加载系统自带的 atiadlxx.dll（ADL2）。
//
// 为什么它是 M1 的 AMD 主路径：
//   * 完全用户态厂商 API —— 不注入、不挂钩、不自研内核驱动（铁律①②③）
//   * **不需要管理员、不需要 PawnIO**（本机实测：非管理员即可读 PMLog）
//   * 不随包分发任何 DLL/头文件/导入库，只 LoadLibraryW + GetProcAddress
//     （铁律④ 无运行时依赖）；DLL 缺失时优雅降级，绝不让进程起不来
//   * 一条 PMLog 调用同时给出 GPU 频率/占用/温度/功耗 与 **CPU 温度/功耗/频率**
//     （AMD APU 上 CPU 与 GPU 同 die，PM 表把两者都报了出来）
//
// 本机实测口径（HP ZHAN66 14 G8a / Ryzen 7 H 255 / Radeon 780M / Win11 26300）：
//   * `ADL2_Main_Control_Create` 的**第一个参数必须是 malloc 回调**；
//     传 nullptr 得到 rc=-11（ADL_ERR_INVALID_RESOURCE）
//   * ADL 对同一物理 GPU 会报多条适配器（本机 5 条），差别只在 PNP 末尾的
//     `&02`..`&05` 后缀 → 去重键 = 去掉末尾 `&NN` 后的前缀
//   * `AdapterInfo.iVendorID` 是 AMD 的 bug（本机报 0x03EA），必须从
//     `strPNPString`/`strUDID` 解析 `VEN_`/`DEV_` 才能与 DXGI 的 VendorId/DeviceId 对齐
//   * 传感器支持性只认 `ADL2_New_QueryPMLogData_Get` 每项的 `supported` 字段；
//     `ADL2_Adapter_PMLog_Support_Get` 在本机返回 rc=0 但内容与 PMLog 支持列表不一致
//     （报 1..9），**已弃用**
//   * 需先置 `iSize` 的结构：`AdapterInfo.iSize`、`ADLPMLogDataOutput.iSize`
//
// 与 ACPI 热区的优先级：本 provider 的读数（die 级、0.1 °C、秒级响应）优先于
// `AcpiThermalProvider` 的热区读数（1 °C、滞后 ~10 s）——见 Collector 的 provider 顺序。
#pragma once

#include "sensors/gpu_enum.h"
#include "sensors/sensor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wpm::sensors {

// 去重后的 ADL 适配器（= 一个物理 GPU）
struct AdlAdapter {
    int adlIndex = -1;  // 传给 ADL2_*_Get 的 iAdapterIndex
    int bus = 0;
    int device = 0;
    int function = 0;
    uint32_t vendorId = 0;  // 从 PNP/UDID 解析（iVendorID 不可信）
    uint32_t deviceId = 0;
    std::string name;
    std::string pnp;
    int matchedGpuIndex = -1;  // 对应的 DXGI 枚举下标
};

class Adl2Provider final : public ISensorProvider {
public:
    Adl2Provider();
    ~Adl2Provider() override;
    Adl2Provider(const Adl2Provider&) = delete;
    Adl2Provider& operator=(const Adl2Provider&) = delete;

    [[nodiscard]] const char* name() const noexcept override { return "adl2"; }

    // Collector 先做 DXGI 枚举与 gpu.selectedIndex 解析，再把结果交给本 provider：
    // 两处各自枚举会导致口径漂移，而 gpu.selectedIndex 的语义只能有一个实现。
    void SetGpuContext(const std::vector<GpuAdapter>& adapters, int resolvedIndex);

    bool Init(std::string& detail) override;
    void Sample(Snapshot& snap) override;
    [[nodiscard]] const std::vector<size_t>& Slots() const noexcept override { return slots_; }
    // 只有"库已加载 + 上下文建立 + 成功绑定到选定适配器"才算可用
    [[nodiscard]] bool Available() const noexcept override { return available_; }

    // ── 诊断接口（--probe），不参与采样 ──────────────────────────────────────
    [[nodiscard]] bool LibraryLoaded() const noexcept { return libLoaded_; }
    [[nodiscard]] const std::string& LibraryPath() const noexcept { return libPath_; }
    [[nodiscard]] const std::vector<AdlAdapter>& RawAdapters() const noexcept { return raw_; }
    [[nodiscard]] const std::vector<AdlAdapter>& DedupAdapters() const noexcept { return dedup_; }
    [[nodiscard]] int BoundAdlIndex() const noexcept { return bound_; }
    [[nodiscard]] const std::vector<int>& Supported() const noexcept { return supported_; }
    // 为什么选中/没选中某个适配器（含 gpu.selectedIndex 的回退理由）
    [[nodiscard]] const std::string& SelectionNote() const noexcept { return selNote_; }
    [[nodiscard]] const std::string& InitDetail() const noexcept { return initDetail_; }
    // 读一次当前 PMLog 原始值（诊断用；返回 false 表示调用失败）
    bool PeekPMLog(std::vector<std::pair<int, int>>& out) const;
    // 本后端是否真的能提供该指标（有绑定 + 驱动在该适配器上报告了对应传感器）。
    // Collector 用它决定"要不要再创建 PDH 兜底提供程序"。
    [[nodiscard]] bool Claims(std::string_view metricId) const;

private:
    struct Bind {
        int sens = 0;         // ADL PMLog 传感器索引
        size_t slot = 0;      // 指标在 Snapshot.metrics 中的下标
        double lo = 0.0;      // 合理区间下界（反伪造判据）
        double hi = 0.0;      // 合理区间上界
        std::string note;     // 写进 Reading.detail 的来源说明
        bool zeroIsMissing = false;  // 0 视为"无数据"（RPM 类）
    };

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::vector<GpuAdapter> gpu_;
    int resolved_ = -1;

    std::vector<size_t> slots_;
    std::vector<Bind> binds_;

    std::vector<AdlAdapter> raw_;
    std::vector<AdlAdapter> dedup_;
    std::vector<int> supported_;
    std::string libPath_;
    std::string selNote_;
    std::string initDetail_;
    bool libLoaded_ = false;
    bool available_ = false;
    bool integrated_ = false;
    int bound_ = -1;

    // 绑定一个传感器到指标槽位
    void BindSensor(int sens, const char* metricId, double lo, double hi, std::string note,
                    bool zeroIsMissing = false);
    // 认领一个"本后端给不出值"的指标，并写明原因：
    // 避免它落到通用的"需要厂商 GPU 库"提示上（那会在库明明可用时自相矛盾）
    void ClaimFixed(const char* metricId, std::string why);
    void MarkUnavailable(Snapshot& snap, const std::string& reason) const;
};

}  // namespace wpm::sensors
