// SPDX-License-Identifier: MIT
#include "fps/framestats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wpm::fps {
namespace {

/// 有效帧的判据：> 0 且 <= 上限。0 或负数说明这一行不是一帧（PresentMon 在切换进程时会写 0），
/// 超过上限（默认 1 s）说明这中间发生了暂停/切桌面/断电源，属于「不是这一帧慢，而是没在采」。
bool IsPlausible(double ms, double maxMs) { return ms > 0.0 && ms <= maxMs; }

/// 把**已升序排列**的帧时间填成统计结果。两条 Low 口径都在这里，别在别处重算。
void FillFromSorted(std::vector<double>& sorted, const FrameStatsOptions& opt, FrameStats* st) {
    const size_t n = sorted.size();
    if (n == 0) return;
    double sum = 0.0;
    for (double v : sorted) sum += v;

    st->frames = n;
    const double nd = static_cast<double>(n);
    st->frameTimeAvgMs = sum / nd;
    st->frameTimeMedianMs = (n % 2 == 1) ? sorted[n / 2]
                                         : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
    st->frameTimeWorstMs = sorted[n - 1];
    st->avgFps = nd * 1000.0 / sum;

    // 减 1e-9 是必要的：0.01 在二进制里不精确，1000 × 0.01 会算出 10.000000000000002，
    // 不减这一下 ceil 就变成 11，1% Low 会凭空多算一帧（正好是 33.33 ms 那种帧）。
    size_t k = static_cast<size_t>(std::ceil(static_cast<double>(n) * opt.lowPercentile - 1e-9));
    if (k < 1) k = 1;
    if (k > n) k = n;

    // 口径 1（本项目，dev-plan §3.4）：最差 k 帧的帧时间均值取倒数
    double worstSum = 0.0;
    for (size_t i = n - k; i < n; ++i) worstSum += sorted[i];
    st->lowFps = worstSum > 0.0 ? 1000.0 / (worstSum / static_cast<double>(k)) : 0.0;

    // 口径 2（CapFrameX）：阈值 = 第 k 大的帧时间，取所有 >= 阈值的帧求均值再取倒数。
    // 与口径 1 在无并列时相等；有并列时集合更大 —— 两个都给出，差异本身就是信息。
    const double threshold = sorted[n - k];
    double qSum = 0.0;
    size_t qN = 0;
    for (size_t i = 0; i < n; ++i) {
        if (sorted[i] >= threshold) {
            qSum += sorted[i];
            ++qN;
        }
    }
    st->lowFpsQuantile = (qN > 0 && qSum > 0.0) ? 1000.0 / (qSum / static_cast<double>(qN)) : 0.0;

    // 卡顿：基线（均值或中位）× factor 为阈值，超过的帧占比。无魔数。
    st->stutterBaselineMs = (opt.baseline == wpm::config::StutterBaseline::Median)
                                ? st->frameTimeMedianMs
                                : st->frameTimeAvgMs;
    const double limit = st->stutterBaselineMs * opt.stutterFactor;
    uint64_t stutters = 0;
    for (size_t i = 0; i < n; ++i) {
        if (sorted[i] > limit) ++stutters;
    }
    st->stutterFrames = stutters;
    st->stutterPct = 100.0 * static_cast<double>(stutters) / static_cast<double>(n);
}

}  // namespace

FrameRing::FrameRing(size_t capacity) : capacity_(capacity > 0 ? capacity : 1) {
    frameMs_.assign(capacity_, 0.0f);
    modeId_.assign(capacity_, 0);
    modes_.reserve(8);
    scratch_.reserve(capacity_);
}

void FrameRing::Reset() {
    head_ = 0;
    count_ = 0;
    rejected_ = 0;
    dropped_ = 0;
    modes_.clear();
    std::fill(modeId_.begin(), modeId_.end(), static_cast<uint8_t>(0));
}

void FrameRing::EnsureBusyArrays() {
    if (hasBusy_) return;
    cpuBusyMs_.assign(capacity_, -1.0f);
    gpuBusyMs_.assign(capacity_, -1.0f);
    hasBusy_ = true;
}

uint8_t FrameRing::InternMode(std::string_view mode) {
    for (size_t i = 0; i < modes_.size(); ++i) {
        if (modes_[i].name == mode) return static_cast<uint8_t>(i);
    }
    if (modes_.size() >= 255) return 255;  // 其它
    modes_.push_back(ModeEntry{std::string(mode)});
    return static_cast<uint8_t>(modes_.size() - 1);
}

size_t FrameRing::RingIndex(size_t i) const noexcept {
    const size_t start = (head_ + capacity_ - count_) % capacity_;
    return (start + i) % capacity_;
}

void FrameRing::Add(const FrameSample& s) {
    FrameStatsOptions def{};
    if (!s.valid || !IsPlausible(s.frameTimeMs, def.maxPlausibleFrameTimeMs)) {
        ++rejected_;
        return;
    }
    if (s.dropped) {
        // 被显示管线丢掉的帧从没上屏：计入 dropped 计数，但不进 FPS 统计。
        // 它占用的时间仍然体现在下一帧更大的 MsBetweenPresents 里，所以不会凭空多出帧。
        ++dropped_;
        return;
    }
    if (s.cpuBusyMs >= 0.0 || s.gpuBusyMs >= 0.0) EnsureBusyArrays();

    frameMs_[head_] = static_cast<float>(s.frameTimeMs);
    if (hasBusy_) {
        cpuBusyMs_[head_] = s.cpuBusyMs >= 0.0 ? static_cast<float>(s.cpuBusyMs) : -1.0f;
        gpuBusyMs_[head_] = s.gpuBusyMs >= 0.0 ? static_cast<float>(s.gpuBusyMs) : -1.0f;
    }
    modeId_[head_] = s.presentMode.empty() ? 0 : InternMode(s.presentMode);

    head_ = (head_ + 1) % capacity_;
    if (count_ < capacity_) ++count_;
}

float FrameRing::FrameTimeAt(size_t i) const noexcept {
    if (i >= count_) return 0.0f;
    return frameMs_[RingIndex(i)];
}

FrameStats FrameRing::Compute(const FrameStatsOptions& opt) const {
    FrameStats st;
    st.rejected = rejected_;
    st.dropped = dropped_;
    if (count_ == 0) return st;

    scratch_.clear();
    double cpuSum = 0.0, gpuSum = 0.0;
    uint64_t cpuN = 0, gpuN = 0;
    uint32_t modeCounts[256] = {0};
    for (size_t i = 0; i < count_; ++i) {
        const size_t idx = RingIndex(i);
        scratch_.push_back(static_cast<double>(frameMs_[idx]));
        if (hasBusy_) {
            const double c = cpuBusyMs_[idx];
            const double g = gpuBusyMs_[idx];
            if (c >= 0.0) { cpuSum += c; ++cpuN; }
            if (g >= 0.0) { gpuSum += g; ++gpuN; }
        }
        ++modeCounts[modeId_[idx]];
    }
    std::sort(scratch_.begin(), scratch_.end());
    FillFromSorted(scratch_, opt, &st);

    if (cpuN > 0) st.cpuBusyAvgMs = cpuSum / static_cast<double>(cpuN);
    if (gpuN > 0) st.gpuBusyAvgMs = gpuSum / static_cast<double>(gpuN);

    uint32_t best = 0;
    size_t bestId = 0;
    for (size_t id = 0; id < 256; ++id) {
        if (modeCounts[id] > best) {
            best = modeCounts[id];
            bestId = id;
        }
    }
    if (best > 0) {
        st.presentModeCount = best;
        if (bestId == 255 || bestId >= modes_.size()) {
            st.presentMode = "其它";
        } else {
            st.presentMode = modes_[bestId].name;
        }
    }
    return st;
}

FrameStats ComputeFromFrameTimes(const std::vector<double>& frameTimesMs,
                                 const FrameStatsOptions& opt) {
    FrameStats st;
    std::vector<double> sorted;
    sorted.reserve(frameTimesMs.size());
    for (double ms : frameTimesMs) {
        if (!IsPlausible(ms, opt.maxPlausibleFrameTimeMs)) {
            ++st.rejected;
            continue;
        }
        sorted.push_back(ms);
    }
    std::sort(sorted.begin(), sorted.end());
    FillFromSorted(sorted, opt, &st);
    return st;
}

}  // namespace wpm::fps
