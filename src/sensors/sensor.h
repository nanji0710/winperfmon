// SPDX-License-Identifier: MIT
// 传感器抽象层（主方案 §2 ISensorProvider）。
//
// M0 只实现"免驱"通道：PDH（CPU/GPU 占用率与频率）+ GlobalMemoryStatusEx + DXGI（适配器枚举）。
// Ring0（PawnIO）与厂商 GPU 库（NVML/ADLX/IGCL）在 M1 接入同一接口。
//
// 热路径约定（config-schema.md §0 不变量 5）：
//   采集线程独占生产 Snapshot，UI 线程只读已发布快照；
//   发布走双缓冲 + atomic 序号，读侧无锁。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wpm::sensors {

// 指标不可用的原因分类 —— UI 直接据此给"为什么读不到"的提示
enum class Status : uint8_t {
    Ok,             // 有有效读数
    Unavailable,    // 本机硬件不提供
    NeedDriver,     // 需要 Ring0 通道（PawnIO 未安装/已禁用）
    NeedGpuLib,     // 需要厂商 GPU 库（NVML/ADLX/IGCL）
    NeedPresentMon, // 需要 PresentMon
    Error,          // 采样出错（错误码见 detail）
    Stale,          // 本次未更新（保留上次值）
};

struct Reading {
    double value = 0.0;
    Status status = Status::Unavailable;
    std::string detail;  // 人类可读的原因/错误（可为空）

    [[nodiscard]] bool ok() const noexcept { return status == Status::Ok; }
};

// 一帧完整读数。长度恒等于 config::MetricCatalog() 的长度，下标一一对应。
struct Snapshot {
    uint64_t seq = 0;       // 单调递增发布序号
    uint64_t tick = 0;      // 第几次采集
    int64_t takenAtMs = 0;  // 自进程启动的毫秒数（steady_clock）
    int64_t utcMs = 0;      // 当前 UTC 毫秒（供 sys.time 用）

    std::vector<Reading> metrics;

    // 诊断/展示用的附加信息（不属于 metrics 指标目录）
    std::vector<double> perCoreFreqMHz;   // cpu.freq.percore
    std::vector<std::string> perCoreDetail;
    double baseClockMHz = 0.0;            // BCLK × 倍频 或注册表 ~MHz
    unsigned logicalCores = 0;
    unsigned physicalCores = 0;
    std::string cpuBrand;
    std::string gpuName;                  // gpu.selectedIndex 选中的适配器
    int gpuSelectedIndex = -1;
    int gpuAdapterCount = 0;

    [[nodiscard]] const Reading& at(size_t slot) const {
        static const Reading kEmpty{};
        return slot < metrics.size() ? metrics[slot] : kEmpty;
    }
};

// 采集器基类。所有实现都必须可重复调用 Sample()，且不得抛异常。
class ISensorProvider {
public:
    virtual ~ISensorProvider() = default;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // 初始化（打开句柄/加载库）；失败返回 false 并把原因写入 detail
    virtual bool Init(std::string& detail) = 0;
    // 采样一次，写入自身负责的指标槽位
    virtual void Sample(Snapshot& snap) = 0;
    // 本提供程序负责的指标槽位（下标）
    [[nodiscard]] virtual const std::vector<size_t>& Slots() const noexcept = 0;
    [[nodiscard]] virtual bool Available() const noexcept = 0;
};

// ── 双缓冲快照发布器：采集线程写、UI 线程读，无锁 ────────────────────────
class SnapshotPublisher {
public:
    SnapshotPublisher();
    ~SnapshotPublisher();
    SnapshotPublisher(const SnapshotPublisher&) = delete;
    SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;

    // 采集线程：取一个可写缓冲，写完调用 Publish()
    [[nodiscard]] Snapshot& Writable();
    void Publish();

    // 任意线程：取最近一次已发布快照（shared_ptr 保证生命周期）
    [[nodiscard]] std::shared_ptr<const Snapshot> Latest() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wpm::sensors
