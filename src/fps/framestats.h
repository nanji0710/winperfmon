// SPDX-License-Identifier: MIT
// 帧统计：口径全部按 docs/winperfmon-dev-plan.md §3.4，纯逻辑、可单测、零动态分配（热路径）。
//
// 三条容易搞错的口径，这里钉死：
//   1) 平均 FPS 是**调和平均**：n × 1000 / Σ帧时间。不是「先求帧时间算术平均再取倒数」，
//      更不是「各帧瞬时 FPS 求算术平均」（后者会被单帧尖峰抬高，见单测 ring_stats_*）。
//   2) 1% Low 用**帧时间的降序尾部**：取最差 k = ceil(n × p) 帧的帧时间均值 T，报 1000 / T。
//      CapFrameX 的分位阈值口径（帧时间 ≥ 第 k 大的所有帧取均值）同时算，两个值都给出：
//      无并列时两者相等，有并列时后者集合更大 —— 差异本身就是诊断信息，不藏。
//   3) 卡顿率没有魔数：stutter% = 100 × count(帧时间 > factor × 基线) / n，基线取均值或中位。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"

namespace wpm::fps {

/// 一帧的原始数据（只留算指标真正需要的列）。
struct FrameSample {
    double frameTimeMs = 0.0;  ///< PresentMon `MsBetweenPresents`
    double cpuBusyMs = -1.0;   ///< `MsCPUBusy`；<0 = 本行没有该列
    double gpuBusyMs = -1.0;   ///< `MsGPUBusy`；<0 = 本行没有该列
    bool dropped = false;      ///< `Dropped`：被显示管线丢掉的帧，不计入 FPS（但计数）
    bool valid = true;         ///< 加载 / 失焦帧由上层置 false：计入剔除数，不进统计
    std::string presentMode;   ///< `PresentMode`（如 "Hardware: Independent Flip"）
};

/// 口径参数（来自 config.json 的 fps 段）。
struct FrameStatsOptions {
    double lowPercentile = 0.01;              ///< 0.001..0.05
    double stutterFactor = 2.0;               ///< 1.5..10.0
    wpm::config::StutterBaseline baseline = wpm::config::StutterBaseline::Mean;
    double maxPlausibleFrameTimeMs = 1000.0;  ///< 超过按无效帧剔除（断了电源/暂停/切桌面的帧）
};

struct FrameStats {
    uint64_t frames = 0;       ///< 参与统计的帧
    uint64_t rejected = 0;     ///< 剔除帧（≤0、超上限、显式无效）
    uint64_t dropped = 0;      ///< 被显示管线丢掉的帧
    double avgFps = 0.0;       ///< 调和平均
    double lowFps = 0.0;       ///< 本项目口径：最差 ceil(n × p) 帧的帧时间均值取倒数
    double lowFpsQuantile = 0.0;  ///< CapFrameX 口径：帧时间 ≥ 第 k 大的所有帧取均值再取倒数
    double frameTimeAvgMs = 0.0;
    double frameTimeMedianMs = 0.0;
    double frameTimeWorstMs = 0.0;
    double stutterPct = 0.0;
    double stutterBaselineMs = 0.0;  ///< 实际用的基线值（便于把口径写进 detail）
    uint64_t stutterFrames = 0;
    double cpuBusyAvgMs = 0.0;  ///< 无该列时为 0
    double gpuBusyAvgMs = 0.0;
    std::string presentMode;  ///< 出现次数最多的模式（空 = 无数据）
    uint32_t presentModeCount = 0;

    bool ok() const noexcept { return frames > 0; }
};

/// 定长环形缓冲：构造时一次性分配，Add 不做任何分配。
/// 内存：容量 × (4 B 帧时间 + 2 B 标志/模式 id) ≈ 393 KB @65536；cpu/gpu 忙两列按需惰性分配。
class FrameRing {
public:
    explicit FrameRing(size_t capacity);

    void Reset();
    void Add(const FrameSample& s);

    size_t Count() const noexcept { return count_; }
    size_t Rejected() const noexcept { return rejected_; }
    size_t Dropped() const noexcept { return dropped_; }
    size_t Capacity() const noexcept { return capacity_; }
    bool Empty() const noexcept { return count_ == 0; }

    FrameStats Compute(const FrameStatsOptions& opt = {}) const;

    /// 环形缓冲里第 i 个（0 = 最旧）帧的帧时间，毫秒。越界返回 0。
    float FrameTimeAt(size_t i) const noexcept;

private:
    void EnsureBusyArrays();
    uint8_t InternMode(std::string_view mode);
    size_t RingIndex(size_t i) const noexcept;

    struct ModeEntry {
        std::string name;
    };

    size_t capacity_ = 0;
    size_t head_ = 0;  ///< 下一个写入位置
    size_t count_ = 0;
    size_t rejected_ = 0;
    size_t dropped_ = 0;
    bool hasBusy_ = false;
    std::vector<float> frameMs_;
    std::vector<float> cpuBusyMs_;
    std::vector<float> gpuBusyMs_;
    std::vector<uint8_t> modeId_;
    std::vector<ModeEntry> modes_;  ///< 最多 255 项，第 255 个 id 表示「其它」
    mutable std::vector<double> scratch_;  ///< Compute 复用，避免每次调用重新分配
};

/// 从帧时间数组直接算统计（给单测与本文件之外的调用方用；不经过环形缓冲）。
FrameStats ComputeFromFrameTimes(const std::vector<double>& frameTimesMs,
                                 const FrameStatsOptions& opt = {});

}  // namespace wpm::fps
