// SPDX-License-Identifier: MIT
// 原生 CPU 占用率的纯函数契约（sensors/cpu_native.h）。
//
// 为什么值得单独测：占用率是 UI 上最显眼的数字，而它的"分子"来自
// NtQuerySystemInformation 的累计计数器差分——回绕、重复读取、时钟未前进
// 这三种情况如果被算成 0% 或 100%，用户看到的就是假数据。
#include "test.h"

#include "sensors/cpu_native.h"

using wpm::sensors::BusyPercentFromDelta;
using wpm::sensors::kNoSlotIndex;

WPM_TEST(cpu_native_busy_percent_half) {
    // 一个采样窗口里 idle 与 busy 各走 500 → 50%
    WPM_CHECK_NEAR(BusyPercentFromDelta(1000, 2000, 1500, 2500), 50.0, 1e-9);
}

WPM_TEST(cpu_native_busy_percent_bounds) {
    WPM_CHECK_NEAR(BusyPercentFromDelta(1000, 2000, 1000, 3000), 100.0, 1e-9);  // 全忙
    WPM_CHECK_NEAR(BusyPercentFromDelta(1000, 2000, 3000, 2000), 0.0, 1e-9);    // 全闲
}

WPM_TEST(cpu_native_busy_percent_rejects_bad_delta) {
    // 计数器回绕（后值小于前值）必须判为"读不到"，不能算成 0%
    WPM_CHECK(BusyPercentFromDelta(5000, 5000, 10, 10) < 0.0);
    WPM_CHECK(BusyPercentFromDelta(1000, 2000, 1000, 1500) < 0.0);  // busy 倒退
    // 两次采样之间时钟没走：没有有效窗口，同样判为读不到
    WPM_CHECK(BusyPercentFromDelta(1000, 2000, 1000, 2000) < 0.0);
}

WPM_TEST(cpu_native_busy_percent_never_exceeds_bounds) {
    // 即便输入极端，输出也必须落在 [0, 100]（UI 与阈值判断都依赖这条）
    const double pct = BusyPercentFromDelta(0, 0, 1, 1000000);
    WPM_CHECK(pct >= 0.0 && pct <= 100.0);
}

WPM_TEST(cpu_native_no_slot_index_is_sentinel) {
    // kNoSlotIndex 是"没有槽位"的哨兵，必须明显不是合法下标
    WPM_CHECK(kNoSlotIndex > 1000u);
}
