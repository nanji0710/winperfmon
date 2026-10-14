// SPDX-License-Identifier: MIT
// 指标目录（config-schema.md §4）：编译期内置，不是用户配置。
// 用户配置里只出现 id、可见性、顺序、颜色、字号倍率、精度与别名标签。
//
// 字段顺序与 config-schema.md §4 / §6 的 MetricInfo 一致：id、group、defaultLabel、
// unit、defaultVisible、defaultPrecision、source。本头文件另加 category/availability
// 两个编译期辅助字段（用于 UI 分组显示与"为什么读不到"分类），不参与配置文件序列化。
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace wpm::config {

// 分组（与 config-schema.md §4 表的「分组」列一一对应：cpu | gpu | fan | fps | sys）
// 注意：mem.usage / mem.used 归入 Sys（文档表格如此定义，无独立 mem 组）。
enum class Category : uint8_t { Cpu, Gpu, Fan, Fps, Sys };

// 数据来源通道。用于 UI 标注"需要驱动"与给出确切的降级原因（ui-settings.md §指标页）。
// 用 Av 前缀避免与 Category 的成员名在同一 using enum 作用域下冲突。
enum class Availability : uint8_t {
    AvAny,        // 免驱即可（PDH / GlobalMemoryStatusEx / 本地时钟）
    AvGpu,        // 需要厂商 GPU 库（NVML / ADLX / IGCL）
    AvRing0,      // 需要 PawnIO 通道（MSR / SMN / Super I/O / EC）
    AvPresentMon, // 需要 PresentMon（服务 SDK 或命令行）
};

struct MetricInfo {
    std::string_view id;           // 规范小写形式，唯一；发布后不得改名（改名走别名表 §8）
    Category category;             // 编译期分组（见 CategoryKey 的字符串形式）
    std::string_view defaultLabel;
    std::string_view unit;         // 空 = 无单位（文本类指标）
    bool defaultVisible;           // 首次运行时是否默认显示
    int defaultPrecision;          // 目录默认小数位；-1 = 不适用
    Availability availability;     // 编译期通道要求（非文档字段，UI 用）
    std::string_view source;       // 数据来源说明（用于 UI tooltip）
};

// 指标目录（顺序 = UI「可用指标」列表的默认顺序，也是 metrics[] 的首次生成顺序）
std::span<const MetricInfo> MetricCatalog();

// 分组显示名（"CPU" / "GPU" / "风扇" / "帧率" / "系统"）
std::string_view CategoryName(Category c);

// 分组的稳定字符串键（"cpu" | "gpu" | "fan" | "fps" | "sys"），与文档 §4 表一致
std::string_view CategoryKey(Category c);

// 查找指标；大小写不敏感（config-schema.md §6 FindMetric）
const MetricInfo* FindMetric(std::string_view id);

// 别名表（config-schema.md §8）：旧 id → 规范 id
const MetricInfo* ResolveMetricAlias(std::string_view id);

}  // namespace wpm::config
