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
#include "sensors/cpu_pdh.h"
#include "sensors/memory.h"
#include "sensors/sensor.h"

#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using wpm::config::Config;
using wpm::config::MetricCatalog;
using wpm::sensors::Snapshot;
using wpm::sensors::Status;

FILE* g_out = nullptr;

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

    bool Init(std::string& report) {
        providers_.push_back(std::make_unique<wpm::sensors::MemoryProvider>());
        providers_.push_back(std::make_unique<wpm::sensors::CpuPdhProvider>());
        bool any = false;
        for (auto& p : providers_) {
            std::string detail;
            const bool ok = p->Init(detail);
            report += std::string("  ") + p->name() + ": " + (ok ? "就绪" : "不可用");
            if (!detail.empty()) report += "（" + detail + "）";
            report += "\n";
            any = any || ok;
        }
        // 未接线的指标统一标注原因，UI 才不会显示成 0
        Snapshot& snap = publisher_->Writable();
        MarkUnwiredByAvailability(snap);
        FillSysMetrics(snap);
        publisher_->Publish();
        return any;
    }

    void Step() {
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
            if (p->Available()) p->Sample(snap);
        }
        FillSysMetrics(snap);
        publisher_->Publish();
    }

    [[nodiscard]] std::shared_ptr<const Snapshot> Latest() const { return publisher_->Latest(); }

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
    static void MarkUnwiredByAvailability(Snapshot& snap) {
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
                    r.detail = "需要 Ring0 通道（PawnIO），M1 接入";
                    break;
                case Availability::AvGpu:
                    r.status = Status::NeedGpuLib;
                    r.detail = "需要厂商 GPU 库（NVML/ADLX/IGCL），M1 接入";
                    break;
                case Availability::AvPresentMon:
                    r.status = Status::NeedPresentMon;
                    r.detail = "需要 PresentMon，M2 接入";
                    break;
            }
        }
    }

    std::unique_ptr<wpm::sensors::SnapshotPublisher> publisher_;
    std::vector<std::unique_ptr<wpm::sensors::ISensorProvider>> providers_;
    int64_t startCounter_ = 0;
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
                const std::shared_ptr<const Snapshot>& snap, const std::string& initReport) {
    std::printf("probeVersion=1\n");
    std::printf("appVersion=%s\n", WPM_VERSION_STRING);
    std::printf("osBuild=%s\n", ReadOsBuild().c_str());
    std::printf("admin=%s\n", IsProcessElevated() ? "true" : "false");
    const bool pawnio = DetectPawnIo();
    std::printf("pawnio=%s\n", pawnio ? "present" : "absent");
    std::printf("hvci=%s\n", DetectHvci() ? "on" : "off");
    std::printf("ring0Backend=%s\n", pawnio ? "PawnIO（模块在 M1 接入）" : "none");
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
    std::printf("note=Super I/O 探测在 M1 接入（需要 Ring0）\n");

    std::printf("\n[gpu]\n");
    std::printf("gpu.adapters=%d\n", snap->gpuAdapterCount);
    std::printf("gpu.selected=%d\n", snap->gpuSelectedIndex);
    std::printf("gpu.name=%s\n", snap->gpuName.c_str());
    std::printf("gpu.metricsSource=none（M1 接入 NVML/ADLX/IGCL）\n");

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
        std::printf("pawnio=absent（CPU 温度/功耗与风扇转速需要 Ring0 通道）\n");
        any = true;
    }
    if (!IsProcessElevated()) {
        std::printf("admin=false（部分厂商 WMI 与 PresentMon CLI 路径需要管理员）\n");
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
    // 文本模式命令直接走控制台；无参数时 M0 阶段也先走文本（GUI 在 M3）
    bool needConsole = true;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--version") needConsole = false;
    }
    if (needConsole) AttachOrAllocateConsole();

    bool showVersion = false;
    bool once = false;
    bool probe = false;
    bool selftest = false;
    bool all = false;
    int watchSeconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--version" || a == L"-v") showVersion = true;
        else if (a == L"--once") once = true;
        else if (a == L"--probe") probe = true;
        else if (a == L"--selftest") selftest = true;
        else if (a == L"--all") all = true;
        else if (a == L"--watch" && i + 1 < argc) watchSeconds = _wtoi(argv[++i]);
    }

    // --version 不需要控制台也不加载配置
    if (showVersion) {
        std::printf("winperfmon %s\n", WPM_VERSION_STRING);
        std::printf("构建：%s %s\n", __DATE__, __TIME__);
        std::fflush(stdout);
        return 0;
    }
    if (!once && !probe && !selftest && watchSeconds == 0) once = true;  // M0 默认行为

    // ── 配置：命令行 > 环境变量 > portable > %APPDATA% > 只读 ──────────────
    const wpm::config::ConfigPaths paths = wpm::config::ResolveConfigPaths(argc, argv);
    wpm::config::LoadResult lr = wpm::config::LoadConfig(paths);
    std::vector<std::string> configNotes;
    if (!lr.error.empty()) configNotes.push_back("配置解析失败，已用默认值：" + lr.error);
    for (const auto& d : lr.diagnostics) configNotes.push_back(d.field + " " + d.message);
    // 命令行覆盖（config-schema.md §8：--interval / --vertical / --horizontal / --gpu）
    std::vector<wpm::config::Diagnostic> cliDiags;
    wpm::config::ApplyCommandLineOverrides(lr.config, argc, argv, cliDiags);
    for (const auto& d : cliDiags) configNotes.push_back(d.field + " " + d.message);
    const Config& cfg = lr.config;

    Collector collector;
    std::string initReport;
    const bool anyProvider = collector.Init(initReport);
    if (!anyProvider) {
        std::printf("没有可用的免驱数据源。\n%s", initReport.c_str());
        std::fflush(stdout);
        return 2;
    }

    std::printf("winperfmon %s（M0 免驱骨架）\n", WPM_VERSION_STRING);
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
        PrintProbe(cfg, paths, collector.Latest(), initReport);
        std::fflush(stdout);
        return 0;
    }

    if (watchSeconds > 0) {
        std::printf("连续采样 %d 秒（Ctrl+C 结束）\n\n", watchSeconds);
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(watchSeconds) * 1000ULL;
        collector.Step();
        while (GetTickCount64() < deadline) {
            Sleep(static_cast<DWORD>(cfg.update.sensorIntervalMs));
            collector.Step();
            auto snap = collector.Latest();
            std::string cpu = "--", freq = "--", mem = "--";
            const auto cat = MetricCatalog();
            for (size_t i = 0; i < cat.size(); ++i) {
                if (cat[i].id == "cpu.usage") FormatValue(cat[i], snap->at(i), cfg, cpu);
                else if (cat[i].id == "cpu.freq.avg") FormatValue(cat[i], snap->at(i), cfg, freq);
                else if (cat[i].id == "mem.usage") FormatValue(cat[i], snap->at(i), cfg, mem);
            }
            std::printf("[%6.1fs] CPU %s%%  频率 %s MHz  内存 %s%%\n",
                        static_cast<double>(snap->takenAtMs) / 1000.0, cpu.c_str(), freq.c_str(),
                        mem.c_str());
            std::fflush(stdout);
        }
        return 0;
    }

    const int rc = RunOnce(collector, cfg, /*onlyVisible=*/!all);
    std::fflush(stdout);
    return rc;
}
