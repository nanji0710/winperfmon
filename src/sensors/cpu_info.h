// SPDX-License-Identifier: MIT
// CPU 静态信息（注册表读取）——基频与品牌串。
//
// 单独成文件的原因：CpuPdhProvider 与 CpuNativeProvider 都需要它们，
// 而注册表路径与"品牌串带尾随空格"这类坑只应有一份实现。
#pragma once

#include <string>

namespace wpm::sensors {

/// `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` 的 `~MHz`（标称基频，MHz）。
/// 读不到返回 0。**注意这是基频，不是当前频率**：AMD 移动 APU 的实测基频为 3801 MHz。
double ReadBaseClockMHz();

/// 同键的 `ProcessorNameString`（如 "AMD Ryzen 7 H 255 w/ Radeon 780M"）。读不到返回空串。
std::string ReadCpuBrand();

/// 逻辑核数（`GetNativeSystemInfo`，含超线程）。
unsigned ReadLogicalCoreCount();

/// 物理核数（`GetLogicalProcessorInformation`，关系为 RelationProcessorCore 的条目数）。
/// 读不到返回 0。
unsigned ReadPhysicalCoreCount();

}  // namespace wpm::sensors
