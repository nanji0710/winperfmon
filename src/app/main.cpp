// SPDX-License-Identifier: MIT
// winperfmon 入口（M0 骨架）。
//
// 本阶段目标是先把"免驱可跑通"这条链路打通并可被命令行验收：
//   winperfmon --once      采集一次并把全部指标目录（50 项）的结果打到控制台
//   winperfmon --watch N   连续采样 N 秒（默认 5）
//   winperfmon --probe     输出可提交的传感器探针文本（release-github.md §11）
//   winperfmon --selftest  跑一遍初始化与一次采样，返回码表示成败
//   winperfmon --version
//
// WIN32 子系统的 EXE 默认没有控制台，文本模式下 AttachConsole 到调用它的终端，
// 没有父终端时（双击运行）再 AllocConsole。图形界面在 M3 接入。
#include "winperfmon/version.h"

#include "config/catalog.h"
#include "config/config.h"
#include "config/store.h"
#include "core/text.h"
#include "fps/fps_provider.h"
#include "sensors/acpi_thermal.h"
#include "sensors/adl2.h"
#include "sensors/cpu_native.h"
#include "sensors/cpu_pdh.h"
#include "sensors/cpu_ring0.h"
#include "sensors/gpu_enum.h"
#include "sensors/memory.h"
#include "ui/hud_window.h"
#include "sensors/sensor.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using wpm::config::Config;
using wpm::config::MetricCatalog;
using wpm::sensors::Snapshot;
using wpm::sensors::Status;

FILE* g_out = nullptr;

// QPC 打点：`docs/winperfmon-dev-plan.md` 的冷启动预算（≤300 ms）必须靠分段计时校准，
// 否则"哪个阶段慢"只能靠猜。QueryPerformanceFrequency 每次调用都由内核缓存，开销可忽略。
double NowMs() {
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// 进程进入 wmain 的第一件事就是取它，作为"冷启动"计时原点。
double g_processStartMs = 0.0;

// 工作集（常驻内存）自测：常驻内存预算 ≤15 MB（含 FPS ≤25 MB）是硬指标，
// 在程序内自报可让验收不依赖外部工具，也让回归可脚本化比对。
// 三个数各有用途：工作集 = 预算口径；私有 = 真正"自己吃掉的"提交内存（堆/栈/私有段，
// 不含共享 DLL 页）；峰值 = 抖动上限。只有把它们分开，超标时才知道该优化谁。
struct MemUsage {
    double workingSetMb = -1.0;
    double privateMb = -1.0;
    double peakWorkingSetMb = -1.0;
};
MemUsage ReadMemUsage() {
    MemUsage m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        m.workingSetMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        m.privateMb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        m.peakWorkingSetMb = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
    return m;
}

// 内存分段打点：超标时要知道"哪一阶段吃掉的钱"，否则只能对总数猜。
// 只在诊断路径（--watch 尾部统计 / --probe 的 [timing]）输出，采样热路径零开销。
struct MemMarkEntry {
    std::string name;
    MemUsage mem;
};
std::vector<MemMarkEntry> g_memMarks;
void MemMark(const std::string& name) { g_memMarks.push_back({name, ReadMemUsage()}); }
void MemMark(const char* name) { g_memMarks.push_back({name, ReadMemUsage()}); }

// 输出通道选择：**优先沿用调用方已经接好的标准句柄**。
// 只有句柄无效（双击运行、无父 shell）时才去 AttachConsole/AllocConsole。
// 反例（曾经这么写，导致 `winperfmon --once > out.txt` 永远得到 0 字节）：
// 无条件 AttachConsole + freopen("CONOUT$") 会把父进程重定向到文件/管道的
// stdout 丢掉，再改接到控制台——脚本化验收与 CI 全部失效。
void AttachOrAllocateConsole() {
    const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool hasStdout =
        hOut != nullptr && hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) != FILE_TYPE_UNKNOWN;
    if (hasStdout) {
        // 已经有可用的 stdout（控制台、重定向文件或管道）：原样使用，不碰它。
        g_out = stdout;
        // 控制台默认代码页是 GBK，中文会乱码；对已重定向的句柄设置失败也无害。
        SetConsoleOutputCP(CP_UTF8);
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    FILE* in = nullptr;
    freopen_s(&in, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
    g_out = stdout;
}

std::string StatusText(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Unavailable: return "不可用";
        case Status::NeedDriver: return "需要 Ring0";
        case Status::NeedGpuLib: return "需要厂商 GPU 库";
        case Status::NeedPresentMon: return "需要 PresentMon";
        case Status::Error: return "错误";
        case Status::Stale: return "等待中";
    }
    return "?";
}

// 有效精度：用户在 metrics[] 里覆盖过（>=0）就用用户的，否则用目录默认。
int EffectivePrecision(wpm::config::MetricInfo const& info, const wpm::config::Config& cfg) {
    for (const auto& m : cfg.metrics) {
        if (wpm::text::iequals(m.id, info.id) && m.precision >= 0) return m.precision;
    }
    return info.defaultPrecision < 0 ? 2 : info.defaultPrecision;
}

void FormatValue(const wpm::config::MetricInfo& info, const wpm::sensors::Reading& r,
                 const wpm::config::Config& cfg, std::string& out) {
    if (!r.ok()) {
        out = "--";
        return;
    }
    out = wpm::text::formatDouble(r.value, EffectivePrecision(info, cfg));
}

// ── 采集编排：M0 只有免驱 provider ─────────────────────────────────────────
class Collector {
public:
    Collector() : publisher_(std::make_unique<wpm::sensors::SnapshotPublisher>()) {}

    bool Init(const Config& cfg, std::string& report) {
        const double tInit = NowMs();
        // ① GPU 适配器枚举 + gpu.selectedIndex 解析。
        //    必须显式解析：本机 Win32_VideoController/DXGI 会报出多个虚拟显示适配器
        //    （GameViewer / OrayIddDriver / Honor），盲取第一个会拿到没有遥测的那个。
        gpuAdapters_ = wpm::sensors::EnumerateGpuAdapters();
        gpuSelectedCfg_ = cfg.gpu.selectedIndex;
        gpuSelected_ = wpm::sensors::ResolveSelectedGpu(gpuAdapters_, gpuSelectedCfg_, gpuNote_);
        initTimings_.push_back({"gpu-enum", NowMs() - tInit});
        MemMark("DXGI 枚举后");
        {
            Snapshot& snap = publisher_->Writable();
            snap.gpuAdapterCount = static_cast<int>(gpuAdapters_.size());
            snap.gpuSelectedIndex = gpuSelected_;
            for (const auto& g : gpuAdapters_) {
                if (g.index == gpuSelected_) snap.gpuName = g.name;
            }
            if (snap.gpuName.empty() && !gpuAdapters_.empty()) {
                // 诚实降级：明确说清「没有任何 GPU 在提供读数」，并把本机枚举到的适配器列出来，
                // 避免用户误以为表格里的数字来自其中某一块 GPU。
                std::string list;
                for (const auto& g : gpuAdapters_) {
                    if (!list.empty()) list += " / ";
                    list += g.name;
                }
                snap.gpuName = "（未选中任何带遥测的适配器；本机枚举到：" + list + "）";
            }
        }

        // ② provider 顺序 = 优先级。更精确的 die 级数据源排在前面，
        //    后面的 provider 会让位（AcpiThermalProvider 跳过已被填好的槽位）。
        //    这个顺序同时决定**同一轮内的采样顺序**：adl2 先跑，cpu.native 才能拿到
        //    本轮刚写入的 cpu.freq.avg 做频率加权（见 CpuNativeProvider::Sample）。
        providers_.push_back(std::make_unique<wpm::sensors::MemoryProvider>());
        const bool hasAdl2 = std::any_of(gpuAdapters_.begin(), gpuAdapters_.end(),
                                        [](const wpm::sensors::GpuAdapter& g) {
                                            return g.backend == "ADL2";
                                        });
        if (hasAdl2) {
            auto adl = std::make_unique<wpm::sensors::Adl2Provider>();
            adl->SetGpuContext(gpuAdapters_, gpuSelected_);
            adl2_ = adl.get();
            providers_.push_back(std::move(adl));
        }
        // Ring0（PawnIO）：每核频率、核心电压要靠它；封装温度/功耗只在 ADL2 缺席时兜底
        // （provider 内部用 taken() 让位，所以排在 ADL2 后面即自动让位）。
        {
            auto ring0 = std::make_unique<wpm::sensors::CpuRing0Provider>();
            ring0->SetIntervalMs(static_cast<uint32_t>(cfg.update.sensorIntervalMs));
            ring0_ = ring0.get();
            providers_.push_back(std::move(ring0));
        }

        bool any = false;
        size_t inited = 0;
        auto initPending = [&]() {
            for (; inited < providers_.size(); ++inited) {
                auto& p = providers_[inited];
                std::string detail;
                const double t0 = NowMs();
                const bool ok = p->Init(detail);
                initTimings_.push_back({p->name(), NowMs() - t0});
                MemMark("provider:" + std::string(p->name()));
                if (!ok && std::string(p->name()) == "cpu.ring0") {
                    // Ring0 没打开时，所有 AvRing0 指标的原因都应指向真正的原因（没装驱动 /
                    // 没放官方模块 / 平台不支持），而不是一句笼统的"需要 Ring0 通道"。
                    ring0Note_ = detail.empty() ? "Ring0 通道不可用" : detail;
                }
                if (std::string(p->name()) == "fps" && fps_ != nullptr && !fps_->CaptureReady()) {
                    // 只有"根本采不了"（没装 PresentMon / 配置关闭）才改写成降级原因；
                    // "还没开始采"不算失败，别把 fps.* 写成错误状态。
                    fpsNote_ = fps_->Note();
                }
                report += std::string("  ") + p->name() + ": " + (ok ? "就绪" : "不可用");
                if (!detail.empty()) report += "（" + detail + "）";
                report += "\n";
                any = any || ok;
            }
        };
        // 先初始化内存与 GPU 侧：CPU 提供程序选哪条通道，取决于 ADL2 能否给出当前频率
        initPending();

        // ③ CPU 占用率 / 当前频率。ADL2 能给出当前频率（APU 上的 CLK_CPUCLK）时用原生
        //    计数器算占用率：它不加载 PDH 计数器表，省下 4.3 MB 私有提交且单步更快
        //    （见 docs/winperfmon-dev-plan.md 的常驻内存一节）。拿不到频率时退回 PDH——
        //    % Processor Utility 与任务管理器完全同口径，那张表属于"该花的钱"。
        if (adl2_ != nullptr && adl2_->Claims("cpu.freq.avg")) {
            providers_.push_back(std::make_unique<wpm::sensors::CpuNativeProvider>());
        } else {
            providers_.push_back(std::make_unique<wpm::sensors::CpuPdhProvider>());
        }
        // ACPI 热区兜底：非管理员可读，但分辨率约 1 °C 且滞后可达 10 s，故排在 ADL2 之后。
        // 它连 PDH 查询都是按需建立——被前面的源填满时一次都不建立。
        {
            auto acpi = std::make_unique<wpm::sensors::AcpiThermalProvider>();
            acpi_ = acpi.get();
            providers_.push_back(std::move(acpi));
        }
        // FPS 采集（M2）：PresentMon 子进程 + 逐帧 CSV。排在最后是因为它的采样最贵
        // （要读管道），且它与其它 provider 没有槽位竞争——它是 fps.* 的唯一来源。
        {
            auto fps = std::make_unique<wpm::fps::FpsProvider>();
            fps->SetOptions(cfg.fps);
            fps->SetDiagnosticOnly(probeMode_);
            fps_ = fps.get();
            providers_.push_back(std::move(fps));
        }
        initPending();
        // 未接线的指标统一标注原因，UI 才不会显示成 0
        const double tPub = NowMs();
        Snapshot& snap = publisher_->Writable();
        MarkUnwiredByAvailability(snap);
        FillSysMetrics(snap);
        publisher_->Publish();
        initTimings_.push_back({"首帧发布", NowMs() - tPub});
        // 冷启动 = 进程入口 → 首帧可用快照
        startupMs_ = NowMs() - g_processStartMs;
        return any;
    }

    struct StageMs {
        std::string name;
        double ms = 0.0;
    };
    [[nodiscard]] const std::vector<StageMs>& InitTimings() const { return initTimings_; }
    [[nodiscard]] double StartupMs() const { return startupMs_; }
    [[nodiscard]] double FirstStepMs() const { return firstStepMs_; }
    [[nodiscard]] uint64_t StepCount() const { return stepCount_; }
    [[nodiscard]] double StepAvgMs() const {
        return stepCount_ == 0 ? 0.0 : stepTotalMs_ / static_cast<double>(stepCount_);
    }
    [[nodiscard]] double StepMaxMs() const { return stepMaxMs_; }

    void Step() {
        const double tStep = NowMs();
        Snapshot& snap = publisher_->Writable();
        // 每轮先把上一轮的值清成"未更新"，provider 覆盖自己负责的槽位
        for (size_t i = 0; i < snap.metrics.size(); ++i) {
            auto& r = snap.metrics[i];
            if (r.status == Status::Ok) r.status = Status::Stale;
        }
        LARGE_INTEGER freq{}, now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if (startCounter_ == 0) startCounter_ = now.QuadPart;
        snap.takenAtMs = (now.QuadPart - startCounter_) * 1000 / freq.QuadPart;
        snap.utcMs = static_cast<int64_t>(GetSystemTimeAsFileTimeRaw());

        for (auto& p : providers_) {
            if (!p->Available()) continue;
            const double tProv = NowMs();
            p->Sample(snap);
            // 逐 provider 累计：CPU 预算超标时，必须先知道钱花在谁身上才能优化。
            SampleAcc& acc = sampleTotals_[p->name()];
            const double provMs = NowMs() - tProv;
            acc.totalMs += provMs;
            if (provMs > acc.maxMs) acc.maxMs = provMs;
            ++acc.count;
        }
        FillSysMetrics(snap);
        publisher_->Publish();

        // 采样耗时统计：这是"CPU ≤0.3%"预算的直接证据（1 Hz 下 avg 即占用率‰的 3.6 倍）
        const double stepMs = NowMs() - tStep;
        if (firstStepMs_ < 0.0) firstStepMs_ = stepMs;
        stepTotalMs_ += stepMs;
        ++stepCount_;
        if (stepMs > stepMaxMs_) stepMaxMs_ = stepMs;
    }

    [[nodiscard]] std::shared_ptr<const Snapshot> Latest() const { return publisher_->Latest(); }

    /// 逐 provider 的采样耗时（CPU 预算定位用）。`--probe` 以 `sample.<name>Ms` 输出。
    struct SampleStat {
        std::string name;
        double avgMs = 0.0;
        double maxMs = 0.0;
        uint64_t count = 0;
    };
    [[nodiscard]] std::vector<SampleStat> SampleStats() const {
        std::vector<SampleStat> out;
        out.reserve(sampleTotals_.size());
        for (const auto& [name, acc] : sampleTotals_) {
            SampleStat s;
            s.name = name;
            s.count = acc.count;
            s.avgMs = acc.count == 0 ? 0.0 : acc.totalMs / static_cast<double>(acc.count);
            s.maxMs = acc.maxMs;
            out.push_back(std::move(s));
        }
        return out;
    }

private:
    static uint64_t GetSystemTimeAsFileTimeRaw() {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        // FILETIME 是 1601 起的 100ns；转 Unix 毫秒
        return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
    }

    static void FillSysMetrics(Snapshot& snap) {
        const auto cat = MetricCatalog();
        for (size_t i = 0; i < cat.size(); ++i) {
            auto& r = snap.metrics[i];
            if (cat[i].id == "sys.time") {
                SYSTEMTIME st{};
                GetLocalTime(&st);
                r.value = st.wHour * 3600.0 + st.wMinute * 60.0 + st.wSecond;
                r.status = Status::Ok;
                r.detail.clear();
            } else if (cat[i].id == "sys.session") {
                r.value = static_cast<double>(snap.takenAtMs) / 1000.0;
                r.status = Status::Ok;
                r.detail.clear();
            }
        }
    }

    // 尚未接线的指标：按 availability 给出确切的"为什么读不到"
    void MarkUnwiredByAvailability(Snapshot& snap) const {
        using wpm::config::Availability;
        const auto cat = MetricCatalog();
        for (size_t i = 0; i < cat.size(); ++i) {
            auto& r = snap.metrics[i];
            if (r.status != Status::Unavailable) continue;
            switch (cat[i].availability) {
                case Availability::AvAny:
                    r.detail = "本机未提供";
                    break;
                case Availability::AvRing0:
                    r.status = Status::NeedDriver;
                    // Ring0 通道已经实现（PawnIO + 官方模块），所以原因要说清是"通道没打开"，
                    // 而不是"还没做"：具体状态见 --probe 的 [ring0] 段。
                    r.detail = ring0Note_.empty()
                                   ? "需要 Ring0 通道（PawnIO 驱动 + 官方模块），状态见 --probe 的 [ring0] 段"
                                   : ring0Note_;
                    break;
                case Availability::AvVendorOrRing0:
                    r.status = Status::NeedDriver;
                    r.detail = "需要 Ring0 通道（PawnIO，Intel MSR/RAPL）或厂商 PM 表"
                               "（AMD ADL2），M1 接入";
                    break;
                case Availability::AvGpu:
                    r.status = Status::NeedGpuLib;
                    // 库里明明装着、却提示"需要厂商 GPU 库"是自相矛盾的，
                    // 所以按本机实际情况给出原因（没选中/虚拟适配器/没装库）
                    r.detail = gpuNote_.empty()
                                   ? "需要厂商 GPU 库（NVML/ADL2/IGCL），M1 接入"
                                   : gpuNote_;
                    break;
                case Availability::AvPresentMon:
                    r.status = Status::NeedPresentMon;
                    // FPS provider 已接线：PresentMon 没找到 / 还没开始采集时，
                    // 原因来自它（含找过的路径），而不是一句"需要 PresentMon，M2 接入"。
                    r.detail = fpsNote_.empty() ? "需要 PresentMon，M2 接入" : fpsNote_;
                    break;
            }
        }
    }

public:
    // 供 --probe 使用
    /// --probe 用：诊断模式。目前只影响 FPS provider（不启动 PresentMon 子进程）——
    /// 一条诊断命令不该顺手在你机器上起进程。
    void SetProbeMode(bool on) noexcept { probeMode_ = on; }
    [[nodiscard]] const wpm::sensors::Adl2Provider* Adl() const noexcept { return adl2_; }    [[nodiscard]] const wpm::sensors::AcpiThermalProvider* Acpi() const noexcept { return acpi_; }
    [[nodiscard]] wpm::sensors::CpuRing0Provider* Ring0() const noexcept { return ring0_; }
    [[nodiscard]] const std::string& Ring0Note() const noexcept { return ring0Note_; }
    [[nodiscard]] wpm::fps::FpsProvider* Fps() const noexcept { return fps_; }
    [[nodiscard]] const std::vector<wpm::sensors::GpuAdapter>& GpuAdapters() const noexcept {
        return gpuAdapters_;
    }
    [[nodiscard]] int GpuSelected() const noexcept { return gpuSelected_; }
    [[nodiscard]] int GpuSelectedConfig() const noexcept { return gpuSelectedCfg_; }
    [[nodiscard]] const std::string& GpuNote() const noexcept { return gpuNote_; }

private:
    std::unique_ptr<wpm::sensors::SnapshotPublisher> publisher_;
    std::vector<std::unique_ptr<wpm::sensors::ISensorProvider>> providers_;
    std::vector<wpm::sensors::GpuAdapter> gpuAdapters_;
    int gpuSelectedCfg_ = -1;
    int gpuSelected_ = -1;
    std::string gpuNote_;
    std::string ring0Note_;
    std::string fpsNote_;
    bool probeMode_ = false;
    wpm::sensors::Adl2Provider* adl2_ = nullptr;         // 不拥有，providers_ 持有
    wpm::sensors::CpuRing0Provider* ring0_ = nullptr;    // 不拥有
    wpm::sensors::AcpiThermalProvider* acpi_ = nullptr;  // 不拥有
    wpm::fps::FpsProvider* fps_ = nullptr;               // 不拥有
    int64_t startCounter_ = 0;
    std::vector<StageMs> initTimings_;
    double startupMs_ = 0.0;
    double firstStepMs_ = -1.0;
    double stepTotalMs_ = 0.0;
    double stepMaxMs_ = 0.0;
    uint64_t stepCount_ = 0;

    struct SampleAcc {
        double totalMs = 0.0;
        double maxMs = 0.0;
        uint64_t count = 0;
    };
    // std::map（而非 unordered_map）：输出顺序稳定 = 字母序，脚本化比对才有意义。
    std::map<std::string, SampleAcc> sampleTotals_;
};

void DumpSnapshot(const Snapshot& snap, bool onlyVisible, const Config& cfg) {
    const auto cat = MetricCatalog();
    std::printf("seq=%llu tick=%llu 采集时长=%.1fs\n",
                static_cast<unsigned long long>(snap.seq),
                static_cast<unsigned long long>(snap.tick),
                static_cast<double>(snap.takenAtMs) / 1000.0);
    std::printf("%-22s %-14s %-10s %-12s %s\n", "指标 id", "名称", "数值", "单位", "状态/原因");
    std::printf("%s\n", std::string(88, '-').c_str());
    for (size_t i = 0; i < cat.size(); ++i) {
        if (onlyVisible) {
            bool visible = cat[i].defaultVisible;
            for (const auto& m : cfg.metrics) {
                if (m.id == cat[i].id) visible = m.visible;
            }
            if (!visible) continue;
        }
        const auto& r = snap.at(i);
        std::string val;
        FormatValue(cat[i], r, cfg, val);
        std::string note = StatusText(r.status);
        if (!r.detail.empty()) note += "：" + r.detail;
        std::printf("%-22s %-14s %-10s %-12s %s\n", std::string(cat[i].id).c_str(),
                    std::string(cat[i].defaultLabel).c_str(), val.c_str(),
                    std::string(cat[i].unit).c_str(), note.c_str());
    }
}

void PrintStatusSummary(const Snapshot& snap) {
    int ok = 0, unavail = 0, need = 0, err = 0;
    for (const auto& r : snap.metrics) {
        switch (r.status) {
            case Status::Ok: ++ok; break;
            case Status::Unavailable: ++unavail; break;
            case Status::Error: ++err; break;
            default: ++need; break;
        }
    }
    std::printf("\n汇总：可用 %d / 本机无 %d / 待接入通道 %d / 错误 %d（合计 %zu）\n", ok, unavail,
                need, err, snap.metrics.size());
}

// ── HUD 窗口（M0.5 可见雏形）──────────────────────────────────────────────
// 这一层只做三件事：把配置翻译成绘制参数、把快照翻译成「行」、把窗口跑起来。
// 采样与降级判断全在 provider/Collector 里，UI 不重新解释数据（尤其不把读不到当 0）。
wpm::ui::HudStyle HudStyleFromConfig(const Config& cfg) {
    wpm::ui::HudStyle s;
    // 颜色：0 表示「随主题」，此时用 HudStyle 里的深色默认值
    if (cfg.colors.background.v != 0) s.bg = cfg.colors.background.v;
    if (cfg.colors.panelBorder.v != 0) s.border = cfg.colors.panelBorder.v;
    if (cfg.colors.label.v != 0) s.label = cfg.colors.label.v;
    if (cfg.colors.value.v != 0) s.value = cfg.colors.value.v;
    if (cfg.colors.unit.v != 0) s.unit = cfg.colors.unit.v;
    if (cfg.colors.accent.v != 0) s.accent = cfg.colors.accent.v;
    s.baseSizeDp = cfg.text.baseSizeDp;
    s.opacity = cfg.window.opacity;
    s.showUnits = cfg.layout.showUnits;
    s.hideMissing = cfg.layout.hideMissing;
    s.marginDp = cfg.window.marginDp;
    // 布局：面板形状（纵铺/横铺、是否带标签、是否换行、间距）完全由配置决定
    s.horizontal = cfg.layout.orientation == wpm::config::Orientation::Horizontal;
    s.showLabels = cfg.layout.showLabels;
    s.wrap = cfg.layout.wrap;
    s.maxWidthDp = cfg.layout.maxWidthDp;
    s.itemGapDp = cfg.layout.itemGapDp;
    s.groupGapDp = cfg.layout.groupGapDp;
    switch (cfg.layout.align) {
        case wpm::config::Align::Center: s.align = 1; break;
        case wpm::config::Align::End: s.align = 2; break;
        case wpm::config::Align::Start:
        default: s.align = 0; break;
    }
    s.topmost = cfg.window.topmost != wpm::config::TopmostPolicy::Never;
    switch (cfg.window.corner) {
        case wpm::config::Corner::TopRight: s.corner = 1; break;
        case wpm::config::Corner::BottomLeft: s.corner = 2; break;
        case wpm::config::Corner::BottomRight: s.corner = 3; break;
        case wpm::config::Corner::TopLeft:
        default: s.corner = 0; break;
    }
    return s;
}

wpm::ui::HudModel BuildHudModel(Collector& collector, const Config& cfg) {
    collector.Step();  // 每帧刷新前采一次；HUD 的刷新节奏就是采样节奏
    const std::shared_ptr<const Snapshot> snapPtr = collector.Latest();
    const Snapshot& snap = *snapPtr;

    wpm::ui::HudModel m;
    // 只留一行标题：CPU/GPU 型号、核心数、基频这些「配置信息」属于 --probe 的内容，
    // 常驻面板上不该占地方（面板是看数的，不是看机器铭牌的）。
    m.title = L"winperfmon " + wpm::text::toWide(WPM_VERSION_STRING);
    const std::string fpsNote = collector.Fps() != nullptr ? collector.Fps()->Note() : std::string();

    const auto cat = MetricCatalog();
    const auto slotOf = [&cat](std::string_view id) -> size_t {
        for (size_t i = 0; i < cat.size(); ++i)
            if (cat[i].id == id) return i;
        return static_cast<size_t>(-1);
    };
    int okCount = 0;
    int missingShown = 0;
    int lastCategory = -1;  // 用于 groupByCategory：换类目时留一段分组间距
    // 顺序即配置数组顺序（config-schema §3.3 顺序语义：数组下标 = 展示顺序，横向从左到右/纵向从上到下）
    for (const auto& mc : cfg.metrics) {
        const size_t i = slotOf(mc.id);
        if (i == static_cast<size_t>(-1) || !mc.visible) continue;

        const auto& r = snap.at(i);
        if (r.ok()) {
            ++okCount;
        } else if (cfg.layout.hideMissing) {
            ++missingShown;
            continue;  // 默认隐藏读不到的项，但底部会告诉你有几项
        }
        wpm::ui::HudRow row;
        const int category = static_cast<int>(cat[i].category);
        row.newGroup = cfg.layout.groupByCategory && lastCategory >= 0 && category != lastCategory;
        lastCategory = category;
        row.label = mc.label.empty() ? wpm::text::toWide(std::string(cat[i].defaultLabel))
                                     : wpm::text::toWide(mc.label);
        row.available = r.ok();
        std::string v;
        FormatValue(cat[i], r, cfg, v);
        // 契约（config-schema §3.3 渲染约束）：缺数据显示 N/A，且颜色降为暗色，避免被误读成异常值
        row.value = r.ok() ? wpm::text::toWide(v) : std::wstring(L"N/A");
        row.colorOverride = mc.color.v;  // 0 = 随主题
        if (cfg.layout.showUnits) {
            // 读不到时单位位置写「具体缺什么」：需要 Ring0 / 需要 PresentMon 这类信息有用，
            // 但状态就是普通 Unavailable 时不写——否则面板上会出现「N/A 不可用」两段同义文字。
            row.unit = r.ok() ? wpm::text::toWide(std::string(cat[i].unit))
                              : (r.status == Status::Unavailable
                                     ? std::wstring()
                                     : wpm::text::toWide(StatusText(r.status)));
        }
        row.emphasis = r.ok() && (cat[i].id == "cpu.usage" || cat[i].id == "gpu.usage" ||
                                  cat[i].id == "fps.avg");
        m.rows.push_back(std::move(row));
    }

    // 页脚说人话：显示了几项、有几项读不到、每轮采样花多少毫秒。
    // （旧文案「13/50 可用 · 0 缺 · 0.8 ms」把「目录总数」和「缺」当术语用，面板上没人看得懂。）
    char footer[192];
    if (missingShown > 0) {
        std::snprintf(footer, sizeof(footer), "%d 项 · %d 项读不到 · 每轮 %.1f ms",
                      okCount, missingShown, collector.StepAvgMs());
    } else {
        std::snprintf(footer, sizeof(footer), "%d 项 · 每轮 %.1f ms", okCount,
                      collector.StepAvgMs());
    }
    m.footer = wpm::text::toWide(footer);
    return m;
}

int RunHud(Collector& collector, const Config& cfg) {
    wpm::ui::HudWindow::EnablePerMonitorDpi();
    wpm::ui::HudWindow win;
    win.SetIntervalMs(static_cast<unsigned>(cfg.update.sensorIntervalMs));
    win.SetBuilder([&collector, &cfg]() { return BuildHudModel(collector, cfg); });
    std::string err;
    if (!win.Create(HudStyleFromConfig(cfg), &err)) {
        std::printf("窗口创建失败：%s\n", err.c_str());
        return 3;
    }
    return win.Run();
}

// ── 探针需要的环境事实（release-github.md §11 头部字段）────────────────────
bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, size, &size) != FALSE;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

// PawnIO 安装检测：驱动文件或服务任一存在即视为已安装（M1 起才真正调用）
bool DetectPawnIo() {
    if (GetFileAttributesW(L"C:\\Windows\\System32\\drivers\\PawnIO.sys") != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"PawnIO", SERVICE_QUERY_STATUS);
    const bool found = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return found;
}

// HVCI（内存完整性）：DeviceGuard 未显式配置时的安全默认值是「启用」
bool DetectHvci() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\"
                      L"HypervisorEnforcedCodeIntegrity",
                      0, KEY_READ, &k) != ERROR_SUCCESS) {
        return true;  // 键不存在 = 出厂默认（启用）
    }
    DWORD enabled = 1;
    DWORD size = sizeof(enabled);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(k, L"Enabled", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&enabled), &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return true;
    return enabled != 0;
}

std::string ReadOsBuild() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_READ, &k) != ERROR_SUCCESS) {
        return {};
    }
    auto readStr = [&](const wchar_t* name) -> std::string {
        wchar_t buf[128]{};
        DWORD size = sizeof(buf) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) !=
                ERROR_SUCCESS ||
            type != REG_SZ) {
            return {};
        }
        return wpm::text::toUtf8(buf);
    };
    auto readDword = [&](const wchar_t* name) -> std::string {
        DWORD v = 0, size = sizeof(v), type = 0;
        if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(&v), &size) !=
                ERROR_SUCCESS ||
            type != REG_DWORD) {
            return {};
        }
        return std::to_string(v);
    };
    // CurrentBuildNumber 是字符串（如 "26200"），UBR 是数字修订号（如 1742）
    const std::string build = readStr(L"CurrentBuildNumber");
    const std::string ubr = readDword(L"UBR");
    RegCloseKey(k);
    if (build.empty()) return {};
    return ubr.empty() ? build : build + "." + ubr;
}

// release-github.md §11：--probe 输出，key=value 行式、UTF-8 无 BOM、段序固定
void PrintProbe(const Config& cfg, const wpm::config::ConfigPaths& paths,
                const std::shared_ptr<const Snapshot>& snap, const std::string& initReport,
                Collector& collector) {
    std::printf("probeVersion=1\n");
    std::printf("appVersion=%s\n", WPM_VERSION_STRING);
    std::printf("osBuild=%s\n", ReadOsBuild().c_str());
    std::printf("admin=%s\n", IsProcessElevated() ? "true" : "false");
    const bool pawnio = DetectPawnIo();
    std::printf("pawnio=%s\n", pawnio ? "present" : "absent");
    std::printf("hvci=%s\n", DetectHvci() ? "on" : "off");
    std::printf("ring0Backend=%s\n", pawnio ? "PawnIO" : "none");
    std::printf("\n[system]\n");

    auto regStr = [](HKEY root, const wchar_t* sub, const wchar_t* name) -> std::string {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
        wchar_t buf[512]{};
        DWORD size = sizeof(buf) - sizeof(wchar_t);
        DWORD type = 0;
        const LONG rc = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf),
                                         &size);
        RegCloseKey(k);
        if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
        return wpm::text::toUtf8(buf);
    };

    const std::wstring base = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    std::printf("dmi.board.vendor=%s\n", regStr(HKEY_LOCAL_MACHINE, base.c_str(), L"BaseBoardManufacturer").c_str());
    std::printf("dmi.board.name=%s\n", regStr(HKEY_LOCAL_MACHINE, base.c_str(), L"BaseBoardProduct").c_str());
    std::printf("dmi.bios.version=%s\n", regStr(HKEY_LOCAL_MACHINE, base.c_str(), L"BIOSVersion").c_str());
    std::printf("cpu.brand=%s\n", snap->cpuBrand.empty() ? "" : snap->cpuBrand.c_str());

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    std::printf("cpu.logical=%u\n", static_cast<unsigned>(si.dwNumberOfProcessors));
    std::printf("cpu.baseClockMHz=%.0f\n", snap->baseClockMHz);

    std::printf("\n[superio]\n");
    std::printf("lpc.detected=%d\n", 0);
    std::printf("note=Super I/O 探测（LPC 0x2E/0x2F、0x4E/0x4F）需要 Ring0，M1 接入；笔记本机型通常无独立 Super I/O\n");

    // [ring0]：PawnIO 通道的自证段。没有它，用户看到"需要 Ring0 通道"时无从判断卡在哪一步
    // （驱动没装 / 服务没起 / 模块文件没放对 / 寄存器读不出来）。
    std::printf("\n[ring0]\n");
    if (auto* r0 = collector.Ring0()) {
        const auto d = r0->Probe();
        std::printf("ring0.vendor=%s\n", d.amd ? "AMD" : "Intel");
        std::printf("ring0.device=%s\n", d.devicePath.empty() ? "(未打开)" : d.devicePath.c_str());
        std::printf("ring0.driverVersion=%s\n",
                    d.driverVersion.empty() ? "(未读到)" : d.driverVersion.c_str());
        std::printf("ring0.module=%s\n", d.moduleLoaded ? "loaded" : "not-loaded");
        if (!d.modulePath.empty()) std::printf("ring0.modulePath=%s\n", d.modulePath.c_str());
        if (!d.moduleNote.empty()) std::printf("ring0.moduleNote=%s\n", d.moduleNote.c_str());
        if (d.smnTempOk) {
            std::printf("ring0.smn.tempC=%.1f\n", d.smnTempC);
            std::printf("ring0.smn.tempRaw=0x%08X\n", d.smnTempRaw);
        }
        if (d.voltageOk) {
            std::printf("ring0.smn.coreVoltageV=%.4f\n", d.voltageV);
            std::printf("ring0.smn.voltageOffset=0x%05X\n", d.voltageOffset);
            std::printf("ring0.smn.voltageRaw=0x%08X\n", d.voltageRaw);
        } else if (d.amd) {
            // 读不到就说清为什么：本机实测 SVI2 平面与 TFN 恒为 0，这里如实报 n/a
            std::printf("ring0.smn.coreVoltageV=n/a\n");
            std::printf("ring0.smn.voltageRaw=0x%08X\n", d.voltageRaw);
        }
        if (d.amd) std::printf("ring0.smn.tfn=0x%08X\n", d.tfnRaw);
        if (!d.voltageNote.empty()) std::printf("ring0.smn.voltageNote=%s\n", d.voltageNote.c_str());
        if (d.pkgEnergyOk) std::printf("ring0.pkgPowerW=%.1f\n", d.pkgPowerW);
        std::printf("ring0.pwrUnitRaw=0x%08X\n", d.pwrUnitRaw);
        std::printf("ring0.perCore=%u/%u\n", d.perCoreSampled, d.perCoreTotal);
        if (d.perCoreSampled > 0) {
            std::printf("ring0.perCoreMHzMin=%.0f\n", d.perCoreMinMHz);
            std::printf("ring0.perCoreMHzMax=%.0f\n", d.perCoreMaxMHz);
        }
        if (!d.note.empty()) std::printf("ring0.note=%s\n", d.note.c_str());
    } else {
        std::printf("ring0.provider=absent\n");
    }
    if (!collector.Ring0Note().empty())
        std::printf("ring0.unavailableReason=%s\n", collector.Ring0Note().c_str());

    // [fps]：FPS 采集链的自证段。FPS 出问题时（游戏在前台却没有数字）要看这一段的
    // csvFrames 是不是 0：0 说明根本没采到，不是"帧率恰好为 0"。
    std::printf("\n[fps]\n");
    if (auto* fps = collector.Fps()) {
        const auto d = fps->Probe();
        std::printf("fps.presentmon=%s\n", d.exePath.empty() ? "(未找到)" : d.exePath.c_str());
        std::printf("fps.running=%s\n", d.running ? "true" : "false");
        if (!d.target.empty()) {
            std::printf("fps.target=%s\n", d.target.c_str());
            std::printf("fps.targetSource=%s\n", d.targetSource.c_str());
            std::printf("fps.processId=%u\n", d.processId);
        }
        std::printf("fps.csvLines=%llu\n", static_cast<unsigned long long>(d.csvLines));
        std::printf("fps.csvFrames=%llu\n", static_cast<unsigned long long>(d.csvFrames));
        std::printf("fps.csvMalformed=%llu\n", static_cast<unsigned long long>(d.csvMalformed));
        std::printf("fps.skippedBeforeHeader=%llu\n",
                    static_cast<unsigned long long>(d.skippedBeforeHeader));
        std::printf("fps.header=%s\n", d.headerNote.c_str());
        std::printf("fps.ringFrames=%llu\n", static_cast<unsigned long long>(d.ringFrames));
        std::printf("fps.ringSpanMs=%.0f\n", d.ringSpanMs);
        if (d.exitCode >= 0) std::printf("fps.exitCode=%d\n", d.exitCode);
        std::printf("fps.note=%s\n", d.note.c_str());
        std::printf("fps.locateNote=%s\n", d.locateNote.c_str());
    } else {
        std::printf("fps.provider=absent\n");
    }

    std::printf("\n[gpu]\n");
    std::printf("gpu.adapters=%d\n", snap->gpuAdapterCount);
    std::printf("gpu.selected=%d\n", snap->gpuSelectedIndex);
    std::printf("gpu.selectedIndexConfig=%d\n", collector.GpuSelectedConfig());
    std::printf("gpu.name=%s\n", snap->gpuName.c_str());
    const auto* adl = collector.Adl();
    std::printf("gpu.metricsSource=%s\n", (adl && adl->Available()) ? "ADL2" : "none");
    if (!collector.GpuNote().empty()) {
        std::printf("gpu.note=%s\n", collector.GpuNote().c_str());
    }
    if (const auto* acpi = collector.Acpi(); acpi && acpi->Available()) {
        // 热区清单是惰性枚举的（枚举本身要 4.3 MB），--probe 是诊断命令，
        // 这里主动触发一次，好让用户看到本机到底有哪些热区。
        acpi->ProbeZones();
        // 只报"候选的 GPU 温度兜底热区"；没有就明确写 none
        // （曾误报 Zones().front()，把电池热区 \_TZ.BAGZ 当成 GPU 来源）
        // 注意：这里报的是**分类结果**，不是"可用"。热区是否真接了传感器要按每次读数判
        // 哨兵值/合理区间（本机 \_TZ.GFXZ 恒报 25 °C，会在 Sample 里被判不可用并说明原因）。
        std::string used = "none";
        std::string list;
        for (const auto& z : acpi->Zones()) {
            // 多个候选时取第一个，与 Sample 的取用顺序一致。
            if (z.role == "gpu" && used == "none") used = z.instance;
            if (!list.empty()) list += " ";
            list += z.instance + "(" + z.role + ")";
        }
        std::printf("gpu.acpiFallbackZone=%s\n", used.c_str());
        std::printf("acpi.zones=%s\n", list.empty() ? "none" : list.c_str());
    }

    int gpuNo = 0;
    for (const auto& g : collector.GpuAdapters()) {
        ++gpuNo;
        std::printf("\n[gpu.%d]\n", gpuNo);
        std::printf("vendor=%s\n", wpm::sensors::VendorName(g.vendorId).c_str());
        std::printf("name=%s\n", g.name.c_str());
        std::printf("vendorId=0x%04X\n", g.vendorId);
        std::printf("deviceId=0x%04X\n", g.deviceId);
        std::printf("backend=%s\n", g.backend.empty() ? "none" : g.backend.c_str());
        std::printf("integrated=%s\n", g.integrated ? "true" : "false");
        std::printf("software=%s\n", g.software ? "true" : "false");
        std::printf("dedicatedVideoMemoryMB=%llu\n",
                    static_cast<unsigned long long>(g.dedicatedVideoMemory / (1024ull * 1024ull)));
        std::printf("sharedSystemMemoryMB=%llu\n",
                    static_cast<unsigned long long>(g.sharedSystemMemory / (1024ull * 1024ull)));
        std::printf("dxgiIndex=%u\n", g.dxgiIndex);
        std::printf("luid=0x%08X%08X\n", static_cast<unsigned>(g.luid >> 32),
                    static_cast<unsigned>(g.luid & 0xFFFFFFFFu));
        std::printf("duplicates=%d\n", g.duplicates);
        const bool sel = (g.index == snap->gpuSelectedIndex);
        std::printf("selected=%s\n", sel ? "true" : "false");
        std::printf("telemetry=%s\n", (!g.backend.empty() && sel) ? "yes" : "no");
        if (sel) {
            // release-github.md §11 的 [gpu.N] 契约要求给出风扇转速原始值
            for (size_t i = 0; i < MetricCatalog().size(); ++i) {
                if (MetricCatalog()[i].id != "gpu.fan.rpm") continue;
                const auto& fr = snap->metrics[i];
                if (fr.status == Status::Ok) {
                    std::printf("fanRpm=%.0f\n", fr.value);
                } else {
                    std::printf("fanRpm=n/a（%s）\n", fr.detail.empty() ? "不可用" : fr.detail.c_str());
                }
                break;
            }
        }
    }

    std::printf("\n[config]\n");
    std::printf("config.path=%s\n", wpm::text::toUtf8(paths.file.wstring()).c_str());
    size_t visible = 0;
    for (const auto& m : cfg.metrics) {
        if (m.visible) ++visible;
    }
    std::printf("config.visibleMetrics=%zu\n", visible);

    std::printf("\n[issues]\n");
    bool any = false;
    if (!pawnio) {
        // release-github.md §11：未装 PawnIO 时仍输出免驱部分，并在 [issues] 记 pawnio=absent
        std::printf("pawnio=absent（每核温度/电压、风扇转速、主板传感器需要 Ring0 通道）\n");
        any = true;
    }
    if (!IsProcessElevated()) {
        std::printf("admin=false（部分厂商 WMI 与 PresentMon CLI 路径需要管理员）\n");
        any = true;
    }
    if (const auto* adlSrc = collector.Adl(); !adlSrc || !adlSrc->Available()) {
        if (!collector.GpuNote().empty()) {
            std::printf("gpu=%s\n", collector.GpuNote().c_str());
            any = true;
        }
    } else {
        std::printf("gpu=ADL2 免驱遥测可用（无管理员、无 Ring0）\n");
        any = true;
    }
    for (const auto& r : snap->metrics) {
        if (r.status == Status::Error) {
            std::printf("error=%s\n", r.detail.c_str());
            any = true;
        }
    }
    if (!any) std::printf("error=none\n");

    std::printf("\n[init]\n");
    std::printf("%s", initReport.c_str());

    // 启动耗时分段（docs/winperfmon-dev-plan.md 冷启动预算 ≤300 ms 的校准依据）。
    // 只在 --probe 里输出，正常采样路径一次 printf 都不多花。
    std::printf("\n[timing]\n");
    std::printf("startupMs=%.1f\n", collector.StartupMs());
    std::printf("firstStepMs=%.2f\n", collector.FirstStepMs());
    std::printf("stepCount=%llu\n", static_cast<unsigned long long>(collector.StepCount()));
    std::printf("stepAvgMs=%.2f\n", collector.StepAvgMs());
    std::printf("stepMaxMs=%.2f\n", collector.StepMaxMs());
    for (const auto& s : collector.SampleStats()) {
        std::printf("sample.%s.avgMs=%.3f\n", s.name.c_str(), s.avgMs);
        std::printf("sample.%s.maxMs=%.3f\n", s.name.c_str(), s.maxMs);
    }
    for (const auto& s : collector.InitTimings()) {
        std::printf("stage.%s=%.1f\n", s.name.c_str(), s.ms);
    }
}

int RunOnce(Collector& c, const Config& cfg, bool onlyVisible) {
    c.Step();
    Sleep(600);  // PDH 需要两次采样之间有时间差
    c.Step();
    auto snap = c.Latest();
    DumpSnapshot(*snap, onlyVisible, cfg);
    PrintStatusSummary(*snap);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    g_processStartMs = NowMs();  // 冷启动计时原点：进程入口第一件事
    MemMark("进程入口");          // 早于 AttachConsole，用于分离"控制台挂接"的内存代价
    // 文本模式命令直接走控制台；无参数时打开 HUD 窗口（M0.5 的可见雏形）。
    bool needConsole = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--version" || a == L"-v") needConsole = false;
        if (a == L"--hud" || a == L"--window") needConsole = false;
    }

    bool showVersion = false;
    bool once = false;
    bool probe = false;
    bool selftest = false;
    bool all = false;
    bool hud = (argc <= 1);  // 双击 / 无参数 = 打开窗口，而不是打印后退出
    int watchSeconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--version" || a == L"-v") showVersion = true;
        else if (a == L"--once") once = true;
        else if (a == L"--probe") probe = true;
        else if (a == L"--selftest") selftest = true;
        else if (a == L"--all") all = true;
        else if (a == L"--hud" || a == L"--window") hud = true;
        else if (a == L"--watch" && i + 1 < argc) watchSeconds = _wtoi(argv[++i]);
    }
    if (needConsole) AttachOrAllocateConsole();

    // --version 不需要控制台也不加载配置
    if (showVersion) {
        std::printf("winperfmon %s\n", WPM_VERSION_STRING);
        std::printf("构建：%s %s\n", __DATE__, __TIME__);
        std::fflush(stdout);
        return 0;
    }
    if (!once && !probe && !selftest && watchSeconds == 0 && !hud) once = true;  // 默认行为

    // ── 配置：命令行 > 环境变量 > portable > %APPDATA% > 只读 ──────────────
    const wpm::config::ConfigPaths paths = wpm::config::ResolveConfigPaths(argc, argv);
    MemMark("配置前");
    wpm::config::LoadResult lr = wpm::config::LoadConfig(paths);
    MemMark("配置加载后");
    std::vector<std::string> configNotes;
    if (!lr.error.empty()) configNotes.push_back("配置解析失败，已用默认值：" + lr.error);
    for (const auto& d : lr.diagnostics) configNotes.push_back(d.field + " " + d.message);
    // 命令行覆盖（config-schema.md §8：--interval / --vertical / --horizontal / --gpu）
    std::vector<wpm::config::Diagnostic> cliDiags;
    wpm::config::ApplyCommandLineOverrides(lr.config, argc, argv, cliDiags);
    for (const auto& d : cliDiags) configNotes.push_back(d.field + " " + d.message);
    const Config& cfg = lr.config;

    Collector collector;
    collector.SetProbeMode(probe);
    std::string initReport;
    const bool anyProvider = collector.Init(cfg, initReport);
    MemMark("provider 初始化后");
    if (!anyProvider) {
        if (hud) {
            MessageBoxW(nullptr, wpm::text::toWide(initReport).c_str(), L"winperfmon：没有可用的数据源",
                        MB_OK | MB_ICONWARNING);
        } else {
            std::printf("没有可用的免驱数据源。\n%s", initReport.c_str());
            std::fflush(stdout);
        }
        return 2;
    }

    // 无参数 / --hud：直接进窗口模式（没有控制台，printf 不会有人看到）
    if (hud) return RunHud(collector, cfg);

    // 读快照时保留一份：下面的控制台输出与控制台命令都基于同一次采样

    std::printf("winperfmon %s（M1 免驱遥测：原生 CPU 采样 + AMD ADL2 PM 表 + ACPI 热区兜底）\n",
                WPM_VERSION_STRING);
    std::printf("配置位置：%s%s\n", wpm::text::toUtf8(paths.file.wstring()).c_str(),
                paths.writable ? "" : "（只读）");
    for (const auto& n : configNotes) std::printf("  配置：%s\n", n.c_str());
    std::printf("数据源：\n%s", initReport.c_str());
    std::printf("\n");

    if (selftest) {
        collector.Step();
        Sleep(600);
        collector.Step();
        auto snap = collector.Latest();
        int errors = 0;
        int ok = 0;
        for (const auto& r : snap->metrics) {
            if (r.status == Status::Error) ++errors;
            if (r.status == Status::Ok) ++ok;
        }
        // 自检：至少 mem.usage 与 cpu.usage/cpu.freq.avg 要有值，且不允许 Error
        std::printf("自检：可用 %d 项，错误 %d 项\n", ok, errors);
        const bool pass = (errors == 0 && ok >= 3);
        std::printf("%s\n", pass ? "SELFTEST PASS" : "SELFTEST FAIL");
        std::fflush(stdout);
        return pass ? 0 : 1;
    }

    if (probe) {
        collector.Step();
        Sleep(600);
        collector.Step();
        PrintProbe(cfg, paths, collector.Latest(), initReport, collector);
        std::fflush(stdout);
        return 0;
    }

    if (watchSeconds > 0) {
        std::printf("连续采样 %d 秒（Ctrl+C 结束）\n\n", watchSeconds);
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(watchSeconds) * 1000ULL;
        collector.Step();
        MemMark("首帧后");
        while (GetTickCount64() < deadline) {
            Sleep(static_cast<DWORD>(cfg.update.sensorIntervalMs));
            collector.Step();
            auto snap = collector.Latest();
            std::string cpu = "--", freq = "--", mem = "--", fps = "--", low = "--";
            const auto cat = MetricCatalog();
            for (size_t i = 0; i < cat.size(); ++i) {
                if (cat[i].id == "cpu.usage") FormatValue(cat[i], snap->at(i), cfg, cpu);
                else if (cat[i].id == "cpu.freq.avg") FormatValue(cat[i], snap->at(i), cfg, freq);
                else if (cat[i].id == "mem.usage") FormatValue(cat[i], snap->at(i), cfg, mem);
                else if (cat[i].id == "fps.avg") FormatValue(cat[i], snap->at(i), cfg, fps);
                else if (cat[i].id == "fps.low1") FormatValue(cat[i], snap->at(i), cfg, low);
            }
            std::printf("[%6.1fs] CPU %s%%  频率 %s MHz  内存 %s%%  FPS %s（1%% Low %s）\n",
                        static_cast<double>(snap->takenAtMs) / 1000.0, cpu.c_str(), freq.c_str(),
                        mem.c_str(), fps.c_str(), low.c_str());
            std::fflush(stdout);
        }
        // 预算验收证据（docs/winperfmon-dev-plan.md §1）：1 Hz 下"平均采样耗时"直接就是
        // 采集线程的 CPU 占用率；工作集是常驻内存预算（≤15 MB）的对照值。
        const auto mem = ReadMemUsage();
        std::printf("\n采样统计：%llu 次，平均 %.2f ms/次，最大 %.2f ms/次\n",
                    static_cast<unsigned long long>(collector.StepCount()), collector.StepAvgMs(),
                    collector.StepMaxMs());
        std::printf("内存：工作集 %.2f MB，私有 %.2f MB，峰值 %.2f MB（预算 ≤15 MB）\n",
                    mem.workingSetMb, mem.privateMb, mem.peakWorkingSetMb);
        std::string breakdown;
        for (const auto& s : collector.SampleStats()) {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%s%s %.2f", breakdown.empty() ? "" : "，", s.name.c_str(),
                          s.avgMs);
            breakdown += buf;
        }
        if (!breakdown.empty()) std::printf("逐源平均耗时（ms）：%s\n", breakdown.c_str());
        for (const auto& m : g_memMarks) {
            std::printf("  %-16s 工作集 %6.2f MB  私有 %5.2f MB\n", m.name.c_str(), m.mem.workingSetMb,
                        m.mem.privateMb);
        }
        return 0;
    }

    const int rc = RunOnce(collector, cfg, /*onlyVisible=*/!all);
    std::fflush(stdout);
    return rc;
}
