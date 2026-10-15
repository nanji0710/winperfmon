// SPDX-License-Identifier: MIT
// Ring0 CPU 传感器（MSR / SMN）——本机是 AMD 时走 AMDFamily17 模块，Intel 走 IntelMSR 模块。
//
// 定位：PDH/ADL2 拿不到的那些指标。AMD 上 ADL2 已提供封装温度与功耗，本 provider 补
// 「每核实际频率」（APERF/MPERF）与「核心电压」（SVI2 遥测）；Intel 上它还是封装温度/功耗的唯一
// 免驱来源。所有读数都带反伪造判据，读不到就说读不到（绝不保留旧值冒充新值）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sensors/pawnio.h"
#include "sensors/sensor.h"

namespace wpm::sensors {

// ── 纯解码函数（抽出来是为了能用单测钉死口径，不需要真机也不需要驱动）──

/// AMD SMN `THM_TCON_CUR_TMP`（0x00059800）原始值 → 摄氏度。
/// 读数位（bits[31:21]）为 0 表示传感器未就绪，或解码结果不在 (0, 125] 时返回 false。
bool DecodeAmdSmnTemp(uint32_t raw, double* celsius);

/// AMD SVI2 遥测原始值 → 伏特（LHM 口径：`1.55 − 6.25 mV × ((raw >> 16) & 0xFF)`）。
/// 原始值为 0 表示该平面没有遥测（**不是** 1.55 V），或结果不在 0.30–1.60 V 时返回 false。
/// 本机（Family 19h model 0x75，Hawk Point）实测 0x5A008/0x5A00C/0x5A010 恒为 0。
bool DecodeAmdSviVoltage(uint32_t raw, double* volts);

/// APERF/MPERF 差分 → MHz（`baseClockMHz × ΔAPERF / ΔMPERF`）。
/// ΔMPERF 为 0、ΔAPERF 为 0、或 ΔAPERF > 4 × ΔMPERF（读数错位）时返回 false。
bool DecodePerCoreMHz(uint64_t dAperf, uint64_t dMperf, double baseClockMHz, double* mhz);

/// Intel `TEMPERATURE_TARGET` / `PACKAGE_THERM_STATUS` → 摄氏度（TjMax − 读数）。
/// TjMax 或数字读数为 0（传感器未就绪）时返回 false。
bool DecodeIntelPackageTemp(uint64_t tempTargetMsr, uint64_t pkgThermStatusMsr, double* celsius);

/// MSR 能量计数器单位（AMD `PWR_UNIT` / Intel `RAPL_POWER_UNIT`）→ 每次计数多少焦耳。
double EnergyUnitJoules(uint64_t pwrUnitMsr);

class CpuRing0Provider final : public ISensorProvider {
 public:
  CpuRing0Provider() = default;
  ~CpuRing0Provider() override = default;

  const char* name() const noexcept override { return "cpu.ring0"; }
  bool Init(std::string& detail) override;
  void Sample(Snapshot& snap) override;
  const std::vector<size_t>& Slots() const noexcept override { return slots_; }
  bool Available() const noexcept override { return available_; }

  /// 采样间隔（跨轮差分的窗口与文案都用它）。
  void SetIntervalMs(uint32_t ms) noexcept { windowMs_ = ms ? ms : 500; }

  /// --probe 用的诊断快照（会真读几个寄存器，只读不写）。
  struct Diag {
    bool amd = false;                 ///< 判定为 AMD 平台
    std::string devicePath;           ///< 已打开的 PawnIO 设备路径
    std::string driverVersion;        ///< 注册表 DisplayVersion
    bool moduleLoaded = false;        ///< 厂商 MSR/SMN 模块是否载入
    std::string modulePath;           ///< 模块文件路径
    std::string moduleNote;           ///< 载入失败原因
    bool smnTempOk = false;           ///< SMN 温度读取成功
    bool smnTempUsed = false;         ///< 本轮真正采用了 SMN 温度（ADL2 未覆盖时才为 true）
    uint32_t smnTempRaw = 0;
    double smnTempC = 0.0;
    bool voltageOk = false;
    uint32_t voltageOffset = 0;
    uint32_t voltageRaw = 0;
    double voltageV = 0.0;
    uint32_t tfnRaw = 0;              ///< SVI0_TFN（bit0/bit1 置位 = 对应平面读不出）
    std::string voltageNote;          ///< 电压读不到时的具体原因（写进降级文案）
    bool pkgEnergyOk = false;
    double pkgPowerW = 0.0;
    uint32_t pwrUnitRaw = 0;
    unsigned perCoreSampled = 0;
    unsigned perCoreTotal = 0;
    double perCoreMinMHz = 0.0;
    double perCoreMaxMHz = 0.0;
    std::string note;
  };
  Diag Probe();

  /// 初始化细节（Collector 的降级文案会用它）。
  const std::string& InitDetail() const noexcept { return initDetail_; }

 private:
  bool ReadSmn(uint32_t offset, uint32_t* value);
  bool ReadMsr(uint32_t index, uint64_t* value);

  /// 采样一批核的 APERF/MPERF（把线程钉到目标核）。
  /// 采用「跨轮差分」而不是「轮内 Sleep 再读」：后者会撞上 Windows 默认 15.6 ms 定时器粒度，
  /// 每次要白等 15 ms；跨轮差分不需要任何 Sleep，窗口天然等于采样间隔，读数也更代表平均频率。
  /// 返回本轮成功算出频率的核数。
  unsigned SamplePerCoreBatch();

  /// 诊断用：一次性把全部核扫一遍（内部只 Sleep 一次 windowMs），返回成功核数。
  unsigned SweepAllCores(uint32_t windowMs);

  bool ReadPerCoreMHz(unsigned core, double* mhz);

  /// AMD：读并解码当前封装温度（°C）——失败返回 false。
  bool ReadAmdPackageTemp(double* celsius, uint32_t* raw);

  /// AMD：读 SVI2 遥测平面，取一个落在合理区间的核心电压。
  /// TFN（SVI0_TFN，+0x8）的 bit0/bit1 置位表示对应平面读不出；平面偏移随型号不同
  /// （Zen 3 与 Zen 1/2 的 +0xC/+0x10 是反的），由 amdModel_ 决定。
  bool ReadAmdCoreVoltage(double* volts, uint32_t* offset, uint32_t* raw, uint32_t* tfn,
                          std::string* note);

  size_t SlotOf(const char* id) const;

  std::vector<size_t> slots_;
  bool available_ = false;
  bool amd_ = false;
  uint32_t amdModel_ = 0;  ///< CPUID 的型号（family 0x19 下 0x75 = Hawk Point）
  std::string initDetail_;
  std::string note_;

  PawnIoModule module_;

  size_t tempSlot_ = size_t(-1);
  size_t voltSlot_ = size_t(-1);
  size_t perCoreSlot_ = size_t(-1);
  size_t powerSlot_ = size_t(-1);

  double baseClockMHz_ = 0.0;
  unsigned coreCount_ = 0;

  // 每核频率：轮换采样（每轮只钉几个核，跨轮差分，不需要任何 Sleep）。
  std::vector<double> perCoreMHz_;
  std::vector<uint64_t> perCoreStampMs_;   ///< 该核上次算出频率的时间戳（GetTickCount64）
  std::vector<uint64_t> prevAperf_;        ///< 该核上一次读到的 APERF
  std::vector<uint64_t> prevMperf_;        ///< 该核上一次读到的 MPERF
  std::vector<uint8_t> primed_;            ///< 该核是否已有基线（第一轮只建基线）
  unsigned nextCore_ = 0;
  uint32_t windowMs_ = 500;                ///< 采样间隔（跨轮差分的窗口，仅用于文案）

  // AMD 封装功耗（MSR 能量计数器差分）。
  bool haveEnergy_ = false;
  uint64_t lastEnergy_ = 0;
  uint64_t lastEnergyTick_ = 0;
  uint32_t pwrUnitRaw_ = 0;
};

}  // namespace wpm::sensors
