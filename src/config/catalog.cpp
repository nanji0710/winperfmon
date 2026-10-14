// SPDX-License-Identifier: MIT
#include "config/catalog.h"

#include "core/text.h"

#include <array>

namespace wpm::config {
namespace {

using enum Category;
using enum Availability;

// config-schema.md §4 指标目录逐行搬入（文档表格是权威定义）。
// 唯一展开项：`fan.chassis1..6.rpm` 一行按命名规范展开为 6 个槽位，
// 故表格 45 行 → 目录 50 项。
// 顺序与文档表格一致，也就是 UI「可用指标」列表与 metrics[] 首次生成的默认顺序。
constexpr std::array<MetricInfo, 50> kCatalog{{
    // ── CPU ────────────────────────────────────────────────────────────────
    {"cpu.usage", Cpu, "CPU", "%", true, 0, AvAny, "PDH / NtQuerySystemInformation"},
    {"cpu.freq.avg", Cpu, "CPU 频率", "MHz", true, 0, AvAny,
     "APERF/MPERF 差分（Intel）→ PDH % Processor Performance × 基频（AMD 免驱同路径）"},
    {"cpu.freq.percore", Cpu, "每核频率", "MHz", false, 0, AvRing0, "绑核读 MSR"},
    {"cpu.temp.package", Cpu, "CPU 温度", "°C", true, 0, AvRing0,
     "Intel 0x19C/0x1B1；AMD SMN 0x00059800（Tctl/Tdie）→ 笔记本 ACPI 热区域"},
    {"cpu.temp.cores", Cpu, "每核温度", "°C", false, 0, AvRing0, "Intel 每核 DTS；AMD PM 表"},
    {"cpu.temp.headroom", Cpu, "距温度上限", "°C", false, 0, AvRing0,
     "Intel TjMax − DTS；AMD 家族上限表 − Tctl"},
    {"cpu.power.package", Cpu, "CPU 功耗", "W", true, 1, AvRing0,
     "RAPL 能量差分（Intel 0x611 / AMD 0xC001029B）"},
    {"cpu.power.cores", Cpu, "核心功耗", "W", false, 1, AvRing0,
     "Intel RAPL PP0（0x639）；AMD 无核级 RAPL（PM 表核心功耗仅作参考值）"},
    {"cpu.power.soc", Cpu, "SoC / Uncore 功耗", "W", false, 1, AvRing0,
     "Intel RAPL PP1（0x641）；AMD PM 表 SoC 域"},
    {"cpu.power.dram", Cpu, "内存功耗", "W", false, 1, AvRing0, "Intel RAPL DRAM（0x619）"},
    {"cpu.voltage", Cpu, "CPU 电压", "V", false, 3, AvRing0, "Intel 0x198；AMD SVI2（PM 表）"},
    {"cpu.throttle.reason", Cpu, "降频原因", "", false, -1, AvRing0,
     "Intel 热/功耗/电流限制位；AMD PM 表限制位"},

    // ── 系统（内存归 sys 组，与文档 §4 表格一致）───────────────────────────
    {"mem.usage", Sys, "内存", "%", true, 0, AvAny, "GlobalMemoryStatusEx"},
    {"mem.used", Sys, "已用内存", "GB", false, 1, AvAny, "GlobalMemoryStatusEx"},

    // ── GPU（全部只对 gpu.selectedIndex 选中的适配器求值）──────────────────
    {"gpu.usage", Gpu, "GPU", "%", true, 0, AvGpu, "NVML / ADLX / IGCL"},
    {"gpu.freq.core", Gpu, "GPU 频率", "MHz", true, 0, AvGpu, "NVML / ADLX / IGCL"},
    {"gpu.freq.mem", Gpu, "显存频率", "MHz", true, 0, AvGpu, "NVML / ADLX / IGCL"},
    {"gpu.temp.core", Gpu, "GPU 温度", "°C", true, 0, AvGpu, "NVML / ADLX / IGCL"},
    {"gpu.temp.hotspot", Gpu, "热点温度", "°C", false, 0, AvGpu, "NVML / ADLX"},
    {"gpu.temp.mem", Gpu, "显存温度", "°C", false, 0, AvGpu,
     "NVML NVML_FI_DEV_MEMORY_TEMP；ADLX"},
    {"gpu.power", Gpu, "GPU 功耗", "W", true, 1, AvGpu, "NVML GetPowerUsage(mW)；ADLX；IGCL"},
    {"gpu.fan.percent", Gpu, "显卡风扇", "%", false, 0, AvGpu, "NVML（多为百分比）；ADLX/IGCL"},
    {"gpu.fan.rpm", Gpu, "显卡风扇", "RPM", false, 0, AvGpu,
     "ADLX / IGCL（NVIDIA 通常不提供 RPM）"},
    {"gpu.vram.usage", Gpu, "显存占用", "%", false, 0, AvGpu, "NVML / ADLX"},
    {"gpu.vram.used", Gpu, "显存已用", "GB", false, 1, AvGpu, "NVML / ADLX"},
    {"gpu.throttle.reason", Gpu, "显卡降频原因", "", false, -1, AvGpu,
     "NVML GetCurrentClocksThrottleReasons；ADLX"},
    {"gpu.enc.usage", Gpu, "编码器", "%", false, 0, AvGpu, "NVML / ADLX"},
    {"gpu.dec.usage", Gpu, "解码器", "%", false, 0, AvGpu, "NVML / ADLX"},

    // ── 风扇 / 主板（Ring0：Super I/O → 笔记本 EC / 厂商 WMI）──────────────
    {"fan.cpu.rpm", Fan, "CPU 风扇", "RPM", false, 0, AvRing0,
     "台式机 Super I/O（Nuvoton/ITE）→ 笔记本 EC / 厂商 WMI（见主方案 §3.3.1）"},
    {"fan.chassis1.rpm", Fan, "机箱风扇 1", "RPM", false, 0, AvRing0,
     "同上（笔记本通常只有 1 个风扇，映射到 fan.cpu.rpm）"},
    {"fan.chassis2.rpm", Fan, "机箱风扇 2", "RPM", false, 0, AvRing0, "同上"},
    {"fan.chassis3.rpm", Fan, "机箱风扇 3", "RPM", false, 0, AvRing0, "同上"},
    {"fan.chassis4.rpm", Fan, "机箱风扇 4", "RPM", false, 0, AvRing0, "同上"},
    {"fan.chassis5.rpm", Fan, "机箱风扇 5", "RPM", false, 0, AvRing0, "同上"},
    {"fan.chassis6.rpm", Fan, "机箱风扇 6", "RPM", false, 0, AvRing0, "同上"},
    {"fan.pump.rpm", Fan, "水泵", "RPM", false, 0, AvRing0, "同上"},
    {"mb.temp.vrm", Fan, "供电温度", "°C", false, 0, AvRing0, "Super I/O"},
    {"mb.temp.board", Fan, "主板温度", "°C", false, 0, AvRing0, "Super I/O"},

    // ── 帧率（PresentMon；口径见 winperfmon-dev-plan.md §3.4）──────────────
    {"fps.instant", Fps, "FPS", "FPS", true, 0, AvPresentMon, "PresentMon（服务 SDK → 命令行）"},
    {"fps.avg", Fps, "平均 FPS", "FPS", true, 0, AvPresentMon, "调和平均：帧数 × 1000 / Σ帧时间"},
    {"fps.low1", Fps, "1% Low", "FPS", true, 0, AvPresentMon,
     "最差 1% 帧时间均值取倒数（口径见 winperfmon-dev-plan.md §3.4）"},
    {"fps.low01", Fps, "0.1% Low", "FPS", false, 0, AvPresentMon, "同上，百分位 0.001"},
    {"fps.frametime", Fps, "帧时间", "ms", false, 1, AvPresentMon, "PresentMon MsBetweenPresents"},
    {"fps.frametime.graph", Fps, "帧时间曲线", "", false, -1, AvPresentMon,
     "环形缓冲 sparkline（30–200 px）"},
    {"fps.stutter", Fps, "卡顿率", "%", false, 1, AvPresentMon,
     "帧时间 > stutterFactor × 基线帧时间的帧占比，基线默认取平均（可切中位）"},
    {"fps.presentmode", Fps, "呈现模式", "", false, -1, AvPresentMon,
     "PresentMon PresentMode（检测 Overlay 是否导致降级）"},
    {"fps.cpu.busy", Fps, "CPU 忙", "ms", false, 1, AvPresentMon, "PresentMon MsCPUBusy"},
    {"fps.gpu.busy", Fps, "GPU 忙", "ms", false, 1, AvPresentMon, "PresentMon MsGPUBusy"},

    // ── 系统 ──────────────────────────────────────────────────────────────
    {"sys.time", Sys, "时间", "", false, -1, AvAny, "本地时钟"},
    {"sys.session", Sys, "采集时长", "s", false, 0, AvAny, "会话计时器"},
}};

// config-schema.md §8 别名表
struct Alias {
    std::string_view from;
    std::string_view to;
};
constexpr std::array<Alias, 2> kAliases{{{"cpu.freq", "cpu.freq.avg"},
                                         {"gpu.temp", "gpu.temp.core"}}};

}  // namespace

std::span<const MetricInfo> MetricCatalog() { return kCatalog; }

std::string_view CategoryName(Category c) {
    switch (c) {
        case Category::Cpu: return "CPU";
        case Category::Gpu: return "GPU";
        case Category::Fan: return "风扇";
        case Category::Fps: return "帧率";
        case Category::Sys: return "系统";
    }
    return "未知";
}

std::string_view CategoryKey(Category c) {
    switch (c) {
        case Category::Cpu: return "cpu";
        case Category::Gpu: return "gpu";
        case Category::Fan: return "fan";
        case Category::Fps: return "fps";
        case Category::Sys: return "sys";
    }
    return "sys";
}

const MetricInfo* FindMetric(std::string_view id) {
    for (const auto& m : kCatalog)
        if (text::iequals(m.id, id)) return &m;
    return nullptr;
}

const MetricInfo* ResolveMetricAlias(std::string_view id) {
    for (const auto& a : kAliases)
        if (text::iequals(a.from, id)) return FindMetric(a.to);
    return nullptr;
}

}  // namespace wpm::config
