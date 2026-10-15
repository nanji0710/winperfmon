// SPDX-License-Identifier: MIT
#include "sensors/adl2.h"

#include "config/catalog.h"
#include "core/text.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wpm::sensors {
namespace {

constexpr size_t kNoSlot = static_cast<size_t>(-1);
constexpr int kAdlMaxPath = 256;
constexpr int kAdlPMLogMaxSensors = 256;

// ── ADL 结构体（按 ADL SDK 口径手工镜像；不引入、不分发 ADL 头文件） ──────────
// 布局已在本机逐字段打印验证（首发偏移与内容都对得上）。
struct AdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[kAdlMaxPath];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;  // AMD 的 bug：本机报 0x03EA，不可信，必须从 PNP/UDID 解析
    char strAdapterName[kAdlMaxPath];
    char strDisplayName[kAdlMaxPath];
    int iPresent;
    int iExist;
    char strDriverPath[kAdlMaxPath];
    char strDriverPathExt[kAdlMaxPath];
    char strPNPString[kAdlMaxPath];
    int iOSDisplayIndex;
};

struct ADLSingleSensorData {
    int supported;
    int value;
};

struct ADLPMLogDataOutput {
    int iSize;
    ADLSingleSensorData sensors[kAdlPMLogMaxSensors];
};

using PFN_Create = int (*)(void* (*)(int), int, void**);
using PFN_Destroy = int (*)(void*);
using PFN_NumAdapters = int (*)(void*, int*);
using PFN_AdapterInfo = int (*)(void*, AdapterInfo*, int);
using PFN_QueryPMLog = int (*)(void*, int, ADLPMLogDataOutput*);

// ── PMLog 传感器索引（ADL 枚举口径） ────────────────────────────────────────
// ★ = 本机（Ryzen 7 H 255 / Radeon 780M）用负载前后对比实测确证过语义；
// 其余只来自 ADL 枚举命名，本机不受支持，因此不会被读出来（会被 supported 挡掉）。
enum : int {
    kClkGfx = 1,            // ★ GFX 时钟 MHz（800 空闲 → 2437 满载）
    kClkMem = 2,            // ★ 显存/内存时钟 MHz（APU 上就是共享系统内存时钟）
    kClkSoc = 3,            // ★ SoC 时钟 MHz
    kClkVcn = 7,            // ★ 视频编解码时钟 MHz
    kFanRpm = 14,           //   风扇转速 RPM（本机不支持）
    kFanPercent = 15,       //   风扇百分比 %（本机不支持）
    kSocVoltage = 16,       // ★ SoC 电压 mV
    kSocPower = 17,         // ★ SoC 功耗 W
    kSocCurrent = 18,       // ★ SoC 电流 A
    kActivityGfx = 19,      // ★ GPU 占用 %（0 空闲 → 98 满载；本机唯一干净的占用信号）
    kGfxVoltage = 21,       // ★ GFX 电压 mV
    kAsicPower = 23,        // ★ ASIC 功耗 W（APU 上表现为整片功耗）
    kTempHotspot = 27,      //   热点温度 °C（本机不支持）
    kTempGfx = 28,          // ★ GFX 温度 °C（43 空闲 → 67 满载）
    kTempSoc = 29,          // ★ SoC 温度 °C
    kGfxPower = 30,         // ★ GFX 供电轨功耗 W
    kGfxCurrent = 31,       // ★ GFX 电流 A
    kTempCpu = 32,          // ★ CPU 温度 °C（43 → 83；与 ACPI CPUZ 热区交叉验证一致）
    kCpuPower = 33,         // ★ CPU 功耗 W（本机仅 0–6 W，与 8 线程负载不符，语义待考）
    kClkCpu = 34,           // ★ CPU 时钟 MHz（3775 → 4625）
    kThrottlerStatus = 35,  // ★ 降频状态位域（需要位→文案映射，M1 不采用）
};

// ADL 要求调用方提供分配器；传 nullptr 会得到 rc=-11。
void* AdlMalloc(int bytes) { return std::malloc(static_cast<size_t>(bytes)); }

size_t SlotOf(std::string_view id) {
    const auto cat = wpm::config::MetricCatalog();
    for (size_t i = 0; i < cat.size(); ++i) {
        if (cat[i].id == id) return i;
    }
    return kNoSlot;
}

// 从 "PCI\VEN_1002&DEV_1900&..." 这类字符串里取 key 后面的十六进制数
uint32_t ParseHexAfter(std::string_view s, std::string_view key) {
    const size_t p = s.find(key);
    if (p == std::string_view::npos) return 0;
    const size_t begin = p + key.size();
    uint32_t v = 0;
    size_t i = begin;
    while (i < s.size() && i - begin < 8) {
        const char c = s[i];
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16u + static_cast<uint32_t>(d);
        ++i;
    }
    return v;
}

// 同一物理 GPU 会被 ADL 报成多条，差别只在 PNP 末尾的 "&NN"（本机 &02..&05）
std::string DedupKey(std::string_view pnp) {
    if (pnp.size() >= 3 && pnp[pnp.size() - 3] == '&') {
        auto hex = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        if (hex(pnp[pnp.size() - 2]) && hex(pnp[pnp.size() - 1])) {
            return std::string(pnp.substr(0, pnp.size() - 3));
        }
    }
    return std::string(pnp);
}

std::string Fmt(double v, int prec) { return wpm::text::formatDouble(v, prec); }

}  // namespace

struct Adl2Provider::Impl {
    HMODULE lib = nullptr;
    void* ctx = nullptr;
    PFN_Create create = nullptr;
    PFN_Destroy destroy = nullptr;
    PFN_NumAdapters numAdapters = nullptr;
    PFN_AdapterInfo adapterInfo = nullptr;
    PFN_QueryPMLog queryPMLog = nullptr;
};

Adl2Provider::Adl2Provider() = default;

Adl2Provider::~Adl2Provider() {
    if (!impl_) return;
    if (impl_->ctx && impl_->destroy) impl_->destroy(impl_->ctx);
    if (impl_->lib) FreeLibrary(impl_->lib);
}

void Adl2Provider::SetGpuContext(const std::vector<GpuAdapter>& adapters, int resolvedIndex) {
    gpu_ = adapters;
    resolved_ = resolvedIndex;
}

bool Adl2Provider::Init(std::string& detail) {
    impl_ = std::make_unique<Impl>();

    // 1) 只在系统目录里找 AMD 显示驱动自带的 ADL2 库（绝不随包分发）
    wchar_t dir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        detail = "无法取得系统目录（GetSystemDirectoryW 失败）";
        initDetail_ = detail;
        return false;
    }
    std::wstring wpath(dir, n);
    wpath += L"\\atiadlxx.dll";
    libPath_ = wpm::text::toUtf8(wpath);

    impl_->lib = LoadLibraryW(wpath.c_str());
    if (impl_->lib == nullptr) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(GetLastError()));
        detail = "未找到 " + libPath_ + "（AMD 显卡驱动未安装？GetLastError=" + buf + "）";
        initDetail_ = detail;
        return false;
    }
    libLoaded_ = true;

    // 2) 取入口点。任一必需导出缺失就整体放弃（避免半可用状态）
    impl_->create = reinterpret_cast<PFN_Create>(
        reinterpret_cast<void*>(GetProcAddress(impl_->lib, "ADL2_Main_Control_Create")));
    impl_->destroy = reinterpret_cast<PFN_Destroy>(
        reinterpret_cast<void*>(GetProcAddress(impl_->lib, "ADL2_Main_Control_Destroy")));
    impl_->numAdapters = reinterpret_cast<PFN_NumAdapters>(
        reinterpret_cast<void*>(GetProcAddress(impl_->lib, "ADL2_Adapter_NumberOfAdapters_Get")));
    impl_->adapterInfo = reinterpret_cast<PFN_AdapterInfo>(
        reinterpret_cast<void*>(GetProcAddress(impl_->lib, "ADL2_Adapter_AdapterInfo_Get")));
    impl_->queryPMLog = reinterpret_cast<PFN_QueryPMLog>(
        reinterpret_cast<void*>(GetProcAddress(impl_->lib, "ADL2_New_QueryPMLogData_Get")));
    if (!impl_->create || !impl_->destroy || !impl_->numAdapters || !impl_->adapterInfo ||
        !impl_->queryPMLog) {
        detail = libPath_ + " 缺少必需的 ADL2 导出（驱动版本过旧？）";
        initDetail_ = detail;
        return false;
    }

    // 3) 建立 ADL 上下文。第一个参数必须是 malloc 回调。
    const int crc = impl_->create(&AdlMalloc, /*enumConnectedAdapters=*/1, &impl_->ctx);
    if (crc != 0 || impl_->ctx == nullptr) {
        detail = "ADL2_Main_Control_Create 失败 rc=" + std::to_string(crc);
        initDetail_ = detail;
        return false;
    }

    // 4) 枚举适配器（同一物理 GPU 会有多条，先全收再按 PNP 去重）
    int count = 0;
    if (impl_->numAdapters(impl_->ctx, &count) != 0 || count <= 0) {
        detail = "ADL2_Adapter_NumberOfAdapters_Get 未返回任何适配器";
        initDetail_ = detail;
        return false;
    }
    if (count > 64) count = 64;  // 健全性上限

    std::vector<AdapterInfo> arr(static_cast<size_t>(count));
    for (auto& a : arr) {
        std::memset(&a, 0, sizeof(a));
        a.iSize = static_cast<int>(sizeof(AdapterInfo));
    }
    const int irc = impl_->adapterInfo(impl_->ctx, arr.data(),
                                       count * static_cast<int>(sizeof(AdapterInfo)));
    if (irc != 0) {
        detail = "ADL2_Adapter_AdapterInfo_Get 失败 rc=" + std::to_string(irc);
        initDetail_ = detail;
        return false;
    }

    for (int i = 0; i < count; ++i) {
        const AdapterInfo& a = arr[static_cast<size_t>(i)];
        AdlAdapter ad;
        ad.adlIndex = a.iAdapterIndex;
        ad.bus = a.iBusNumber;
        ad.device = a.iDeviceNumber;
        ad.function = a.iFunctionNumber;
        ad.name = a.strAdapterName;
        ad.pnp = a.strPNPString;
        // iVendorID 不可信（AMD bug），一律从 PNP / UDID 里解析
        ad.vendorId = ParseHexAfter(ad.pnp, "VEN_");
        ad.deviceId = ParseHexAfter(ad.pnp, "DEV_");
        if (ad.vendorId == 0) ad.vendorId = ParseHexAfter(a.strUDID, "VEN_");
        if (ad.deviceId == 0) ad.deviceId = ParseHexAfter(a.strUDID, "DEV_");
        if (a.iExist != 0 || a.iPresent != 0) raw_.push_back(std::move(ad));
    }

    std::vector<std::string> seen;
    for (const AdlAdapter& a : raw_) {
        std::string key;
        if (!a.pnp.empty()) {
            key = DedupKey(a.pnp);
        } else {
            // 驱动没填 PNP 时不敢按名字合并（两台同名独显会撞车），退回总线三元组
            key = "#" + std::to_string(a.bus) + ":" + std::to_string(a.device) + ":" +
                  std::to_string(a.function) + "|" + a.name;
        }
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        dedup_.push_back(a);
    }

    // 5) 与 DXGI 口径对齐（按 VEN/DEV 匹配），得出每个 ADL 适配器对应的枚举下标
    for (AdlAdapter& a : dedup_) {
        for (const GpuAdapter& g : gpu_) {
            if (g.vendorId == a.vendorId && g.deviceId == a.deviceId) {
                a.matchedGpuIndex = g.index;
                break;
            }
        }
    }

    // 6) 绑定到 Collector 解析出的那个枚举下标
    integrated_ = false;
    for (const GpuAdapter& g : gpu_) {
        if (g.index == resolved_) integrated_ = g.integrated;
    }
    if (resolved_ < 0) {
        selNote_ = "gpu.selectedIndex 解析结果为 -1（没有可用的免驱 GPU 遥测目标），未绑定 ADL 适配器";
    } else {
        for (const AdlAdapter& a : dedup_) {
            if (a.matchedGpuIndex == resolved_) {
                bound_ = a.adlIndex;
                selNote_ = "已绑定 ADL 适配器 #" + std::to_string(bound_) + "（" + a.name + "）";
                break;
            }
        }
        if (bound_ < 0) {
            selNote_ = "枚举下标 " + std::to_string(resolved_) +
                       " 在 ADL 适配器列表里没有 VEN/DEV 匹配项，未绑定；"
                       "**不会改用其它适配器的读数**";
        }
    }
    if (bound_ < 0) {
        detail = selNote_;
        initDetail_ = detail;
        return false;
    }

    // 7) 取一次支持列表（只认 PMLog 每项的 supported 字段）
    ADLPMLogDataOutput d{};
    d.iSize = static_cast<int>(sizeof(ADLPMLogDataOutput));
    const int qrc = impl_->queryPMLog(impl_->ctx, bound_, &d);
    if (qrc != 0) {
        detail = "ADL2_New_QueryPMLogData_Get 失败 rc=" + std::to_string(qrc) +
                 "（驱动不支持 PMLog？）";
        initDetail_ = detail;
        available_ = false;
        return false;
    }
    for (int i = 1; i < kAdlPMLogMaxSensors; ++i) {
        if (d.sensors[i].supported != 0) supported_.push_back(i);
    }

    // 8) 建立绑定表。只绑定"语义已确证"的索引；未绑定的指标保持 Ring0/PresentMon 的
    //    既有说明，不会被这里含糊地改成"已支持"。
    const bool apu = integrated_;
    BindSensor(kActivityGfx, "gpu.usage", 0.0, 100.0, "来源：ADL2 PMLog INFO_ACTIVITY_GFX");
    BindSensor(kClkGfx, "gpu.freq.core", 1.0, 10000.0, "来源：ADL2 PMLog CLK_GFXCLK");
    BindSensor(kClkMem, "gpu.freq.mem", 1.0, 20000.0,
               apu ? "来源：ADL2 PMLog CLK_MEMCLK（APU 上即共享系统内存时钟）"
                   : "来源：ADL2 PMLog CLK_MEMCLK");
    BindSensor(kTempGfx, "gpu.temp.core", 1.0, 125.0, "来源：ADL2 PMLog TEMPERATURE_GFX");
    BindSensor(kTempHotspot, "gpu.temp.hotspot", 1.0, 125.0,
               "来源：ADL2 PMLog TEMPERATURE_HOTSPOT");
    if (apu) {
        // 核显：ADL2 的 GFX_POWER 在本机实测**不是** GPU 独占功耗——8 线程纯 CPU
        // 满载（GPU 占用 1–8%、GFXCLK 恒 800 MHz）时它照样升到 24–34 W，
        // 与 ASIC_POWER（32–33 W）几乎重合。把它当成"GPU 功耗"来显示，
        // 用户会在 GPU 空载时看到 30 W，属于会被误读的假数据 → 明确不提供。
        ClaimFixed("gpu.power",
                   "核显与 CPU 共享供电轨：ADL2 GFX_POWER 在 GPU 空载、CPU 满载时仍报 24–34 W，"
                   "无法作为 GPU 功耗（整片功耗见 cpu.power.package）");
    } else {
        BindSensor(kGfxPower, "gpu.power", 0.0, 1000.0, "来源：ADL2 PMLog GFX_POWER");
    }
    BindSensor(kFanRpm, "gpu.fan.rpm", 1.0, 30000.0, "来源：ADL2 PMLog FAN_RPM",
               /*zeroIsMissing=*/true);
    BindSensor(kFanPercent, "gpu.fan.percent", 0.0, 100.0, "来源：ADL2 PMLog FAN_PERCENTAGE");
    if (apu) {
        // APU 上 CPU 与 GPU 同 die：AMD 的 ASIC_POWER 报的就是整片功耗，
        // 与业界 "CPU Package Power" 在 APU 上的口径一致。
        BindSensor(kTempCpu, "cpu.temp.package", 1.0, 125.0,
                   "来源：ADL2 PMLog TEMPERATURE_CPU（AMD PM 表，免驱免管理员）");
        BindSensor(kAsicPower, "cpu.power.package", 0.0, 1000.0,
                   "来源：ADL2 PMLog ASIC_POWER（APU 上即整片功耗，含 GPU 与 uncore；"
                   "实测 CPU 满载 32–33 W、GPU 满载 42 W）");
        BindSensor(kSocPower, "cpu.power.soc", 0.0, 1000.0,
                   "来源：ADL2 PMLog SOC_POWER（与 SOC_VOLTAGE×SOC_CURRENT 自洽："
                   "0.77 V × 2 A ≈ 1.5 W）");
        // CPU 当前频率：有它，CpuNativeProvider 才能给出与任务管理器同口径的占用率，
        // 从而整机不必加载 PDH 计数器表（省下 4.3 MB 私有提交，见 dev-plan 内存一节）。
        // 实测与 PDH % Processor Performance × 基频一致：3775 MHz 空闲 → 4625 MHz 满载。
        BindSensor(kClkCpu, "cpu.freq.avg", 100.0, 10000.0,
                   "来源：ADL2 PMLog CLK_CPUCLK（AMD PM 表，免驱免管理员）");
    }

    // 显存用量：**默认不启用**。ADL2 的两个"用量"导出（Dedicated / Shared）实测
    // 首次调用会把性能计数器表映射进进程（+4.3 MB 私有提交、+280 ms），而本构建
    // 的全部努力就是为了不加载那张表；况且 GPU 满载时它恒定报 391 MB（疑为保留量，
    // 不是实时用量）。详见 docs/amd-facts.md 的 ADL2 一节。
    ClaimFixed("gpu.vram.used",
               "默认不启用：ADL2 显存用量导出首次调用即让性能计数器表常驻（+4.3 MB），"
               "且实测 GPU 满载时数值恒定，疑非实时用量");
    ClaimFixed("gpu.vram.usage", "同上：显存占用率的分母/分子口径同样存疑，M1 不提供");

    // 剩余 GPU 指标：ADL2 后端给不出值（索引口径未确证 / 只有位域）。
    // 明确认领并写明原因，而不是让它们显示成"需要厂商 GPU 库"。
    ClaimFixed("gpu.temp.mem",
               "ADL2 PMLog 的显存温度索引口径未确证（本机不支持），M1 不提供");
    ClaimFixed("gpu.throttle.reason",
               "ADL2 只给降频状态位域（THROTTLER_STATUS），需要位→文案映射，M1 未接入");
    ClaimFixed("gpu.enc.usage", "ADL2 PMLog 编码器占用索引口径未确证，M1 不提供");
    ClaimFixed("gpu.dec.usage", "ADL2 PMLog 解码器占用索引口径未确证，M1 不提供");

    available_ = true;
    detail = selNote_ + "；支持 " + std::to_string(supported_.size()) + " 项传感器";
    initDetail_ = detail;
    return true;
}

void Adl2Provider::BindSensor(int sens, const char* metricId, double lo, double hi,
                              std::string note, bool zeroIsMissing) {
    const size_t slot = SlotOf(metricId);
    if (slot == kNoSlot) return;  // 目录里没有这个 id：直接不认领，避免写越界
    Bind b;
    b.sens = sens;
    b.slot = slot;
    b.lo = lo;
    b.hi = hi;
    b.note = std::move(note);
    b.zeroIsMissing = zeroIsMissing;
    binds_.push_back(std::move(b));
    if (std::find(slots_.begin(), slots_.end(), slot) == slots_.end()) slots_.push_back(slot);
}

void Adl2Provider::ClaimFixed(const char* metricId, std::string why) {
    BindSensor(/*sens=*/0, metricId, 0.0, 0.0, std::move(why));
}

bool Adl2Provider::Claims(std::string_view metricId) const {
    const size_t slot = SlotOf(metricId);
    if (slot == kNoSlot) return false;
    for (const Bind& b : binds_) {
        if (b.sens <= 0 || b.slot != slot) continue;
        // 必须同时"驱动在本适配器上真的报了这个传感器"，否则等于空认领：
        // Collector 会据此决定不再创建 PDH 兜底，那样 cpu.freq.avg 会变成无人提供
        if (std::find(supported_.begin(), supported_.end(), b.sens) != supported_.end()) {
            return true;
        }
    }
    return false;
}

void Adl2Provider::MarkUnavailable(Snapshot& snap, const std::string& reason) const {
    for (const Bind& b : binds_) {
        if (b.sens <= 0) continue;  // 固定原因的槽位不受读取失败影响
        Reading& r = snap.metrics[b.slot];
        r.status = Status::Unavailable;
        r.detail = reason;
    }
}

void Adl2Provider::Sample(Snapshot& snap) {
    if (!available_ || !impl_ || impl_->ctx == nullptr || bound_ < 0) return;

    ADLPMLogDataOutput d{};
    d.iSize = static_cast<int>(sizeof(ADLPMLogDataOutput));
    const int rc = impl_->queryPMLog(impl_->ctx, bound_, &d);
    if (rc != 0) {
        // 读失败不保留旧值：宁可显示"读不到"，也不显示可能已经失效的数字
        MarkUnavailable(snap, "ADL2_New_QueryPMLogData_Get 失败 rc=" + std::to_string(rc));
        return;
    }

    for (const Bind& b : binds_) {
        Reading& r = snap.metrics[b.slot];
        if (b.sens <= 0) {
            // 认领了但本后端给不出值：原因已在 note 里写清
            r.status = Status::Unavailable;
            r.detail = b.note;
            continue;
        }
        const ADLSingleSensorData& s = d.sensors[b.sens];
        if (s.supported == 0) {
            r.status = Status::Unavailable;
            r.detail = "ADL2 未在本适配器上报告该传感器（索引 " + std::to_string(b.sens) + "）";
            continue;
        }
        if (b.zeroIsMissing && s.value == 0) {
            r.status = Status::Unavailable;
            r.detail = b.note + "：驱动报告 0，按「无数据」处理（不代表 0）";
            continue;
        }
        const double v = static_cast<double>(s.value);
        if (v < b.lo || v > b.hi) {
            r.status = Status::Unavailable;
            r.detail = "ADL2 索引 " + std::to_string(b.sens) + " 读数 " + std::to_string(s.value) +
                       " 超出合理区间 [" + Fmt(b.lo, 0) + ", " + Fmt(b.hi, 0) + "]，已丢弃";
            continue;
        }
        r.value = v;
        r.status = Status::Ok;
        r.detail = b.note;
    }

}

bool Adl2Provider::PeekPMLog(std::vector<std::pair<int, int>>& out) const {
    out.clear();
    if (!impl_ || impl_->ctx == nullptr || !impl_->queryPMLog) return false;
    int idx = bound_;
    if (idx < 0) {
        if (dedup_.empty()) return false;
        idx = dedup_.front().adlIndex;  // 诊断用：未绑定时也看一眼第一条
    }
    ADLPMLogDataOutput d{};
    d.iSize = static_cast<int>(sizeof(ADLPMLogDataOutput));
    if (impl_->queryPMLog(impl_->ctx, idx, &d) != 0) return false;
    for (int i = 0; i < kAdlPMLogMaxSensors; ++i) {
        if (d.sensors[i].supported != 0) out.emplace_back(i, d.sensors[i].value);
    }
    return true;
}

}  // namespace wpm::sensors
