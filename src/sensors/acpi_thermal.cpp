// SPDX-License-Identifier: MIT
#include "sensors/acpi_thermal.h"

#include "config/catalog.h"
#include "core/text.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <string>
#include <vector>

namespace wpm::sensors {
namespace {

constexpr wchar_t kWildcardPath[] =
    L"\\Thermal Zone Information(*)\\High Precision Temperature";

// ACPI 热区域的哨兵值（见 docs/laptop-telemetry-facts.md §T4）：
//   0    = 无数据
//   2732 = 0.0 °C，同样表示"没接传感器"而不是真的 0 度
bool IsSentinel(double raw) {
    return raw <= 0.5 || (raw > 2731.5 && raw < 2732.5);
}

double RawToCelsius(double raw) { return raw / 10.0 - 273.15; }

// 从完整计数器路径里抽出实例名：...Thermal Zone Information(\_TZ.CPUZ)\High... → "\_TZ.CPUZ"
std::string InstanceOf(const std::wstring& path) {
    const size_t l = path.find(L'(');
    const size_t r = path.find(L')', l == std::wstring::npos ? 0 : l);
    if (l == std::wstring::npos || r == std::wstring::npos || r <= l + 1) return {};
    return wpm::text::toUtf8(path.substr(l + 1, r - l - 1));
}

std::string UpperAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

// 只认 ACPI 固件里的通用命名：_TZ.CPUZ / _TZ.GFXZ。
// 其余名字（BATZ/CHGZ/PGTZ...）不参与指标，但会出现在 --probe 里，
// 便于用户回报"我的机器上 CPU 热区叫什么"。
std::string ClassifyZone(const std::string& instance) {
    const std::string up = UpperAscii(instance);
    if (up.find("CPUZ") != std::string::npos) return "cpu";
    if (up.find("GFXZ") != std::string::npos || up.find("GPUZ") != std::string::npos) return "gpu";
    return "other";
}

bool AddCounter(PDH_HQUERY q, const std::wstring& path, PDH_HCOUNTER& out) {
    return PdhAddEnglishCounterW(q, path.c_str(), 0, &out) == ERROR_SUCCESS;
}

}  // namespace

AcpiThermalProvider::~AcpiThermalProvider() {
    if (query_) PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
}

bool AcpiThermalProvider::Init(std::string& detail) {
    const auto cat = config::MetricCatalog();
    for (size_t i = 0; i < cat.size(); ++i) {
        if (cat[i].id == "cpu.temp.package") cpuSlot_ = static_cast<int>(i);
        if (cat[i].id == "gpu.temp.core") gpuSlot_ = static_cast<int>(i);
    }
    if (cpuSlot_ < 0) {
        detail = "指标目录缺少 cpu.temp.package";
        return false;
    }

    // 这里**什么都不枚举**：连"本机有哪些热区"都要等到真要读温度时才知道，
    // 因为仅 PdhExpandWildCardPathW 一次调用就会把性能计数器表映射进进程
    // （本机实测 +4.3 MB 私有提交，不归还）。见 EnsureZones/EnsureQuery。
    slots_.push_back(static_cast<size_t>(cpuSlot_));
    if (gpuSlot_ >= 0) slots_.push_back(static_cast<size_t>(gpuSlot_));

    available_ = true;
    detail = "热区与 PDH 查询均按需建立（高优先级源已覆盖槽位时不付出内存代价）";
    return true;
}

bool AcpiThermalProvider::EnsureZones() const {
    if (zonesReady_) return !cpuPath_.empty() || !gpuPath_.empty();
    zonesReady_ = true;

    // 1) 通配展开，拿到本机所有热区的计数器路径
    DWORD size = 0;
    PdhExpandWildCardPathW(nullptr, kWildcardPath, nullptr, &size, 0);
    if (size == 0) {
        probeNote_ = "本机没有 ACPI 热区域计数器（Thermal Zone Information）";
        return false;
    }
    std::vector<wchar_t> buf(size + 2, 0);
    const PDH_STATUS rc = PdhExpandWildCardPathW(nullptr, kWildcardPath, buf.data(), &size, 0);
    if (rc != ERROR_SUCCESS) {
        probeNote_ = "展开 ACPI 热区域计数器失败";
        return false;
    }

    std::vector<std::wstring> paths;
    for (const wchar_t* p = buf.data(); *p != L'\0'; p += wcslen(p) + 1) paths.emplace_back(p);
    if (paths.empty()) {
        probeNote_ = "本机没有 ACPI 热区域";
        return false;
    }

    // 2) 记录实例名与角色，并挑出候选计数器路径（只解析，不建查询）
    for (const auto& p : paths) {
        ZoneInfo zi;
        zi.instance = InstanceOf(p);
        zi.role = ClassifyZone(zi.instance);
        zones_.push_back(zi);
        if (zi.role == "cpu" && cpuPath_.empty()) cpuPath_ = p;
        else if (zi.role == "gpu" && gpuPath_.empty()) gpuPath_ = p;
    }
    if (cpuPath_.empty() && gpuPath_.empty()) {
        probeNote_ = "未找到 _TZ.CPUZ / _TZ.GFXZ 命名的热区（共 " + std::to_string(zones_.size()) +
                     " 个热区，名字见 --probe）";
        return false;
    }
    return true;
}

bool AcpiThermalProvider::EnsureQuery() {
    if (query_ != nullptr) return true;
    if (cpuPath_.empty() && gpuPath_.empty()) return false;

    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return false;
    query_ = q;
    if (!cpuPath_.empty()) {
        PDH_HCOUNTER c = nullptr;
        if (AddCounter(q, cpuPath_, c)) cpuCounter_ = c;
    }
    if (!gpuPath_.empty()) {
        PDH_HCOUNTER c = nullptr;
        if (AddCounter(q, gpuPath_, c)) gpuCounter_ = c;
    }
    if (cpuCounter_ == nullptr && gpuCounter_ == nullptr) {
        PdhCloseQuery(q);
        query_ = nullptr;
        return false;
    }
    PdhCollectQueryData(q);  // 首次收集只建立基线
    return true;
}

void AcpiThermalProvider::Sample(Snapshot& snap) {
    if (!available_) return;

    // 优先级：更精确的 die 级数据源（如 ADL2 读 AMD PM 表）若已在本轮填好该槽位，
    // 本 provider 不再覆盖。ACPI 热区只有约 1 °C 分辨率、且滞后可达 10 s，
    // 只配当兜底；Collector 因此把更精确的 provider 排在前面。
    auto taken = [&snap](int slot) {
        return slot >= 0 && snap.metrics[static_cast<size_t>(slot)].status == Status::Ok;
    };

    // 短路：本轮要写的槽位若已全被更精确的源填成 Ok，就一个计数器都不必读。
    // 实测读一次 9 个热区约 0.7 ms（逐源耗时见 docs/build-and-run.md），
    // 而结果注定被丢弃——这是"极致轻量化"里最该省掉的那笔钱。
    // 更关键的是：**一次都不读，就永远不会枚举热区、不会建立 PDH 查询**，
    // 省掉 4.3 MB 常驻内存（枚举本身就有代价，见 EnsureZones 注释）。
    const bool wantCpu = cpuSlot_ >= 0 && !taken(cpuSlot_);
    const bool wantGpu = gpuSlot_ >= 0 && !taken(gpuSlot_);
    if (!wantCpu && !wantGpu) return;
    if (!EnsureZones()) {
        if (wantCpu) {
            Reading& r = snap.metrics[static_cast<size_t>(cpuSlot_)];
            r.status = Status::Unavailable;
            r.detail = probeNote_;
        }
        if (wantGpu) {
            Reading& r = snap.metrics[static_cast<size_t>(gpuSlot_)];
            r.status = Status::Unavailable;
            r.detail = probeNote_;
        }
        return;
    }
    const bool haveCpu = wantCpu && !cpuPath_.empty();
    const bool haveGpu = wantGpu && !gpuPath_.empty();
    if (!haveCpu && !haveGpu) return;

    if (!EnsureQuery()) {
        // 明确的失败原因好过留着上一轮的旧值
        if (haveCpu) {
            Reading& r = snap.metrics[static_cast<size_t>(cpuSlot_)];
            r.status = Status::Unavailable;
            r.detail = "ACPI 热区计数器按需创建失败（PDH）";
        }
        if (haveGpu) {
            Reading& r = snap.metrics[static_cast<size_t>(gpuSlot_)];
            r.status = Status::Unavailable;
            r.detail = "ACPI 热区计数器按需创建失败（PDH）";
        }
        return;
    }
    const bool needCpu = haveCpu && cpuCounter_ != nullptr;
    const bool needGpu = haveGpu && gpuCounter_ != nullptr;
    if (!needCpu && !needGpu) return;

    PDH_HQUERY q = static_cast<PDH_HQUERY>(query_);
    PdhCollectQueryData(q);

    auto read = [](void* counter, double& out) -> bool {
        if (!counter) return false;
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(counter), PDH_FMT_DOUBLE, nullptr,
                                        &v) != ERROR_SUCCESS) {
            return false;
        }
        if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return false;
        out = v.doubleValue;
        return true;
    };

    if (firstSample_) {
        firstSample_ = false;
        // 第一帧只有基线：标成等待中，下一帧起给真值
        if (needCpu) {
            Reading& r = snap.metrics[static_cast<size_t>(cpuSlot_)];
            r.status = Status::Stale;
            r.detail = "等待首次采样窗口";
        }
        return;
    }

    if (needCpu) {
        double raw = 0.0;
        Reading& r = snap.metrics[static_cast<size_t>(cpuSlot_)];
        if (read(cpuCounter_, raw) && !IsSentinel(raw)) {
            const double c = RawToCelsius(raw);
            if (c >= 15.0 && c <= 115.0) {
                r.value = c;
                r.status = Status::Ok;
                // 明确标注来源：ACPI 热区域分辨率约 1 °C，不是 DTS 级读数
                r.detail = "来源：ACPI 热区域（约 1 °C 分辨率）";
                if (!cpuMoved_ && cpuLast_ != 0.0 && std::abs(c - cpuLast_) >= 0.5) cpuMoved_ = true;
                cpuLast_ = c;
            } else {
                r.status = Status::Unavailable;
                r.detail = "ACPI 热区域读数超出合理范围";
            }
        } else {
            r.status = Status::Unavailable;
            r.detail = "ACPI 热区域无有效数据（哨兵值 0 / 2732）";
        }
    }

    // GPU 温度只在热区确实接了传感器时才采信：
    // 本机 \_TZ.GFXZ 恒为 25.1 °C（低于任何 GPU 芯片的空闲温度），属"ACPI 表里有、底层没接"，
    // 报出来会让人以为显卡很凉，因此设一个下限，低于它就不认。
    constexpr double kGpuPlausibleFloorC = 30.0;
    if (needGpu) {
        double raw = 0.0;
        Reading& r = snap.metrics[static_cast<size_t>(gpuSlot_)];
        if (read(gpuCounter_, raw) && !IsSentinel(raw)) {
            const double c = RawToCelsius(raw);
            if (c >= kGpuPlausibleFloorC && c <= 115.0) {
                r.value = c;
                r.status = Status::Ok;
                r.detail = "来源：ACPI 热区域（约 1 °C 分辨率）";
            } else if (c >= 15.0) {
                r.status = Status::Unavailable;
                r.detail = "ACPI GPU 热区恒定在 " + wpm::text::formatDouble(c, 1) +
                           " °C，低于合理下限，疑为未接传感器（需要 Ring0/厂商库）";
            }
        }
    }
}

}  // namespace wpm::sensors
