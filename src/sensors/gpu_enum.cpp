// SPDX-License-Identifier: MIT
#include "sensors/gpu_enum.h"

#include "core/text.h"

#include <windows.h>
// dxgi.h 需要先有 windows.h
#include <dxgi.h>

#include <algorithm>
#include <cstring>

namespace wpm::sensors {
namespace {

// 系统目录下是否存在该 DLL —— 只判断"厂商免驱后端是否在系统里"，
// 不代表一定能读到值（真正能否读到由各 provider 的 Init 决定）。
bool SystemDllExists(const wchar_t* name) {
    wchar_t dir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring p(dir, n);
    p += L"\\";
    p += name;
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 各厂商的免驱遥测后端探测。绝不随包分发这些 DLL/头文件/导入库，
// 一律运行时 LoadLibrary + GetProcAddress（铁律④：无运行时依赖）。
std::string DetectBackend(uint32_t vendorId) {
    switch (vendorId) {
        case 0x1002:  // AMD
            // ADL2：随 AMD 显卡驱动安装到 System32（本机实测可用，非管理员即可读 PMLog）
            if (SystemDllExists(L"atiadlxx.dll")) return "ADL2";
            if (SystemDllExists(L"amdadlx64.dll")) return "ADLX";
            return {};
        case 0x10DE:  // NVIDIA
            // NVML：dll 名固定为 nvml.dll（位于 System32）
            if (SystemDllExists(L"nvml.dll")) return "NVML";
            return {};
        case 0x8086:  // Intel
            // IGCL：驱动随附 ControlLib.dll（SDK 头名 igcl_api.h）
            if (SystemDllExists(L"ControlLib.dll")) return "IGCL";
            if (SystemDllExists(L"igcl.dll")) return "IGCL";
            return {};
        default:
            return {};
    }
}

// 集成显卡判据：专用显存很小、而共享内存远大于它。
// 只服务于一件事——决定"GPU 的整片功耗（ADL ASIC_POWER）能否当作 CPU 包功耗"。
// 判错成 false 只会退化为"不提供该指标"，不会给出错值。
bool LooksIntegrated(const DXGI_ADAPTER_DESC1& d) {
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;
    const uint64_t dvm = d.DedicatedVideoMemory;
    const uint64_t shared = d.SharedSystemMemory;
    return dvm <= (1ull << 30) && shared >= dvm * 4;
}

}  // namespace

std::string VendorName(uint32_t vendorId) {
    switch (vendorId) {
        case 0x1002: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        default: return "Unknown";
    }
}

std::vector<GpuAdapter> EnumerateGpuAdapters() {
    std::vector<GpuAdapter> out;

    IDXGIFactory1* factory = nullptr;
    const HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) return out;

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* ad = nullptr;
        const HRESULT ehr = factory->EnumAdapters1(i, &ad);
        if (ehr == DXGI_ERROR_NOT_FOUND || FAILED(ehr) || ad == nullptr) break;

        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(ad->GetDesc1(&d))) {
            GpuAdapter a;
            a.index = static_cast<int>(i);  // 先记 DXGI 原始下标，排序后重写
            a.dxgiIndex = i;
            a.luid = (static_cast<uint64_t>(static_cast<uint32_t>(d.AdapterLuid.HighPart)) << 32) |
                     static_cast<uint32_t>(d.AdapterLuid.LowPart);
            a.name = text::toUtf8(d.Description);
            a.vendorId = d.VendorId;
            a.deviceId = d.DeviceId;
            a.dedicatedVideoMemory = d.DedicatedVideoMemory;
            a.dedicatedSystemMemory = d.DedicatedSystemMemory;
            a.sharedSystemMemory = d.SharedSystemMemory;
            a.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            a.integrated = LooksIntegrated(d);
            if (!a.software) a.backend = DetectBackend(a.vendorId);
            out.push_back(std::move(a));
        }
        ad->Release();
    }
    factory->Release();

    // 折叠"同一块物理 GPU 的多条记录"：除 LUID 外描述完全相同的视为一块。
    // 本机实测：Radeon 780M 会被 DXGI 报成 4 条同名同 deviceId 的记录，
    // 不折叠的话设置页会出现 4 个一模一样的选项，用户无从选择。
    {
        std::vector<GpuAdapter> uniq;
        uniq.reserve(out.size());
        for (auto& a : out) {
            bool merged = false;
            for (auto& u : uniq) {
                if (u.vendorId == a.vendorId && u.deviceId == a.deviceId && u.name == a.name &&
                    u.software == a.software && u.dedicatedVideoMemory == a.dedicatedVideoMemory) {
                    ++u.duplicates;
                    merged = true;
                    break;
                }
            }
            if (!merged) uniq.push_back(std::move(a));
        }
        out.swap(uniq);
    }

    // 有免驱遥测的（物理）排前面，无遥测的（虚拟/软件）排后面；
    // 同类保持 DXGI 原序 → stable_sort。
    std::stable_sort(out.begin(), out.end(), [](const GpuAdapter& a, const GpuAdapter& b) {
        return (!a.backend.empty()) > (!b.backend.empty());
    });
    // 枚举下标 = 显示顺序（即 gpu.selectedIndex 的取值域）
    for (size_t i = 0; i < out.size(); ++i) out[i].index = static_cast<int>(i);
    return out;
}

int ResolveSelectedGpu(const std::vector<GpuAdapter>& adapters, int selected, std::string& reason) {
    reason.clear();
    if (adapters.empty()) {
        reason = "DXGI 未枚举到任何显示适配器";
        return -1;
    }
    if (selected < 0) {
        for (size_t i = 0; i < adapters.size(); ++i) {
            if (!adapters[i].backend.empty()) {
                reason = "自动：选中枚举下标 " + std::to_string(i) + "（" + adapters[i].name + "，" +
                         adapters[i].backend + "）";
                return static_cast<int>(i);
            }
        }
        reason = "自动：没有适配器带免驱遥测后端";
        return -1;
    }
    if (static_cast<size_t>(selected) >= adapters.size()) {
        // 注意：配置里的 -1 是"自动"，但"显式指定却无法满足"是另一种状态。
        // 这里**不会**顺手改成自动——否则用户盯着独显看，表格里却冒出核显的数字。
        reason = "gpu.selectedIndex=" + std::to_string(selected) + " 越界（本机枚举到 " +
                 std::to_string(adapters.size()) +
                 " 个适配器），已停用 GPU 遥测（想自动选择请把 gpu.selectedIndex 设为 -1）";
        return -1;
    }
    const GpuAdapter& a = adapters[static_cast<size_t>(selected)];
    if (a.backend.empty()) {
        reason = "gpu.selectedIndex=" + std::to_string(selected) + "（" + a.name +
                 "）没有免驱遥测后端，已停用 GPU 遥测；**不会改用其它适配器的读数**"
                 "（想自动选择请把 gpu.selectedIndex 设为 -1）";
        return -1;
    }
    reason = "按配置选中枚举下标 " + std::to_string(selected) + "（" + a.name + "，" + a.backend +
             "）";
    return selected;
}

}  // namespace wpm::sensors
