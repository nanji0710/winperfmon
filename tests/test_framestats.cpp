// SPDX-License-Identifier: MIT
// 帧统计口径的单测：这些数字是要写进用户报告的，口径错了没人看得出来（"1% Low 差 0.5 FPS"
// 这种验收要求必须靠这里钉死）。所有用例都是合成数据，不含任何真实游戏样本。
#include "fps/framestats.h"

#include <cmath>
#include <vector>

#include "test.h"

using namespace wpm::fps;

namespace {

std::vector<double> Repeat(double ms, size_t n) { return std::vector<double>(n, ms); }

void Append(std::vector<double>* v, double ms, size_t n) {
    for (size_t i = 0; i < n; ++i) v->push_back(ms);
}

FrameRing MakeRing(size_t cap, const std::vector<double>& times) {
    FrameRing ring(cap);
    for (double ms : times) {
        FrameSample s;
        s.frameTimeMs = ms;
        ring.Add(s);
    }
    return ring;
}

}  // namespace

// 平均 FPS 必须是调和平均：n × 1000 / Σ帧时间。
// 反例（本用例会把它钉死）：先对帧时间取算术平均再取倒数，或对各帧瞬时 FPS 取算术平均。
WPM_TEST(framestats_harmonic_mean_not_arithmetic) {
    std::vector<double> times = Repeat(10.0, 99);
    times.push_back(25.0);
    const FrameStats st = ComputeFromFrameTimes(times);

    WPM_CHECK_EQ(st.frames, 100u);
    // 调和平均：100 × 1000 / (99×10 + 25) = 100000 / 1015
    WPM_CHECK_NEAR(st.avgFps, 100000.0 / 1015.0, 1e-9);
    // 若误用「瞬时 FPS 的算术平均」，会得到 (99×100 + 40)/100 = 99.4
    WPM_CHECK(st.avgFps < 99.0);
    WPM_CHECK_NEAR(st.frameTimeAvgMs, 10.15, 1e-9);
}

// 无异常帧时，1% Low 应等于平均帧率的倒数（即帧时间都一样时两条 Low 口径都退化成同一个数）
WPM_TEST(framestats_uniform_frames_low_equals_avg) {
    const FrameStats st = ComputeFromFrameTimes(Repeat(16.6667, 1000));
    WPM_CHECK_NEAR(st.avgFps, 60.0001, 0.01);
    WPM_CHECK_NEAR(st.lowFps, st.avgFps, 1e-6);
    WPM_CHECK_NEAR(st.lowFpsQuantile, st.avgFps, 1e-6);
    WPM_CHECK_NEAR(st.stutterPct, 0.0, 1e-12);
}

// 1% Low 的项目口径：最差 ceil(n × 1%) 帧的帧时间均值取倒数。
// 990 帧 10 ms + 10 帧 33.3333 ms → 最差 10 帧均值 33.3333 → 30.0 FPS。
WPM_TEST(framestats_low1_project_definition) {
    std::vector<double> times = Repeat(10.0, 990);
    Append(&times, 1000.0 / 30.0, 10);  // 33.3333 ms
    const FrameStats st = ComputeFromFrameTimes(times);

    WPM_CHECK_EQ(st.frames, 1000u);
    WPM_CHECK_NEAR(st.lowFps, 30.0, 1e-6);
    WPM_CHECK_NEAR(st.lowFpsQuantile, 30.0, 1e-6);  // 无并列时两条口径一致
    WPM_CHECK_NEAR(st.frameTimeWorstMs, 1000.0 / 30.0, 1e-9);
    WPM_CHECK(st.lowFps < st.avgFps);
}

// 有并列帧时间时，CapFrameX 的分位阈值口径会纳入更多帧 → 两个数必须都给出来，不能私吞差异。
WPM_TEST(framestats_low_quantile_variant_diverges_on_ties) {
    std::vector<double> times = Repeat(10.0, 100);
    times.push_back(50.0);
    const FrameStats st = ComputeFromFrameTimes(times);

    // 项目口径：k = ceil(101 × 0.01) = 2 → 最差两帧 (50, 10) 均值 30 → 33.333 FPS
    WPM_CHECK_NEAR(st.lowFps, 1000.0 / 30.0, 1e-6);
    // 分位口径：阈值 = 第 2 大 = 10 ms → 101 帧全部 >= 10 → 均值 (100×10+50)/101
    WPM_CHECK_NEAR(st.lowFpsQuantile, 1000.0 / ((100.0 * 10.0 + 50.0) / 101.0), 1e-6);
    WPM_CHECK(st.lowFpsQuantile > st.lowFps * 2.0);
}

// 0.1% Low：p = 0.001 时 k 至少为 1（不能算出 0 帧）
WPM_TEST(framestats_low01_keeps_at_least_one_frame) {
    std::vector<double> times = Repeat(10.0, 999);
    times.push_back(100.0);
    FrameStatsOptions opt;
    opt.lowPercentile = 0.001;
    const FrameStats st = ComputeFromFrameTimes(times, opt);
    WPM_CHECK_NEAR(st.lowFps, 10.0, 1e-9);  // 只看最差的那 1 帧
}

// 卡顿率没有魔数：100 × count(帧时间 > factor × 基线) / n
WPM_TEST(framestats_stutter_uses_factor_times_baseline) {
    std::vector<double> times = Repeat(10.0, 9);
    times.push_back(25.0);
    FrameStatsOptions opt;
    opt.stutterFactor = 2.0;
    opt.baseline = wpm::config::StutterBaseline::Mean;

    const FrameStats st = ComputeFromFrameTimes(times, opt);
    WPM_CHECK_NEAR(st.stutterBaselineMs, 11.5, 1e-9);  // (9×10 + 25)/10
    WPM_CHECK_EQ(st.stutterFrames, 1u);
    WPM_CHECK_NEAR(st.stutterPct, 10.0, 1e-9);

    // 换中位数基线：median = 10 → 阈值 20 → 25 ms 那一帧仍然超
    opt.baseline = wpm::config::StutterBaseline::Median;
    const FrameStats st2 = ComputeFromFrameTimes(times, opt);
    WPM_CHECK_NEAR(st2.stutterBaselineMs, 10.0, 1e-9);
    WPM_CHECK_EQ(st2.stutterFrames, 1u);

    // 阈值调高后不该再有卡顿帧
    opt.stutterFactor = 3.0;
    const FrameStats st3 = ComputeFromFrameTimes(times, opt);
    WPM_CHECK_EQ(st3.stutterFrames, 0u);
    WPM_CHECK_NEAR(st3.stutterPct, 0.0, 1e-12);
}

// 异常帧剔除：<= 0 与 > 上限的帧不计入统计，但要计入 rejected（不能让它们污染平均值）
WPM_TEST(framestats_rejects_implausible_frames) {
    std::vector<double> times = {10.0, 0.0, -5.0, 1500.0, 8.0};
    const FrameStats st = ComputeFromFrameTimes(times);
    WPM_CHECK_EQ(st.frames, 2u);
    WPM_CHECK_EQ(st.rejected, 3u);
    WPM_CHECK_NEAR(st.avgFps, 2000.0 / 18.0, 1e-9);
    WPM_CHECK_NEAR(st.frameTimeWorstMs, 10.0, 1e-9);
}

// 中位数：偶数个取中间两个的平均
WPM_TEST(framestats_median_even_and_odd) {
    const FrameStats even = ComputeFromFrameTimes({10.0, 20.0, 30.0, 40.0});
    WPM_CHECK_NEAR(even.frameTimeMedianMs, 25.0, 1e-9);
    const FrameStats odd = ComputeFromFrameTimes({10.0, 20.0, 30.0});
    WPM_CHECK_NEAR(odd.frameTimeMedianMs, 20.0, 1e-9);
}

// 环形缓冲绕圈：容量 100、送 150 帧后只保留最后 100 帧（最旧的是第 50 帧）
WPM_TEST(framestats_ring_wraps_and_keeps_newest) {
    FrameRing ring(100);
    for (int i = 0; i < 150; ++i) {
        FrameSample s;
        s.frameTimeMs = static_cast<double>(i + 1);
        ring.Add(s);
    }
    WPM_CHECK_EQ(ring.Count(), 100u);
    WPM_CHECK_NEAR(ring.FrameTimeAt(0), 51.0, 1e-6);
    WPM_CHECK_NEAR(ring.FrameTimeAt(99), 150.0, 1e-6);
    const FrameStats st = ring.Compute();
    WPM_CHECK_EQ(st.frames, 100u);
    WPM_CHECK_NEAR(st.frameTimeWorstMs, 150.0, 1e-6);
}

WPM_TEST(framestats_ring_rejects_and_drops_are_counted_separately) {
    FrameRing ring(16);
    FrameSample good;
    good.frameTimeMs = 10.0;
    ring.Add(good);

    FrameSample bad;
    bad.frameTimeMs = 0.0;
    ring.Add(bad);

    FrameSample dropped;
    dropped.frameTimeMs = 12.0;
    dropped.dropped = true;
    ring.Add(dropped);

    FrameSample invalid;
    invalid.frameTimeMs = 11.0;
    invalid.valid = false;
    ring.Add(invalid);

    WPM_CHECK_EQ(ring.Count(), 1u);
    WPM_CHECK_EQ(ring.Rejected(), 2u);
    WPM_CHECK_EQ(ring.Dropped(), 1u);
    const FrameStats st = ring.Compute();
    WPM_CHECK_EQ(st.frames, 1u);
    WPM_CHECK_EQ(st.rejected, 2u);
    WPM_CHECK_EQ(st.dropped, 1u);
}

// cpu/gpu 忙两列：缺该列的帧不参与平均（但参与帧率统计）
WPM_TEST(framestats_busy_columns_are_optional_per_frame) {
    FrameRing ring(8);
    FrameSample a;
    a.frameTimeMs = 10.0;
    a.cpuBusyMs = 1.0;
    a.gpuBusyMs = 8.0;
    ring.Add(a);
    FrameSample b;
    b.frameTimeMs = 10.0;
    b.cpuBusyMs = 3.0;  // 没有 MsGPUBusy
    ring.Add(b);

    const FrameStats st = ring.Compute();
    WPM_CHECK_NEAR(st.cpuBusyAvgMs, 2.0, 1e-6);
    WPM_CHECK_NEAR(st.gpuBusyAvgMs, 8.0, 1e-6);
    WPM_CHECK_EQ(st.frames, 2u);
}

// PresentMode：报出现次数最多的那个；空模式不计
WPM_TEST(framestats_present_mode_majority) {
    FrameRing ring(8);
    auto add = [&ring](const char* mode) {
        FrameSample s;
        s.frameTimeMs = 10.0;
        s.presentMode = mode ? mode : "";
        ring.Add(s);
    };
    add("Composed: Copy");
    add("Hardware: Independent Flip");
    add("Hardware: Independent Flip");
    add("Hardware: Independent Flip");
    add("Composed: Copy");
    const FrameStats st = ring.Compute();
    WPM_CHECK(st.presentMode == "Hardware: Independent Flip");
    WPM_CHECK_EQ(st.presentModeCount, 3u);
}

WPM_TEST(framestats_empty_ring_is_not_ok) {
    FrameRing ring(8);
    const FrameStats st = ring.Compute();
    WPM_CHECK(!st.ok());
    WPM_CHECK_EQ(st.frames, 0u);
    WPM_CHECK_NEAR(st.avgFps, 0.0, 1e-12);
    WPM_CHECK_NEAR(st.lowFps, 0.0, 1e-12);
}

// 环形缓冲与「直接算」两条路径必须给出同一个答案（否则 watch 与报告会对不上）
WPM_TEST(framestats_ring_and_direct_paths_agree) {
    std::vector<double> times = Repeat(12.5, 40);
    Append(&times, 40.0, 7);
    Append(&times, 9.0, 53);
    const FrameRing ring = MakeRing(256, times);

    FrameStatsOptions opt;
    opt.lowPercentile = 0.05;
    opt.stutterFactor = 2.5;
    const FrameStats viaRing = ring.Compute(opt);
    const FrameStats direct = ComputeFromFrameTimes(times, opt);

    WPM_CHECK_EQ(viaRing.frames, direct.frames);
    WPM_CHECK_NEAR(viaRing.avgFps, direct.avgFps, 1e-6);
    WPM_CHECK_NEAR(viaRing.lowFps, direct.lowFps, 1e-6);
    WPM_CHECK_NEAR(viaRing.lowFpsQuantile, direct.lowFpsQuantile, 1e-6);
    WPM_CHECK_NEAR(viaRing.frameTimeMedianMs, direct.frameTimeMedianMs, 1e-6);
    WPM_CHECK_NEAR(viaRing.stutterPct, direct.stutterPct, 1e-6);
}

// Reset 之后一切从头开始（换游戏时需要）
WPM_TEST(framestats_ring_reset_clears_state) {
    FrameRing ring(8);
    FrameSample s;
    s.frameTimeMs = 10.0;
    s.presentMode = "Composed: Copy";
    ring.Add(s);
    ring.Reset();
    WPM_CHECK_EQ(ring.Count(), 0u);
    WPM_CHECK(ring.Empty());
    const FrameStats st = ring.Compute();
    WPM_CHECK(!st.ok());
    WPM_CHECK(st.presentMode.empty());
}
