// SPDX-License-Identifier: MIT
#include "sensors/cpu_ring0.h"

#include <windows.h>

#include <algorithm>
#include <cpuid.h>
#include <cstdio>

#include "config/catalog.h"
#include "core/text.h"
#include "sensors/cpu_info.h"

namespace wpm::sensors {
namespace {

// —— AMD SMN 偏移（来自 LibreHardwareMonitor 的 Amd17Cpu 常量表；SMN = 通过 ioctl_read_smn 访问的
//    内部间接寻址空间，不是 PCI 配置空间） ——
constexpr uint32_t kSmnThmTcon = 0x00059800;   // THM_TCON_CUR_TMP：bits[31:21] 温度读数
constexpr uint32_t kSmnSviTfn = 0x0005A008;    // SVI0_TFN：bit0/bit1 置位 = 对应平面读不出
constexpr uint32_t kSmnSvi = 0x0005A000;       // F17H_M01H_SVI 基址，平面偏移随型号不同

// SVI2 遥测平面偏移（LHM Amd17Cpu 的 switch）：Zen 3 与 Zen 1/2 的 +0xC/+0x10 是反的，
// 取错偏移会解出一个看似合理、实则属于另一路电源的电压。
//   0x31（Threadripper 3000）→ plane0 = +0x14, plane1 = +0x10
//   0x21（Zen 3）/ 0x44（Zen 5）→ plane0 = +0x10, plane1 = +0xC
//   其它（含 0x75 Hawk Point）→ plane0 = +0xC, plane1 = +0x10
void SviPlaneOffsets(uint32_t model, uint32_t* plane0, uint32_t* plane1) {
  switch (model) {
    case 0x31:
      *plane0 = kSmnSvi + 0x14;
      *plane1 = kSmnSvi + 0x10;
      break;
    case 0x21:
    case 0x44:
      *plane0 = kSmnSvi + 0x10;
      *plane1 = kSmnSvi + 0x0C;
      break;
    default:
      *plane0 = kSmnSvi + 0x0C;
      *plane1 = kSmnSvi + 0x10;
      break;
  }
}

/// AMD 型号（CPUID.1.EAX 的 extModel<<4 | baseModel）。仅用于选 SVI 平面偏移。
uint32_t AmdCpuModel() {
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) return 0;
  return ((eax >> 16) & 0xFu) << 4 | ((eax >> 4) & 0xFu);
}

// —— AMD MSR ——
constexpr uint32_t kMsrMperf = 0xC00000E7;        // MPERF：基准频率节拍
constexpr uint32_t kMsrAperf = 0xC00000E8;        // APERF：实际频率节拍
constexpr uint32_t kMsrPwrUnit = 0xC0010299;      // PWR_UNIT：能量/时间/功率单位
constexpr uint32_t kMsrPkgEnergyStat = 0xC001029B; // PKG_ENERGY_STAT：封装累计能量（48 位）

// —— Intel MSR ——
constexpr uint32_t kMsrTempTarget = 0x1A2;         // TEMPERATURE_TARGET：bits[23:16] = TjMax
constexpr uint32_t kMsrPkgThermStatus = 0x1B1;     // PACKAGE_THERM_STATUS：bits[22:16] = 数字读数
constexpr uint32_t kMsrRaplPwrUnit = 0x606;        // RAPL_POWER_UNIT：bits[12:8] = 能量单位指数
constexpr uint32_t kMsrRaplPkgEnergy = 0x611;      // RAPL PKG_ENERGY_STATUS

constexpr unsigned kCoresPerTick = 4;   // 每轮钉几个核（16 核 ≈ 每 2 s 全部刷新一次）
constexpr double kMinWatts = 0.3;
constexpr double kMaxWatts = 250.0;

/// 把线程钉到某个逻辑核；返回 false 表示钉不上（此时绝不读该核的 MSR：读到的会是别人的值）。
bool PinToCore(unsigned core, DWORD_PTR* previous) {
  if (core >= 64) return false;  // 本项目目标 ≤64 逻辑处理器（ReadPhysicalCoreCount 同口径）
  const DWORD_PTR want = static_cast<DWORD_PTR>(1) << core;
  const DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), want);
  if (prev == 0) return false;
  *previous = prev;
  return true;
}

void Unpin(DWORD_PTR previous) { SetThreadAffinityMask(GetCurrentThread(), previous); }

}  // namespace

// ────────────────────────────── 纯解码 ──────────────────────────────

bool DecodeAmdSmnTemp(uint32_t raw, double* celsius) {
  const uint32_t reading = (raw >> 21) & 0x7FFu;
  if (reading == 0) return false;  // 0 = 传感器未就绪（与 ACPI 的 2732 同类陷阱）
  const double t = static_cast<double>(reading) / 8.0 - 49.0;
  if (t <= 0.0 || t > 125.0) return false;
  *celsius = t;
  return true;
}

bool DecodeAmdSviVoltage(uint32_t raw, double* volts) {
  // 0 = 该平面没有遥测。LHM 不判这一条，于是会把"读不到"显示成 1.55 V —— 本机实测正是这种
  // 情况（0x5A008/0x5A00C/0x5A010 恒为 0），所以这里必须拒绝，而不是报一个漂亮但假的电压。
  if (raw == 0) return false;
  const uint32_t vid = (raw >> 16) & 0xFFu;  // SVI0_PLANEX_VDDCOR [24:16]，见 LHM Amd17Cpu
  const double v = 1.55 - 0.00625 * static_cast<double>(vid);
  if (v < 0.30 || v > 1.60) return false;
  *volts = v;
  return true;
}

bool DecodePerCoreMHz(uint64_t dAperf, uint64_t dMperf, double baseClockMHz, double* mhz) {
  if (baseClockMHz <= 0.0 || dMperf == 0 || dAperf == 0) return false;
  if (dAperf > dMperf * 4) return false;  // da/dm 远超 1 说明两次读数错位
  const double v = baseClockMHz * static_cast<double>(dAperf) / static_cast<double>(dMperf);
  if (v < 100.0 || v > 12000.0) return false;
  *mhz = v;
  return true;
}

bool DecodeIntelPackageTemp(uint64_t tempTargetMsr, uint64_t pkgThermStatusMsr, double* celsius) {
  const unsigned tjMax = static_cast<unsigned>((tempTargetMsr >> 16) & 0xFFu);
  const unsigned readout = static_cast<unsigned>((pkgThermStatusMsr >> 16) & 0x7Fu);
  if (tjMax == 0 || readout == 0) return false;
  *celsius = static_cast<double>(tjMax) - static_cast<double>(readout);
  return true;
}

double EnergyUnitJoules(uint64_t pwrUnitMsr) {
  const unsigned esu = static_cast<unsigned>((pwrUnitMsr >> 8) & 0x1Fu);
  // 能量单位 = 2^-ESU 焦耳（AMD PWR_UNIT 与 Intel RAPL_POWER_UNIT 同构）。
  return 1.0 / static_cast<double>(uint64_t{1} << esu);
}

// ────────────────────────────── 初始化 ──────────────────────────────

size_t CpuRing0Provider::SlotOf(const char* id) const {
  const auto catalog = wpm::config::MetricCatalog();
  for (size_t i = 0; i < catalog.size(); ++i) {
    if (catalog[i].id == id) return i;
  }
  return size_t(-1);
}

bool CpuRing0Provider::Init(std::string& detail) {
  tempSlot_ = SlotOf("cpu.temp.package");
  voltSlot_ = SlotOf("cpu.voltage");
  perCoreSlot_ = SlotOf("cpu.freq.percore");
  powerSlot_ = SlotOf("cpu.power.package");
  for (size_t s : {tempSlot_, voltSlot_, perCoreSlot_, powerSlot_}) {
    if (s != size_t(-1)) slots_.push_back(s);
  }
  if (slots_.empty()) {
    detail = "指标目录里没有本 provider 负责的项";
    return false;
  }

  const std::string brand = ReadCpuBrand();
  std::string upper = brand;
  for (char& c : upper) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
  amd_ = upper.find("AMD") != std::string::npos;
  const bool intel = upper.find("INTEL") != std::string::npos;
  if (!amd_ && !intel) {
    detail = "未识别的 CPU 厂商（品牌串：" + brand + "），Ring0 指标暂只支持 AMD / Intel";
    return false;
  }
  // 型号只用来选 SVI2 平面偏移（Zen 3 与 Zen 1/2 相反）。品牌串不可靠，走 CPUID。
  if (amd_) amdModel_ = AmdCpuModel();

  auto* dev = PawnIoDevice::Instance();
  std::string openDetail;
  if (!dev->Open(&openDetail)) {
    detail = "PawnIO 不可用：" + openDetail;
    return false;
  }

  const char* moduleName = amd_ ? "AMDFamily17.bin" : "IntelMSR.bin";
  std::string loadDetail;
  if (!dev->LoadModule(moduleName, &module_, &loadDetail)) {
    detail = std::string(amd_ ? "AMD" : "Intel") + " 模块载入失败：" + loadDetail;
    return false;
  }

  baseClockMHz_ = ReadBaseClockMHz();
  coreCount_ = ReadLogicalCoreCount();
  if (coreCount_ == 0 || coreCount_ > 64) {
    detail = "逻辑核数异常（" + std::to_string(coreCount_) + "），不予接线";
    return false;
  }
  perCoreMHz_.assign(coreCount_, 0.0);
  perCoreStampMs_.assign(coreCount_, 0);
  prevAperf_.assign(coreCount_, 0);
  prevMperf_.assign(coreCount_, 0);
  primed_.assign(coreCount_, 0);

  // 自检：MSR 读一次 APERF（读不到说明模块/权限不对，宁可完全不接线）。
  uint64_t probe = 0;
  std::string msrDetail;
  if (!ReadMsr(kMsrAperf, &probe)) {
    detail = "MSR 读取自检失败：" + msrDetail;
    return false;
  }

  if (amd_) {
    uint32_t unit = 0;
    if (ReadMsr(kMsrPwrUnit, &probe)) {
      unit = static_cast<uint32_t>(probe);
      pwrUnitRaw_ = unit;
    }
  }
  available_ = true;
  initDetail_ = std::string(amd_ ? "AMD" : "Intel") + " Ring0（" + moduleName + "）就绪，模块 " +
                module_.path() + "；负责 " + std::to_string(slots_.size()) + " 项";
  detail = initDetail_;
  return true;
}

// ────────────────────────────── 读寄存器 ──────────────────────────────

bool CpuRing0Provider::ReadSmn(uint32_t offset, uint32_t* value) {
  if (!module_.loaded()) return false;
  uint64_t out = 0;
  std::string err;
  if (!module_.Call1("ioctl_read_smn", offset, &out, &err)) return false;
  *value = static_cast<uint32_t>(out);
  return true;
}

bool CpuRing0Provider::ReadMsr(uint32_t index, uint64_t* value) {
  if (!module_.loaded()) return false;
  std::string err;
  return module_.Call1("ioctl_read_msr", index, value, &err);
}

bool CpuRing0Provider::ReadAmdPackageTemp(double* celsius, uint32_t* raw) {
  uint32_t v = 0;
  if (!ReadSmn(kSmnThmTcon, &v)) return false;
  if (!DecodeAmdSmnTemp(v, celsius)) return false;
  if (raw != nullptr) *raw = v;
  return true;
}

bool CpuRing0Provider::ReadAmdCoreVoltage(double* volts, uint32_t* offset, uint32_t* raw,
                                          uint32_t* tfn, std::string* note) {
  uint32_t planes[2] = {0, 0};
  SviPlaneOffsets(amdModel_, &planes[0], &planes[1]);

  uint32_t tfnValue = 0;
  const bool haveTfn = ReadSmn(kSmnSviTfn, &tfnValue);
  if (tfn != nullptr) *tfn = tfnValue;
  // SVI0_TFN 的 bit0/bit1 是「该平面读不出」的标志位（LHM 判据：`(tfn & 1) == 0` 才认为 Core 有效；
  // Ryzen 7000/9000 上它干脆强制跳过两个平面）。TFN 自己都读不到时，两个平面都试一遍。
  const bool usable0 = !haveTfn || (tfnValue & 0x01u) == 0;
  const bool usable1 = !haveTfn || (tfnValue & 0x02u) == 0;
  const bool usable[2] = {usable0, usable1};

  bool sawNonZero = false;
  for (int i = 0; i < 2; ++i) {
    if (!usable[i]) continue;
    uint32_t v = 0;
    if (!ReadSmn(planes[i], &v)) continue;
    if (v != 0) sawNonZero = true;
    double decoded = 0.0;
    if (DecodeAmdSviVoltage(v, &decoded)) {
      *volts = decoded;
      if (offset != nullptr) *offset = planes[i];
      if (raw != nullptr) *raw = v;
      return true;
    }
  }
  if (note != nullptr) {
    *note = sawNonZero
                ? "SVI2 平面读到了非零值但解码后落在合理区间（0.30–1.60 V）之外，按读不到处理"
                : "SVI2 平面与 TFN 全为 0：本机 SMU 不通过 SVI2 暴露电压（实测 Family 19h "
                  "model 0x75 Hawk Point 恒为 0），0 不是 0 V、更不是 1.55 V，按读不到处理";
  }
  return false;
}

// ────────────────────────────── 每核频率 ──────────────────────────────

bool CpuRing0Provider::ReadPerCoreMHz(unsigned core, double* mhz) {
  if (core >= coreCount_ || baseClockMHz_ <= 0.0) return false;
  DWORD_PTR prev = 0;
  if (!PinToCore(core, &prev)) return false;
  uint64_t aperf = 0, mperf = 0;
  const bool ok = ReadMsr(kMsrAperf, &aperf) && ReadMsr(kMsrMperf, &mperf);
  Unpin(prev);
  if (!ok) return false;

  const bool had = primed_[core] != 0;
  const uint64_t prevA = prevAperf_[core];
  const uint64_t prevM = prevMperf_[core];
  prevAperf_[core] = aperf;
  prevMperf_[core] = mperf;
  primed_[core] = 1;
  if (!had) return false;                       // 第一轮只建基线

  const uint64_t da = aperf - prevA;            // 64 位无符号回绕天然正确
  const uint64_t dm = mperf - prevM;
  double v = 0.0;
  if (!DecodePerCoreMHz(da, dm, baseClockMHz_, &v)) return false;
  *mhz = v;
  perCoreMHz_[core] = v;
  perCoreStampMs_[core] = GetTickCount64();
  return true;
}

unsigned CpuRing0Provider::SamplePerCoreBatch() {
  if (coreCount_ == 0) return 0;
  unsigned ok = 0;
  for (unsigned k = 0; k < kCoresPerTick; ++k) {
    const unsigned core = nextCore_;
    nextCore_ = (nextCore_ + 1) % coreCount_;
    double mhz = 0.0;
    if (ReadPerCoreMHz(core, &mhz)) ++ok;
  }
  return ok;
}

unsigned CpuRing0Provider::SweepAllCores(uint32_t windowMs) {
  if (coreCount_ == 0) return 0;
  std::vector<uint64_t> a0(coreCount_, 0), m0(coreCount_, 0);
  std::vector<uint8_t> got(coreCount_, 0);
  for (unsigned core = 0; core < coreCount_; ++core) {
    DWORD_PTR prev = 0;
    if (!PinToCore(core, &prev)) continue;
    uint64_t a = 0, m = 0;
    if (ReadMsr(kMsrAperf, &a) && ReadMsr(kMsrMperf, &m)) {
      a0[core] = a;
      m0[core] = m;
      got[core] = 1;
    }
    Unpin(prev);
  }
  Sleep(windowMs);
  unsigned ok = 0;
  for (unsigned core = 0; core < coreCount_; ++core) {
    if (!got[core]) continue;
    DWORD_PTR prev = 0;
    if (!PinToCore(core, &prev)) continue;
    uint64_t a = 0, m = 0;
    const bool read = ReadMsr(kMsrAperf, &a) && ReadMsr(kMsrMperf, &m);
    Unpin(prev);
    if (!read) continue;
    const uint64_t da = a - a0[core];
    const uint64_t dm = m - m0[core];
    double v = 0.0;
    if (!DecodePerCoreMHz(da, dm, baseClockMHz_, &v)) continue;
    perCoreMHz_[core] = v;
    perCoreStampMs_[core] = GetTickCount64();
    primed_[core] = 1;
    prevAperf_[core] = a;
    prevMperf_[core] = m;
    ++ok;
  }
  return ok;
}

// ────────────────────────────── 采样 ──────────────────────────────

void CpuRing0Provider::Sample(Snapshot& snap) {
  if (!available_) return;
  auto taken = [&snap](size_t slot) {
    return slot != size_t(-1) && slot < snap.metrics.size() && snap.metrics[slot].status == Status::Ok;
  };
  auto setOk = [&snap](size_t slot, double value, std::string text) {
    if (slot == size_t(-1) || slot >= snap.metrics.size()) return;
    auto& r = snap.metrics[slot];
    r.value = value;
    r.status = Status::Ok;
    r.detail = std::move(text);
  };
  auto setNa = [&snap](size_t slot, std::string text) {
    if (slot == size_t(-1) || slot >= snap.metrics.size()) return;
    auto& r = snap.metrics[slot];
    r.value = 0.0;
    r.status = Status::Unavailable;
    r.detail = std::move(text);
  };

  // 1) 每核频率：每轮钉 kCoresPerTick 个核，跨轮差分（无需 Sleep）。
  if (perCoreSlot_ != size_t(-1)) {
    const unsigned okThisTick = SamplePerCoreBatch();
    snap.perCoreFreqMHz.assign(coreCount_, 0.0);
    snap.perCoreDetail.assign(coreCount_, std::string());
    const uint64_t now = GetTickCount64();
    unsigned withValue = 0;
    double sum = 0.0, lo = 0.0, hi = 0.0, oldestS = 0.0;
    for (unsigned i = 0; i < coreCount_; ++i) {
      const double v = perCoreMHz_[i];
      snap.perCoreFreqMHz[i] = v;
      if (v <= 0.0) {
        snap.perCoreDetail[i] = "未采样";
        continue;
      }
      const double ageS = static_cast<double>(now - perCoreStampMs_[i]) / 1000.0;
      snap.perCoreDetail[i] = (ageS < 0.3) ? "本轮"
                                           : (text::formatDouble(ageS, 1) + " s 前");
      if (withValue == 0) {
        lo = hi = v;
      } else {
        lo = (std::min)(lo, v);
        hi = (std::max)(hi, v);
      }
      sum += v;
      oldestS = (std::max)(oldestS, ageS);
      ++withValue;
    }
    if (withValue == 0) {
      setNa(perCoreSlot_, "APERF/MPERF 尚无两个采样点（第一轮只建基线）；每轮钉 " +
                              std::to_string(kCoresPerTick) + " 核轮换，" +
                              text::formatDouble(windowMs_ / 1000.0, 1) + " s 一轮");
    } else {
      const std::string text =
          "APERF/MPERF 跨轮差分（每轮钉 " + std::to_string(kCoresPerTick) + " 核轮换，窗口 " +
          text::formatDouble(windowMs_ / 1000.0, 1) + " s）；本值取 " + std::to_string(withValue) +
          "/" + std::to_string(coreCount_) + " 个核的平均（范围 " + text::formatDouble(lo, 0) + "–" +
          text::formatDouble(hi, 0) + " MHz，最旧 " + text::formatDouble(oldestS, 1) + " s，本轮新采 " +
          std::to_string(okThisTick) + " 核）";
      setOk(perCoreSlot_, sum / static_cast<double>(withValue), text);
    }
  }

  // 2) 封装温度（ADL2 已覆盖时不抢；这是 AMD 无 ADL2 / Intel 的唯一免驱来源）。
  if (tempSlot_ != size_t(-1) && !taken(tempSlot_)) {
    if (amd_) {
      double c = 0.0;
      uint32_t raw = 0;
      if (ReadAmdPackageTemp(&c, &raw)) {
        setOk(tempSlot_, c, "来源：SMN THM_TCON_CUR_TMP（0x00059800，读数 " +
                                std::to_string((raw >> 21) & 0x7FFu) +
                                " → (读数/8)−49 °C，Ring0）；注意这是 Tctl 口径，本机实测比 ADL2 "
                                "TEMPERATURE_CPU 高 10–20 °C，两者不可直接比较");
      } else {
        setNa(tempSlot_, "SMN 温度读数不可用（0 或超出合理区间；Ring0 THM_TCON_CUR_TMP）");
      }
    } else {
      uint64_t status = 0, target = 0;
      double c = 0.0;
      if (ReadMsr(kMsrPkgThermStatus, &status) && ReadMsr(kMsrTempTarget, &target) &&
          DecodeIntelPackageTemp(target, status, &c)) {
        const unsigned tjMax = static_cast<unsigned>((target >> 16) & 0xFFu);
        const unsigned readout = static_cast<unsigned>((status >> 16) & 0x7Fu);
        setOk(tempSlot_, c, "来源：MSR PACKAGE_THERM_STATUS（TjMax " + std::to_string(tjMax) +
                                " − 读数 " + std::to_string(readout) + "，Ring0）");
      } else {
        setNa(tempSlot_, "读不到 MSR PACKAGE_THERM_STATUS / TEMPERATURE_TARGET（Ring0）");
      }
    }
  }

  // 3) 核心电压（ADL2 没有这一项，是本 provider 在 AMD 上的独有价值）。
  if (voltSlot_ != size_t(-1) && !taken(voltSlot_)) {
    if (!amd_) {
      setNa(voltSlot_, "Intel 核心电压需要 SVID 表且各代口径不同，M1 不提供（不猜）");
    } else {
      double v = 0.0;
      uint32_t off = 0, raw = 0, tfn = 0;
      std::string why;
      if (ReadAmdCoreVoltage(&v, &off, &raw, &tfn, &why)) {
        char buf[192]{};
        std::snprintf(buf, sizeof(buf),
                      "来源：SMN SVI2 遥测 0x%05X（1.55 V − 6.25 mV × vid，Ring0；型号 0x%02X）",
                      off, amdModel_);
        setOk(voltSlot_, v, buf);
      } else {
        setNa(voltSlot_, why);
      }
    }
  }

  // 4) 封装功耗（ADL2 未覆盖时才用 MSR 能量计数器差分）。
  if (powerSlot_ != size_t(-1) && !taken(powerSlot_)) {
    uint64_t raw = 0;
    if (!ReadMsr(amd_ ? kMsrPkgEnergyStat : kMsrRaplPkgEnergy, &raw)) {
      setNa(powerSlot_, "读不到封装能量计数器（MSR）");
    } else {
      const uint64_t now = GetTickCount64();
      const bool have = haveEnergy_ && now > lastEnergyTick_;
      const uint64_t prevEnergy = lastEnergy_;
      const uint64_t prevTick = lastEnergyTick_;
      lastEnergy_ = raw;
      lastEnergyTick_ = now;
      haveEnergy_ = true;
      if (!have) {
        setNa(powerSlot_, "封装能量计数器已建基线，下一轮给功耗（MSR 差分）");
      } else {
        uint32_t unit = pwrUnitRaw_;
        if (!amd_) {
          uint64_t u = 0;
          if (ReadMsr(kMsrRaplPwrUnit, &u)) unit = static_cast<uint32_t>(u);
        }
        const unsigned esu = (unit >> 8) & 0x1Fu;
        const double dt = static_cast<double>(now - prevTick) / 1000.0;
        const double delta = static_cast<double>(raw - prevEnergy);
        const double watts = delta * EnergyUnitJoules(unit) / dt;
        if (watts >= kMinWatts && watts <= kMaxWatts) {
          setOk(powerSlot_, watts,
                "来源：MSR " + std::string(amd_ ? "PKG_ENERGY_STAT" : "RAPL PKG_ENERGY_STATUS") +
                    " 差分（ESU=" + std::to_string(esu) + "，窗口 " + text::formatDouble(dt, 1) +
                    " s，Ring0）");
        } else {
          char buf[200]{};
          std::snprintf(buf, sizeof(buf),
                        "能量差分换算得到 %.6g W（ESU=%u，Δ=%llu，%.1f s），超出 0.3–250 W 合理区间，"
                        "判为读不到而非显示可疑值",
                        watts, esu, static_cast<unsigned long long>(raw - prevEnergy), dt);
          setNa(powerSlot_, buf);
        }
      }
    }
  }
}

// ────────────────────────────── 诊断 ──────────────────────────────

CpuRing0Provider::Diag CpuRing0Provider::Probe() {
  Diag d;
  d.amd = amd_;
  d.devicePath = PawnIoDevice::Instance()->devicePath();
  d.driverVersion = ProbePawnIo().version;
  d.moduleLoaded = module_.loaded();
  d.modulePath = module_.path();
  if (!available_) {
    d.note = initDetail_.empty() ? "Ring0 provider 未接线（见 [init] 段）" : initDetail_;
    return d;
  }
  d.perCoreTotal = coreCount_;
  d.smnTempOk = ReadAmdPackageTemp(&d.smnTempC, &d.smnTempRaw);
  d.voltageOk = ReadAmdCoreVoltage(&d.voltageV, &d.voltageOffset, &d.voltageRaw, &d.tfnRaw,
                                   &d.voltageNote);
  uint64_t unit = 0;
  if (ReadMsr(amd_ ? kMsrPwrUnit : kMsrRaplPwrUnit, &unit)) {
    d.pwrUnitRaw = static_cast<uint32_t>(unit);
  }
  uint64_t e0 = 0;
  if (ReadMsr(amd_ ? kMsrPkgEnergyStat : kMsrRaplPkgEnergy, &e0)) {
    const ULONGLONG t0 = GetTickCount64();
    Sleep(500);
    uint64_t e1 = 0;
    if (ReadMsr(amd_ ? kMsrPkgEnergyStat : kMsrRaplPkgEnergy, &e1)) {
      const double dt = static_cast<double>(GetTickCount64() - t0) / 1000.0;
      const double watts = static_cast<double>(e1 - e0) * EnergyUnitJoules(d.pwrUnitRaw) / dt;
      if (watts >= kMinWatts && watts <= kMaxWatts) {
        d.pkgEnergyOk = true;
        d.pkgPowerW = watts;
      }
    }
  }
  d.perCoreSampled = SweepAllCores(200);
  double lo = 0.0, hi = 0.0;
  unsigned n = 0;
  for (unsigned i = 0; i < coreCount_; ++i) {
    if (perCoreMHz_[i] <= 0.0) continue;
    if (n == 0) {
      lo = hi = perCoreMHz_[i];
    } else {
      lo = (std::min)(lo, perCoreMHz_[i]);
      hi = (std::max)(hi, perCoreMHz_[i]);
    }
    ++n;
  }
  d.perCoreMinMHz = lo;
  d.perCoreMaxMHz = hi;
  d.smnTempUsed = (tempSlot_ != size_t(-1));
  if (d.perCoreSampled < coreCount_) {
    d.note = "部分核未能算出频率（钉核失败或计数器未前进）：" + std::to_string(d.perCoreSampled) + "/" +
             std::to_string(coreCount_);
  }
  return d;
}

}  // namespace wpm::sensors
