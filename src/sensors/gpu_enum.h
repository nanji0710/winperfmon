// SPDX-License-Identifier: MIT
// GPU 适配器枚举（免驱、零第三方库）：DXGI 的 WDDM 适配器列表。
//
// 为什么不用 WMI 的 Win32_VideoController：
//   * 需要 COM + WmiPrvSE 进程，冷启动开销与常驻内存都远超 DXGI
//     （本项目预算：冷启动 ≤300 ms、常驻 ≤15 MB）
//   * DXGI 的枚举顺序就是渲染 API 实际使用的适配器顺序，也是 NVML/IGCL 的对应口径
//
// 本机实测（HP ZHAN66 14 G8a / Ryzen 7 H 255 / Win11 26300）：
//   Win32_VideoController 报 4 个显示适配器，其中 3 个是虚拟/间接显示驱动
//   （GameViewer / OrayIddDriver / Honor Virtual Display）——
//   **按枚举顺序取第一个会拿到虚拟适配器**，这正是需要 gpu.selectedIndex 的原因
//   （A1 决策②，见 docs/config-schema.md §3.5）。
//
// 依赖：dxgi（系统库，非运行时依赖；不随包分发任何 DLL）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wpm::sensors {

struct GpuAdapter {
    int index = 0;      // 枚举下标（= gpu.selectedIndex 的取值域，按本文件的排序重写）
    std::string name;   // DXGI 描述，设置页照原样显示
    uint32_t vendorId = 0;  // 0x1002=AMD / 0x10DE=NVIDIA / 0x8086=Intel
    uint32_t deviceId = 0;
    uint64_t dedicatedVideoMemory = 0;
    uint64_t dedicatedSystemMemory = 0;
    uint64_t sharedSystemMemory = 0;
    bool software = false;   // DXGI_ADAPTER_FLAG_SOFTWARE（WARP / 基本渲染驱动）
    // 该适配器可用的免驱遥测后端（按厂商 + 系统 DLL 存在性判定）；空 = 无有效遥测
    std::string backend;
    // 是否为集成显卡（APU/iGPU）。判据：独显专用显存很小且共享内存远大于它。
    // 只用于决定"GPU 的整片功耗能否当作 CPU 包功耗"，判错时退化为不提供该指标。
    bool integrated = false;
    // ---- 以下仅用于排障（--probe）----
    uint32_t dxgiIndex = 0;  // DXGI EnumAdapters1 的原始下标（排序前的真实位置）
    uint64_t luid = 0;       // 适配器 LUID，拼成 64 位
    // 被折叠的同类条目数（含自身，≥1）。本机实测：同一块 Radeon 780M 会被 DXGI
    // 报成 4 条除 LUID 外完全相同的记录 → 折叠成一条，否则设置页出现 4 个同名项
    int duplicates = 1;
};

// 厂商名（用于 UI 与 --probe）
[[nodiscard]] std::string VendorName(uint32_t vendorId);

// 枚举顺序：有免驱遥测后端的（物理）适配器在前，无遥测的（虚拟/软件）在后；
// 同类内部保持 DXGI 原始顺序（stable_sort）。
[[nodiscard]] std::vector<GpuAdapter> EnumerateGpuAdapters();

// 解析 gpu.selectedIndex：
//   selected < 0  → 自动：取第一个有遥测后端的适配器
//   selected >= 0 → 必须落在范围内且该适配器有遥测后端，否则回退 -1
// 回退时把原因写进 reason。**绝不回退到另一个 GPU 的读数值**（A1 决策②）。
[[nodiscard]] int ResolveSelectedGpu(const std::vector<GpuAdapter>& adapters, int selected,
                                     std::string& reason);

}  // namespace wpm::sensors
