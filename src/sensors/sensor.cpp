// SPDX-License-Identifier: MIT
#include "sensors/sensor.h"

#include "config/catalog.h"

#include <mutex>

namespace wpm::sensors {

struct SnapshotPublisher::Impl {
    std::vector<std::shared_ptr<Snapshot>> buffers;
    std::atomic<int> writableSlot{0};
    std::atomic<uint64_t> seq{0};
    mutable std::mutex publishMu;  // 只保护指针交换，不在读热路径上
    std::shared_ptr<const Snapshot> latest;
};

SnapshotPublisher::SnapshotPublisher() : impl_(std::make_unique<Impl>()) {
    const size_t n = config::MetricCatalog().size();
    auto a = std::make_shared<Snapshot>();
    auto b = std::make_shared<Snapshot>();
    a->metrics.resize(n);
    b->metrics.resize(n);
    impl_->buffers = {std::move(a), std::move(b)};
    impl_->latest = impl_->buffers[0];
}

SnapshotPublisher::~SnapshotPublisher() = default;

Snapshot& SnapshotPublisher::Writable() {
    const int idx = impl_->writableSlot.load(std::memory_order_acquire);
    Snapshot& s = *impl_->buffers[static_cast<size_t>(idx)];
    s.seq = impl_->seq.load(std::memory_order_relaxed) + 1;
    s.tick += 1;
    // 清掉逐次采集的附加数组；指标槽位由各 provider 覆盖或显式置 Stale
    s.perCoreFreqMHz.clear();
    s.perCoreDetail.clear();
    return s;
}

void SnapshotPublisher::Publish() {
    std::lock_guard<std::mutex> lk(impl_->publishMu);
    const int idx = impl_->writableSlot.load(std::memory_order_relaxed);
    auto published = impl_->buffers[static_cast<size_t>(idx)];
    // 另一块变成可写缓冲：先把上次的值搬过去，让 Sample() 只需覆盖自己负责的槽位
    const int other = 1 - idx;
    auto& next = impl_->buffers[static_cast<size_t>(other)];
    const Snapshot& src = *published;
    next->seq = src.seq;
    next->tick = src.tick;
    next->metrics = src.metrics;
    next->perCoreFreqMHz = src.perCoreFreqMHz;
    next->perCoreDetail = src.perCoreDetail;
    next->baseClockMHz = src.baseClockMHz;
    next->logicalCores = src.logicalCores;
    next->physicalCores = src.physicalCores;
    next->cpuBrand = src.cpuBrand;
    next->gpuName = src.gpuName;
    next->gpuSelectedIndex = src.gpuSelectedIndex;
    next->gpuAdapterCount = src.gpuAdapterCount;

    impl_->seq.store(src.seq, std::memory_order_release);
    impl_->latest = published;
    impl_->writableSlot.store(other, std::memory_order_release);
}

std::shared_ptr<const Snapshot> SnapshotPublisher::Latest() const {
    std::lock_guard<std::mutex> lk(impl_->publishMu);
    return impl_->latest;
}

}  // namespace wpm::sensors
