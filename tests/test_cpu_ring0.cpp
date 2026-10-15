// SPDX-License-Identifier: MIT
// Ring0 解码口径的纯函数契约（sensors/cpu_ring0.h）。
//
// 为什么值得单独测：这些解码公式把"寄存器里的整数"变成"用户看到的温度/电压/频率"，
// 而它们的错误方式很隐蔽——例如 AMD SMN 温度读数为 0 时公式会算出 −49 °C，
// SVI 电压的 vid 截断会算出负数。这些数一旦显示出来就是假数据，所以每个分支都要钉住。
#include "test.h"

#include "sensors/cpu_ring0.h"

#include <cmath>

using wpm::sensors::DecodeAmdSmnTemp;
using wpm::sensors::DecodeAmdSviVoltage;
using wpm::sensors::DecodeIntelPackageTemp;
using wpm::sensors::DecodePerCoreMHz;
using wpm::sensors::EnergyUnitJoules;

namespace {
// THM_TCON_CUR_TMP 的编码：bits[31:21] = 8 × (T + 49)
uint32_t SmnTempRaw(double celsius) {
    const auto reading = static_cast<uint32_t>((celsius + 49.0) * 8.0);
    return reading << 21;
}
}  // namespace

WPM_TEST(ring0_smn_temp_roundtrip) {
    double t = 0.0;
    WPM_CHECK(DecodeAmdSmnTemp(SmnTempRaw(49.0), &t));
    WPM_CHECK_NEAR(t, 49.0, 1e-9);
    WPM_CHECK(DecodeAmdSmnTemp(SmnTempRaw(95.5), &t));
    WPM_CHECK_NEAR(t, 95.5, 1e-9);
}

WPM_TEST(ring0_smn_temp_rejects_not_ready) {
    double t = 0.0;
    // 读数位为 0 = 传感器未就绪。绝不能让它变成 −49 °C。
    WPM_CHECK(!DecodeAmdSmnTemp(0u, &t));
    // 超出物理区间的值（>125 °C）同样判为读不到
    WPM_CHECK(!DecodeAmdSmnTemp(SmnTempRaw(130.0), &t));
}

WPM_TEST(ring0_svi_voltage_decode) {
    double v = 0.0;
    // 原始值 0 = 该平面没有遥测（本机实测恒为 0）。绝不能解成 1.55 V —— 那是个漂亮但假的读数。
    WPM_CHECK(!DecodeAmdSviVoltage(0u, &v));
    WPM_CHECK(DecodeAmdSviVoltage((88u << 16) | 0xFFFFu, &v));  // 低 16 位是电流等其它数据，必须掩掉
    WPM_CHECK_NEAR(v, 1.00, 1e-9);
    WPM_CHECK(DecodeAmdSviVoltage(0x20u << 16, &v));  // vid = 32 → 1.55 − 0.20 = 1.35 V
    WPM_CHECK_NEAR(v, 1.35, 1e-9);
    WPM_CHECK(DecodeAmdSviVoltage(88u << 16, &v));  // vid = 88 → 1.55 − 0.55 = 1.00 V
    WPM_CHECK_NEAR(v, 1.00, 1e-9);
}

WPM_TEST(ring0_svi_voltage_rejects_out_of_range) {
    double v = 0.0;
    WPM_CHECK(!DecodeAmdSviVoltage(0x00FF0000u, &v));  // vid = 0xFF → 1.55 − 1.594 < 0
    WPM_CHECK(!DecodeAmdSviVoltage(250u << 16, &v));   // vid = 250 → 1.55 − 1.5625 < 0
    WPM_CHECK(DecodeAmdSviVoltage(1u, &v));  // 低 16 位非零、vid = 0 → 编码上限 1.55 V
    WPM_CHECK_NEAR(v, 1.55, 1e-9);
}

WPM_TEST(ring0_per_core_mhz_ratio) {
    double m = 0.0;
    WPM_CHECK(DecodePerCoreMHz(1000u, 1000u, 3801.0, &m));  // 比值 1 → 基频
    WPM_CHECK_NEAR(m, 3801.0, 1e-9);
    WPM_CHECK(DecodePerCoreMHz(2000u, 1000u, 3801.0, &m));  // 比值 2 → 双倍
    WPM_CHECK_NEAR(m, 7602.0, 1e-9);
}

WPM_TEST(ring0_per_core_mhz_rejects_bad_delta) {
    double m = 0.0;
    WPM_CHECK(!DecodePerCoreMHz(1000u, 0u, 3801.0, &m));      // ΔMPERF = 0
    WPM_CHECK(!DecodePerCoreMHz(0u, 1000u, 3801.0, &m));      // ΔAPERF = 0
    WPM_CHECK(!DecodePerCoreMHz(5000u, 1000u, 3801.0, &m));   // 比值 > 4 → 读数错位
    WPM_CHECK(!DecodePerCoreMHz(1000u, 1000u, 0.0, &m));      // 没有基频
    WPM_CHECK(!DecodePerCoreMHz(1000000u, 1u, 3801.0, &m));   // 离谱高频 → 读不到
}

WPM_TEST(ring0_intel_package_temp) {
    const uint64_t target = static_cast<uint64_t>(100) << 16;  // TjMax = 100
    double t = 0.0;
    WPM_CHECK(DecodeIntelPackageTemp(target, static_cast<uint64_t>(40) << 16, &t));
    WPM_CHECK_NEAR(t, 60.0, 1e-9);
    // 数字读数为 0 表示传感器未就绪 → 不能算成 TjMax 度
    WPM_CHECK(!DecodeIntelPackageTemp(target, 0u, &t));
    WPM_CHECK(!DecodeIntelPackageTemp(0u, static_cast<uint64_t>(40) << 16, &t));
}

WPM_TEST(ring0_energy_unit) {
    WPM_CHECK_NEAR(EnergyUnitJoules(0u), 1.0, 1e-12);                      // ESU = 0 → 1 J
    WPM_CHECK_NEAR(EnergyUnitJoules(10u << 8), 1.0 / 1024.0, 1e-15);       // ESU = 10 → 2^-10 J
}
