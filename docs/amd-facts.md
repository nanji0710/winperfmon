# AMD CPU 遥测：8 项技术事实核实记录（+ §9 ADL2 免驱 PM 表实测）

> 本文件是 `amd-cpu.md` §9 待核实清单的核实记录，日期 **2026-10-14**。规则：**每条结论必须带证据；证据形式＝仓库 + 文件路径 + 行号，或官方文档 URL + 章节**；查不到可靠来源的一律写「未找到可靠来源」并给探测式实现建议，不填猜测值。
>
> §9 不在 `amd-cpu.md` §9 的原始待核实清单内，是同日在真机上追加确认的 **ADL2 免驱通道**；其证据形式与本文件前八节不同：不引仓库行号，而是**本机直接采样**（ADL2 PMLog / PDH / DXGI）得到的实测值 + 采用判定。

核实手段：`grep.app` API（给出「仓库 + 路径 + 行号 + 该行原文」）、`api.github.com` 目录/树 API、`raw.githubusercontent.com` / `cdn.jsdelivr.net` 取源码全文、amd.com 官方产品页。注意：本机 HTTPS 在 Windows PowerShell 5.1 下不可用（schannel `SEC_E_NO_CREDENTIALS`），全部取数经外部抓取工具完成。

## 1. SMN 访问通道（PCI `0x60/0x64` vs PawnIO SMN IOCTL）

| 项 | 内容 |
|---|---|
| 结论 | **两者不是二选一，而是「接口」与「实现」的关系**。PawnIO 模块确实导出 SMN/SMU 专用 IOCTL：`AMDFamily17.p` 的 `ioctl_read_smn`（通用 SMN 读，**内部就是** device `0:0:0` 的 PCI 配置空间 `0x60` 地址 / `0x64` 数据间接访问）；`RyzenSMU.p` 另有一套 SMU 专用 IOCTL，内部用 `0xC4/0xC8`（Carrizo 系 `0xB8/0xBC`）。**推荐走 IOCTL**，`0x60/0x64` 只作为自建通道（或模块不可用）时的备选。原文「0x60/0x64 还是 IOCTL」的表述已更正。 |
| 证据 | ① `namazso/PawnIO.Modules` 目录清单（`https://api.github.com/repos/namazso/PawnIO.Modules/contents/`）含 `AMDFamily17.p`（8004 B）、`RyzenSMU.p`（27722 B）。② `namazso/PawnIO.Modules:AMDFamily17.p`：`SMN_INDEX_OFFSET (0x60)`、`SMN_DATA_OFFSET (0x64)`；`DEFINE_IOCTL_SIZED(ioctl_read_smn, 1, 1)` 在 `AMDFamily17.p:193`（in[0]=SMN 地址 → out[0]=值），实现先 `pci_config_read_dword(0,0,0,0,...)` 校验厂商 `0x1022`，再 `pci_config_write_dword(0,0,0,SMN_INDEX_OFFSET,in[0])` + `pci_config_read_dword(0,0,0,SMN_DATA_OFFSET,out[0])`；`@warning You should acquire the "\\BaseNamedObjects\\Access_PCI" mutant before calling this` 在 `AMDFamily17.p:192`。③ `namazso/PawnIO.Modules:RyzenSMU.p`：`const SMU_PCI_ADDR_REG = 0xC4; const SMU_PCI_DATA_REG = 0xC8; const SMU_PCI_CZ_ADDR_REG = 0xB8; const SMU_PCI_CZ_DATA_REG = 0xBC;`。④ 上层调用约定：`LibreHardwareMonitor/LibreHardwareMonitor:LibreHardwareMonitorLib/PawnIo/AmdFamily17.cs:9` `long[] result = _pawnIo.Execute("ioctl_read_smn", [offset], 1);`；`irusanov/ZenStates-Core:PawnIo/AmdFamily17.cs:8-9`（`IOCTL_READ_SMN = "ioctl_read_smn"`、`IOCTL_READ_MSR = "ioctl_read_msr"`）与 `:45` `ExecuteHr(IOCTL_READ_SMN, input, 1, output, 1, out uint returnSize)`；`a1ive/nwinfo:ioctl/ioctl.c:1183-1184`。 |
| 置信度 | 已证实 |
| 对设计的影响 | 改 `amd-cpu.md` §3 矩阵「Ring0 · SMN」行、§5.5 表格；§9 第 1 行关闭 |

## 2. Tdie offset 的来源、符号方向与 X3D 特例

| 项 | 内容 |
|---|---|
| 结论 | offset **不来自 SMU PM 表**，有两个独立来源：**(a) 通用 −49 °C 量程偏移**——`THM_TCON_CUR_TMP` 的 bit19 `CUR_TEMP_RANGE_SEL` 置位、或 bits[17:16] `CUR_TEMP_TJ_SEL == 0b11` 时，读数要 **减 49 °C**（BIOS 写入，EPYC/服务器常见）；**(b) 按型号常数表**——只覆盖 17h 的 X / Threadripper 型号（1600X/1700X/1800X +20 °C、2700X +10 °C、Threadripper 19xx/29xx +27 °C）。**符号方向：寄存器原始值即 Tctl，Tdie = Tctl − offset**（权威实现把 offset 记为负值再加）。**X3D/19h/1Ah 的专属 offset：未找到可靠来源**——所有公开表里都没有 19h/1Ah/X3D 条目，因此这些型号按 offset = 0（Tctl == Tdie）处理。原文 §5.2「X3D 型号的 Tctl offset 与普通型号不同」**已更正**：X3D 的实测差异体现在 **Tjmax（90/89 °C）**，不是 Tctl offset。 |
| 证据 | ① `torvalds/linux:drivers/hwmon/k10temp.c:75-77`（`ZEN_CUR_TEMP_SHIFT 21` / `ZEN_CUR_TEMP_RANGE_SEL_MASK BIT(19)` / `ZEN_CUR_TEMP_TJ_SEL_MASK GENMASK(17, 16)`）、`:189-191`（`if ((regval & data->temp_adjust_mask) \|\| (regval & ZEN_CUR_TEMP_TJ_SEL_MASK) == ZEN_CUR_TEMP_TJ_SEL_MASK) temp -= 49000;`）、`:465-467`（is_zen → `temp_adjust_mask = ZEN_CUR_TEMP_RANGE_SEL_MASK`）；`:117-121` `struct tctl_offset { u8 model; char const *id; int offset; };`；`:123-129` 6 行表（20/20/20/10/27/27 °C，单位 millidegree）；`:537` 匹配循环（family + 品牌串 `strstr`）。② 同表被多实现照抄：`ocerman/zenpower:zenpower.c:107-113`、`mattkeenan/zenpower5:zenpower_core.c:161-167`、`trulyspinach/SMCAMDProcessor:AMDRyzenCPUPowerManagement/AMDRyzenCPUPowerManagement.cpp:7-13`（匹配逻辑 `:309-312`）、`macos86/SMCProcessorAMD:SMCProcessorAMD/SMCProcessorAMD.cpp:7-13`。③ 符号证据：`memtest86plus/memtest86plus:system/hwquirks.c:170-171` 表中直接存负值 `{ 0x8, 0x0, -20.0f, "AMD Ryzen 5 1600X" }, // Summit Ridge`，应用式在 `system/x86/temperature.c:779` `return cpu_temp_offset + 0.125f * (float)((regl >> 21) & 0x7FF);`，−49 偏移的通用检测在 `temperature.c:754`（`cpu_temp_offset += -49.0f;`）；`hwquirks.c:178-179` 注释明说「EPYC Milan/Genoa/Siena/Turin need no entry here」。④ `a1ive/nwinfo:ioctl/ryzen_smu.h:183-185`（`F17H_TEMP_OFFSET_FLAG 0x80000`、`F1AH_TEMP_OFFSET_FLAG 0x30000`、`float ryzen_smu_get_temp_offset(...)`）、`libnw/cpu/amd_cpu.c:393-396`（`float offset = ryzen_smu_get_temp_offset(...); ... return 0.001f * ((temperature >> 21) * 125) + offset;`）。⑤ LKML：<https://www.spinics.net/lists/linux-hwmon/msg19427.html>（引 AMD 规范：TJ_SEL==3 且 RANGE_SEL==0 时为 `CurTmp*0.125 − 49` °C）。⑥ 温度寄存器：`k10temp.c:67-68`（`ZEN_REPORTED_TEMP_CTRL_BASE 0x00059800`，注释「Common for Zen CPU families (Family 17h and 18h and 19h and 1Ah)」）；`LibreHardwareMonitorLib/Hardware/Cpu/Amd17Cpu.cs:815-817`、`:189-190`。 |
| 置信度 | 机制与符号方向：**已证实**；X3D / 19h / 1Ah 专属 offset：**未找到** |
| 对设计的影响 | 改 §5.2 第 2、5 行；§9 第 2 行关闭（X3D 部分改为运行期探测） |

## 3. AMD 上 IA32_APERF(0xE7)/IA32_MPERF(0xE8) 是否可用

| 项 | 内容 |
|---|---|
| 结论 | **不可用**。`IA32_APERF (0xE7)` / `IA32_MPERF (0xE8)` 是 Intel 定义的 MSR：Linux 只在 Intel 型号匹配表（`X86_MATCH_VFM_FEATURE`）里授予 `X86_FEATURE_APERFMPERF`，perf 的 APERF/MPERF 事件也以该特性为前提，AMD 路径从不设置它。AMD 自己的对应物是 `MSRC000_00E7`(MPERF_RO `0xC00000E7`) / `MSRC000_00E8`(APERF_RO `0xC00000E8`)，PawnIO 只读白名单里可读，但**未找到任何权威实现**（LibreHardwareMonitor / OpenHardwareMonitor / ZenStates / RyzenAdj）用它算有效频率——LHM 的 AMD 频率来自 PState FID/DID 与 PM 表。原文 §4/§5.1 把 APERF/MPERF 当 AMD 首选频率路径**已更正**。 |
| 证据 | ① `torvalds/linux:arch/x86/include/asm/cpufeatures.h:100` `#define X86_FEATURE_APERFMPERF ( 3*32+28) /* "aperfmperf" P-State hardware coordination feedback capability (APERF/MPERF MSRs) */`。② `arch/x86/kernel/cpu/aperfmperf.c:128-129` `#define X86_MATCH(vfm) ... X86_MATCH_VFM_FEATURE(vfm, X86_FEATURE_APERFMPERF, NULL)`（唯一授予点，Intel VFM 表）。③ `arch/x86/events/msr.c:21-23`（`test_aperfmperf()` → `boot_cpu_has(X86_FEATURE_APERFMPERF)`）与 `:84-85`（`[PERF_MSR_APERF] = { MSR_IA32_APERF, &group_aperf, test_aperfmperf, }`、`[PERF_MSR_MPERF] = { MSR_IA32_MPERF, ... }`）；`arch/x86/kvm/x86.h:766-768` 同前提。④ `namazso/PawnIO.Modules:AMDFamily17.p` 只读白名单 `MSR_MPERF_RO 0xC00000E7` / `MSR_APERF_RO 0xC00000E8`。⑤ 反证：grep.app 搜字面量 `0xC00000E7` 的 530 条命中**全部**是 Windows NTSTATUS `STATUS_BAD_DESCRIPTOR_FORMAT`，无一条 AMD MSR 用法。⑥ `LibreHardwareMonitorLib/Hardware/Cpu/Amd17Cpu.cs:439-447` 的 AMD 频率分支只用 FID/DID，文件内无 APERF/MPERF。 |
| 置信度 | 「0xE7/0xE8 在 AMD 上不作为有效频率来源」：**已证实**；「0xC00000E7/E8 差分能否等价替代」：**未找到** |
| 对设计的影响 | §4 `cpu.freq.avg` 与 §5.1 第 1 条改为「PM 表每核频率为首选」；§9 第 3 行关闭 |

## 4. Zen5（family 1Ah）的 CoreCOF 公式

| 项 | 内容 |
|---|---|
| 结论 | 17h/19h 维持 `CoreCOF = 200 MHz × CpuFid / CpuDid`（`CpuFid = bits[7:0]`、`CpuDid/DfsId = bits[13:8]`）；**1Ah（Zen5）改为 `CoreCOF(MHz) = CpuFid × 5`，且 `CpuFid` 扩到 `bits[11:0]`（12 位）**。原文 §5.1 猜测的 `CpuFid × 50 MHz` **已更正为 `× 5 MHz`**（差一个数量级），依据下方 LHM 源码。参照实现返回值的单位是 100 MHz（17h 分支 `2.0 * Fid / DfsId`）。 |
| 证据 | `LibreHardwareMonitor/LibreHardwareMonitor:LibreHardwareMonitorLib/Hardware/Cpu/Amd17Cpu.cs:439-447`：`:439` `//zen 5`、`:440` `uint cpuFid = eax & 0xfff;`、`:441` `return (cpuFid * 5) / 100.0;`、`:443` `else`、`:445` `uint cpuDfsId = (eax >> 8) & 0x3f;`、`:446` `uint cpuFid = eax & 0xff;`、`:447` `return 2.0 * cpuFid / cpuDfsId;`。旁证（同为 17h 公式）：`openhardwaremonitor/openhardwaremonitor:Hardware/CPU/AMD17CPU.cs:187-189`。自检：5800X 若 Fid=0xBC(188)、DfsId=8 → 200×188/8 = 4700 MHz；Zen5 若 Fid=0x3AC(940) → 940×5 = 4700 MHz。 |
| 置信度 | 已证实（权威开源实现源码）；AMD 官方文档级出处：**未找到**（PPR 未公开可引章节） |
| 对设计的影响 | 改 §5.1 第 3 条；§9 第 4 行关闭 |

## 5. SMU 邮箱三元组与「获取 PM 表」命令号

| 项 | 内容 |
|---|---|
| 结论 | **邮箱三元组（msg / arg / rsp）没有「家族唯一值」，而是按 SMU 接口版本/型号分组**。跨来源一致的主集合：17h 与 19h 桌面、19h threadripper/服务器 = `0x3B10528 / 0x3B10998 / 0x3B10564`；APU（Vangogh/Rembrandt/Phoenix/Mendocino/Hawk Point/Strix Halo 等）第二组 = msg `0x3B10528` **rsp `0x3B10578`**、arg `0x3B10998`；第三组（Strix Halo/Glinda 类）= `0x3B10928 / 0x3B10998 / 0x3B10978`；PSMU 备用平面 = `0x3B10A20 / 0x3B10A88 / 0x3B10A80`。**arg base 恒为 `0x3B10998`**。PM 表命令号（PawnIO `RyzenSMU.p` 逐 codename 实现，见下表）与「一条命令」的直觉不同：**取版本 / 取基址 / 刷新到 DRAM 是三条不同命令**，家族分组不同。注意原文 §6 示例把 `pmTableCmd` 写成 `0x66`：`0x66` 属 **Renoir/Cezanne/Phoenix 等 APU 组**，不是 19h 桌面（Vermeer = `0x06`、Raphael = `0x04`）——**已更正**。 |
| 证据 | ① `FlyGoat/RyzenAdj:lib/nb_smu_ops.c:8-20`（`MP1_C2PMSG_MESSAGE_ADDR_1 0x3B10528`、`MP1_C2PMSG_RESPONSE_ADDR_1 0x3B10564`、`MP1_C2PMSG_ARG_BASE_1 0x3B10998`、注释 `/* For Vangogh and Rembrandt */` 后 `MESSAGE_ADDR_2 0x3B10528` / `RESPONSE_ADDR_2 0x3B10578` / `ARG_BASE_2 0x3B10998`、`RESPONSE_ADDR_3 0x3b10978` / `ARG_BASE_3 0x3b10998`）。② `coreboot/coreboot`：`src/soc/amd/cezanne/include/soc/smu.h:8-9`、`src/soc/amd/picasso/include/soc/smu.h:8-9`（rsp `0x3b10564`、args `0x3b10998`）、`src/soc/amd/mendocino/include/soc/smu.h:8-9`、`src/soc/amd/phoenix/include/soc/smu.h:8-9`（rsp `0x3b10578`）、`src/soc/amd/strix_halo/include/soc/smu.h:10-11`、`src/soc/amd/glinda/include/soc/smu.h:10-11`（rsp `0x3b10978`、args `0x3b10998`）。③ `amkillam/ryzen_smu:smu.c:506-508`（IF_VERSION_9：`addr_mp1_mb_cmd = 0x3B10528`、`addr_mp1_mb_rsp = 0x3B10564`、`:517-519` args `0x3B10998`）、`:536-540`、`:547-548`（IF_VERSION_10/12，Vangogh-Rembrandt 组 rsp `0x3B10578`）；`leogx9r/ryzen_smu:smu.c:359-361,368-370,384-385` 同模式。④ `JamesCJ60/Universal-x86-Tuning-Utility:Universal x86 Tuning Utility/Scripts/AMD Backend/RyzenSmu.cs:49-51,144-145,229-230,238-239`。⑤ `yunusemreyl/omen-space:src/omen-space-daemon/src/ryzen.rs:171-183`（含 PSMU 平面 `psmu_msg 0x3B10A20` / `psmu_rsp 0x3B10A80` / `psmu_arg 0x3B10A88`）。⑥ 命令号：`namazso/PawnIO.Modules:RyzenSMU.p` 的 `get_pm_table_version`（`:436` 起，按 codename switch）、`get_pm_table_base`（`:486` 起）、`transfer_table_to_dram`、`get_smu_version`（cmd `0x02`，arg[0]=1）。 |
| 置信度 | 地址集合与命令号存在性：**已证实**（多来源一致）；「家族 → 三元组」的精确映射：**推断**（APU 的 rsp 在不同来源有 `0x3B10564` / `0x3B10578` 两种，需运行期择优） |
| 对设计的影响 | 改 §5.6 第 1 步（三元组改为「候选集合 + 逐个试」）、§6 示例的 `msg/arg/rsp` 与 `pmTableCmd`；§9 第 5 行关闭 |

**逐家族/型号组的 PM 表命令号**（全部来自 `namazso/PawnIO.Modules:RyzenSMU.p` 的 codename 分支）：

| codename 组 | 取版本 | 取基址 | 刷新/传输到 DRAM |
|---|---|---|---|
| SummitRidge / PinnacleRidge / Colfax / Threadripper / Naples（17h 早期 + HEDT） | `0x0D` | `0x0B`、`0x0C`、`0x0E` | `0x0A` |
| RavenRidge / RavenRidge2 / Dali / Picasso / FireFlight（17h APU） | `0x0C` | `0x0A`、`0x3D`、`0x0B` | `0x3D`（arg=3） |
| Matisse / Vermeer / CastlePeak / Rome / Chagall / Milan（Zen2/Zen3 桌面与服务器） | `0x08` | `0x06` | `0x05` |
| Raphael / Genoa / StormPeak / DragonRange / GraniteRidge / Bergamo / Turin / TurinD / ShimadaPeak（Zen4/Zen5） | `0x05` | `0x04` | `0x03` |
| Renoir / Rembrandt / Cezanne / Mero / Vangogh / Phoenix / Phoenix2 / HawkPoint / Mendocino / StrixHalo / StrixPoint / KrackanPoint / Lucienne（Zen2/Zen3/Zen4/Zen5 APU） | `0x06` | **`0x66`** | `0x65`（arg=3） |
| Carrizo / BristolRidge / StoneyRidge（15h，仅作降级参考） | `0x40` | `0x2D`、`0x2C` | `0x2D` 然后 `0x2C`（arg=1） |

## 6. PawnIO.Modules 里与 AMD / 通用 MSR 相关的模块

| 项 | 内容 |
|---|---|
| 结论 | 通用 MSR 模块：`IntelMSR.p`、`ARMMSR.p`、`ZhaoxinMSR.p`；AMD 相关：`AMDFamily17.p`（门禁 `family ∈ [0x17, 0x1A]`，即 **17h/19h/1Ah 一个模块全覆盖**，无独立的 19h/1A 模块）、`RyzenSMU.p`（门禁 `family ∈ {0x15,0x17,0x19,0x1A}`，SMU/PM 表专用）、`AMDFamily0F.p`、`AMDFamily10.p`、`AMDReset.p`。**导出 IOCTL 与调用约定**：`AMDFamily17.p` → `ioctl_read_msr`（in 1 → out 1）、`ioctl_write_msr`（in 2 → out 0）、`ioctl_read_smn`（in 1 → out 1）；`RyzenSMU.p` → `ioctl_resolve_pm_table`（in 0 → out 2＝[版本, 表基址]）、`ioctl_update_pm_table`（0→0）、`ioctl_read_pm_table`（in 0 → out = 表内容 QWORD 数组）、`ioctl_get_code_name`（0→1）、`ioctl_get_smu_version`（0→1）、`ioctl_read_smu_register`（1→1）、`ioctl_write_smu_register`（2→0）、`ioctl_send_smu_command`（**in 7＝[cmd,arg0..arg5] → out 6**）。全部 `DEFINE_IOCTL_SIZED(name, in_count, out_count)`，输入/输出均为 64 位数组，调用方显式传 in/out 个数并取返回长度；触碰 PCI 的 IOCTL **调用前须获取 `\BaseNamedObjects\Access_PCI` 互斥体**。许可 LGPL-2.1-or-later（仓库 `COPYING`）。 |
| 证据 | ① 目录清单 `https://api.github.com/repos/namazso/PawnIO.Modules/contents/`（文件名单与字节数）。② `AMDFamily17.p`：家族门禁 `if (family < 0x17 \|\| family > 0x1A) return STATUS_NOT_SUPPORTED;`、`DEFINE_IOCTL_SIZED(ioctl_read_msr, 1, 1)` / `(ioctl_write_msr, 2, 0)` / `(ioctl_read_smn, 1, 1)`（后者定义在 `:193`）、`@warning ... Access_PCI mutant` 在 `:192`。③ `RyzenSMU.p`：`SMUStatus { SMU_Busy = 0x00, SMU_OK = 0x01, SMU_CmdRejectedBusy = 0xFC, SMU_CmdRejectedPrereq = 0xFD, SMU_UnknownCmd = 0xFE, SMU_Failed = 0xFF }` + `smu_status_to_nt()`；`send_command(msg, args[6])` 六步握手（等 rsp 非 0 → 清 rsp → 写 6 个 arg → 写 msg → 轮询 rsp → 回读 6 个 arg，重试上限 `SMU_RETRIES_MAX = 8096`）；`check_smu_register_range` 白名单 `0x3B10000–0x3B10FF0`（邮箱）、`0x56000–0x5AFFF`、`0x6F000–0x6FFFF`（SVI2 平面）。④ 上层调用：`LibreHardwareMonitorLib/PawnIo/AmdFamily17.cs:9-10`、`irusanov/ZenStates-Core:PawnIo/AmdFamily17.cs:8-9,45`、`a1ive/nwinfo:ioctl/ioctl.c:1183-1184`（`pio = &drv->pio_amd17; pif = "ioctl_read_smn";`）。 |
| 置信度 | 已证实 |
| 对设计的影响 | 改 §3 矩阵、§5.5 表格、§5.6（可直接用 `ioctl_send_smu_command` + `ioctl_resolve_pm_table` 替代自建邮箱时序）；§9 第 6 行关闭 |

## 7. 各家族/型号的温度上限（TjMax 等效）

| 项 | 内容 |
|---|---|
| 结论 | **AMD 没有 Intel `MSR_IA32_TEMPERATURE_TARGET (0x1A2)` 那样的 Tjmax 寄存器**，家族级常数也不存在 → Tjmax 只能**按 SKU 从 amd.com 官方规格页取**（字段名 `Max. Operating Temperature (Tjmax)`）。已逐条抓到的官方值：**9950X（Zen5, 1Ah）= 95 °C**、**7800X3D（Zen4, 19h X3D）= 89 °C**、**5800X3D（Zen3, 19h X3D）= 90 °C**、**Ryzen 7 PRO 2700X（Zen+, 17h）= 95 °C**。9800X3D 一致性零售页显示 95 °C（未逐条抓官方页，标推断）。原文 §5.2「19h 95/115 °C」**已更正**：「115 °C」**未找到可靠来源**，且家族级常数与实测不符（同属 19h 的 5800X3D=90 与 7800X3D=89 不同）。寄存器里的 `CUR_TEMP_TJ_SEL (bits[17:16])` 只决定量程/`−49 °C` 偏移，**不是**可直接读出的 Tjmax 数值。 |
| 证据 | ① amd.com 官方产品页：<https://www.amd.com/en/products/processors/desktops/ryzen/9000-series/amd-ryzen-9-9950x.html>（General Specifications → `Max. Operating Temperature (Tjmax) 95°C`）；<https://www.amd.com/en/products/processors/desktops/ryzen/7000-series/amd-ryzen-7-7800x3d.html>（`89°C`）；<https://www.amd.com/en/products/processors/desktops/ryzen/5000-series/amd-ryzen-7-5800x3d.html>（`90°C`）；<https://www.amd.com/en/support/downloads/drivers.html/processors/ryzen-pro/ryzen-pro-2000-series/amd-ryzen-7-pro-2700x.html>（`95°C`，1000/2000 系列产品页多已下线，PRO 页仍保留同一规格表）。② 反证「没有可读的 Tjmax」：`memtest86plus/memtest86plus:system/x86/temperature.c:693`（`static int TjMax = 0;`）与 `:716-740`（只对 Intel 走 `MSR_IA32_TEMPERATURE_TARGET` 读 `(regl >> 16) & 0x7F`，AMD 分支不设 TjMax）；grep.app 搜 `tjmax_table` 只命中 Intel 的 `torvalds/linux:drivers/hwmon/coretemp.c`。③ 通用 −49 °C 量程机制：见本文件 §2 证据 ①③。 |
| 置信度 | 已抓取的 4 个 SKU：**已证实**；未逐条抓取的型号（含 9800X3D、APU）：**推断**；家族级常数 / 115 °C：**未找到** |
| 对设计的影响 | 改 §5.2 第 4 行、§4 `cpu.temp.headroom` 行、§6 `tempLimitC` 语义（必须按 SKU 匹配，匹配不到就不可用）；§9 第 7 行改为「一手来源＝官方规格页，需随包维护 SKU 表」 |

## 8. 「功耗读数翻倍」现象的范围与判据

| 项 | 内容 |
|---|---|
| 结论 | **「同一物理域存在两个能量 MSR、读数成 2 倍关系」这一前提未找到可靠来源（AMD 侧无一手证据）**，原文 §5.3 的「长期 ≈ 2.0× 就自动折半」**已更正**为「校验式诊断，不静默改数」。已证实会产生 2× 级误差的是**软件聚合/单位问题**：① **域叠加求和**——`PKG` 已包含 `CORE/PP0`，再相加即重复计数（CodeCarbon 文档以 Intel 为例的「Triple counting」）；② AMD 上 `0xC001029A` 是 core/PP0 域计数、`0xC001029B` 是 package 域计数，**不是同一物理域**；③ 每个逻辑处理器读同一个 package 计数再求和 → N 倍；④ 能量单位 `ESU` 取错（必须从 `0xC0010299` bits[12:8] 动态取，`1/2^ESU` J）。**判据**：同一 MSR 前后差分才做比值；跨域/跨线程求和要禁止；与 PM 表 PPT 长期比值只作为诊断信号（长期 ≈2.0 时记录诊断并提示可能误聚合，而不是自动折半）。 |
| 证据 | ① `torvalds/linux:arch/x86/events/rapl.c:598` `[PERF_RAPL_PKG] = { MSR_AMD_PKG_ENERGY_STATUS, &rapl_events_pkg_group, test_msr, false, RAPL_MSR_MASK }`（AMD 只注册 PKG 域）。② `drivers/powercap/intel_rapl_msr.c:121-123`（AMD：单位 MSR `MSR_AMD_RAPL_POWER_UNIT`、PKG `MSR_AMD_PKG_ENERGY_STATUS`、PP0/CORE `MSR_AMD_CORE_ENERGY_STATUS`）。③ `tools/power/x86/turbostat/turbostat.c:680-698`（`RAPL_AMD_PKG_ENERGY_STAT = BIT(16) /* 0xc001029b */`）。④ `djselbeck/rapl-read-ryzen:ryzen.c`（`AMD_MSR_PWR_UNIT 0xC0010299` / `AMD_MSR_CORE_ENERGY 0xC001029A` / `AMD_MSR_PACKAGE_ENERGY 0xC001029B`；`time_unit=(x&0xF0000)>>16; energy_unit=(x&0x1F00)>>8; power_unit=x&0xF;`，换算 `pow(0.5, unit)`）。⑤ AMD 官方文档指针：`ocerman/zenmonitor:src/ss/msr.c:57` 注释 `AMD OSRR: page 139 - MSRC001_0299`；`coreboot/coreboot:util/amdtool/cpu.c:468-470`（`0xc0010299 RAPL_PWR_UNIT`、`0xc001029a CORE_ENERGY_STS`）。⑥ 域叠加导致翻倍的实例：CodeCarbon RAPL 文档 <https://docs.codecarbon.io/latest/explanation/rapl>（「RAPL domains are hierarchical and overlapping. Summing them causes severe over-counting!」）；winsiderss/systeminformer Issue #2001 <https://github.com/winsiderss/systeminformer/issues/2001>（「RAPL seems to overstate the power usage by a factor of two」，Intel i5-12600K / i5-1145G7，成因是「sum of all CPU power domains and the package power」）。⑦ AMD RAPL 的历史：Phoronix <https://www.phoronix.com/news/Google-RAPLs-AMD-Zen>（Google 补丁才让 Zen1/Zen2 暴露封装级 RAPL，此前只支持 Intel）。 |
| 置信度 | 软件聚合与单位导致 2× 级误差：**已证实**；「AMD RAPL 硬件本身给出 2× 读数」：**未找到** |
| 对设计的影响 | 改 §5.3 第 2 条、§8 单测项（翻倍自检改为「聚合与单位校验」）；§9 第 8 行关闭 |

## 9. ADL2 免驱 PM 表实测（2026-10-14，HP ZHAN66 14 G8a / Ryzen 7 H 255）

> 本节是结论摘要；**索引 0–45 的原始核实记录（含 §0 更正公告）在 `docs/adl2-facts.md`**。

**测试机**：

| 项 | 实测值 |
|---|---|
| 机型 / APU | HP ZHAN66 14 G8a；AMD Ryzen 7 H 255 w/ Radeon 780M（Family 25 = `0x19`、Model 117 = `0x75`，Hawk Point Zen4 APU / RDNA3） |
| 系统 | Windows 11 build 26300.9711；HVCI **已启用** |
| 权限 | **非管理员**（IsAdmin=False）；**未安装 PawnIO** |
| 已在系统里的厂商 DLL | `C:\Windows\System32\atiadlxx.dll` 存在（2,508,304 字节、2734 个导出）；`amdadlx64.dll` 存在 |
| 不存在的 DLL | `nvml.dll`、`igcl.dll`、`ze_loader.dll` 均不存在 |

**调用范式**（全部在运行期 `LoadLibraryW` + `GetProcAddress`，不链接、不分发）：

| 步骤 | 事实 |
|---|---|
| 建上下文 | `ADL2_Main_Control_Create(malloc 回调, enumConnected=1, &ctx)`——**第一个参数必须是 malloc 回调**；传 `nullptr` 返回 `rc = -11`（`ADL_ERR_INVALID_RESOURCE`） |
| 取适配器数 | `ADL2_Adapter_NumberOfAdapters_Get` 返回 **5** |
| 取数据 | `ADL2_New_QueryPMLogData_Get(ctx, iAdapterIndex, &data)` 返回 `rc = 0` |
| 结构体 | 必须先置 `iSize`：`AdapterInfo.iSize`、`ADLPMLogDataOutput.iSize = sizeof(...)`；`ADLSingleSensorData { int supported; int value; }`，数组 `sensors[256]` |
| 支持性判定 | **只能看 `ADLSingleSensorData.supported`**。`ADL2_New_QueryPMLogSupport_Get` **不可用**：它返回的结构不是「按索引的 bool 数组」，不能拿它查支持性 |
| 适配器绑定 | `ADL2_Adapter_NumberOfAdapters_Get` → `ADL2_Adapter_AdapterInfo_Get` 取 `strUDID`（形如 `PCI\VEN_1002&DEV_1900&SUBSYS_...&REV_C1`）→ 与 DXGI 的 vendorId/deviceId 匹配 → `ADL2_Adapter_Active_Get` 取绑定下标 |

**本机可用传感器索引共 20 个**：`1 2 3 7 16 17 18 19 21 23 28 29 30 31 32 33 34 35 38 39`。

**索引语义表**（名称取自 Linux HWMonitor 的 `AtiAdlxx.cs` 枚举，经本机负载实测确认。「仅交叉校验」＝读数只用于验证同域 U/I/P 自洽，不进指标目录）：

| 索引 | 名称 | 单位 | 本机实测范围 | 是否采用及理由 |
|---:|---|---|---|---|
| 1 | `CLK_GFXCLK` | MHz | 空闲 800 → GPU 满载 2267–2437 | **采用**：`gpu.freq.core` |
| 2 | `CLK_MEMCLK` | MHz | 1000–2800 | **采用**：`gpu.freq.mem`（APU 上即共享系统内存时钟） |
| 3 | `CLK_SOCCLK` | MHz | 400–1028 | 未采用 |
| 7 | `CLK_VCNCLK` | MHz | 800 | 未采用 |
| 16 | `SOC_VOLTAGE` | mV | 765–979 | 仅交叉校验 |
| 17 | `SOC_POWER` | W | 1–3 | **采用**：`cpu.power.soc` |
| 18 | `SOC_CURRENT` | A | 2–3 | 仅交叉校验 |
| 19 | `INFO_ACTIVITY_GFX` | % | 空闲 0–8 → GPU 满载 98–99 | **采用**：`gpu.usage`，本机唯一干净的 GPU 负载信号 |
| 21 | `GFX_VOLTAGE` | mV | 1005–1107 | 仅交叉校验 |
| 23 | `ASIC_POWER` | W | 空闲 7 → CPU 满载 32–33 → GPU 满载 42 | **采用**：核显上作为 `cpu.power.package`（整片功耗） |
| 28 | `TEMPERATURE_GFX` | °C | 空闲 43–45 → GPU 满载 67 | **采用**：`gpu.temp.core` |
| 29 | `TEMPERATURE_SOC` | °C | 63–64 | 未采用 |
| 30 | `GFX_POWER` | W | 空闲 1 → CPU 满载 24–34 → GPU 满载 35–41 | **核显上不采用**（读数被 CPU 负载污染，见下节）；独显上作为 GFX 供电轨读数照常采用 |
| 31 | `GFX_CURRENT` | A | 24–28 | 仅交叉校验 |
| 32 | `TEMPERATURE_CPU` | °C | 空闲 43–50 → 满载 74–79 | **采用**：`cpu.temp.package`（AMD 免驱主路径） |
| 33 | `CPU_POWER` | W | 0–6 | **不采用**：量级与 28–45 W 的 TDP 级明显不符 |
| 34 | `CLK_CPUCLK` | MHz | 空闲 3775 → 满载 4625 | **采用**：`cpu.freq.avg`（免驱主路径；**不碰 perflib**，见下"显存用量导出"节） |
| 35 | `THROTTLER_STATUS` | 位域 | 恒 1 | 未采用：ADL2 只给位域、**未做位→文案映射**，所以 `gpu.throttle.reason` 仍不提供 |
| 38 | `SMART_POWERSHIFT_CPU` | — | 恒 0 | 未采用 |
| 39 | `SMART_POWERSHIFT_DGPU` | — | 恒 0 | 未采用 |

**反伪造：核显上 `GFX_POWER` 不是 GPU 功耗**。实验＝`cpuload.exe 22 8`（8 线程纯 CPU 满载）期间每秒采样一次 ADL2 PMLog，共 18 秒：

| 传感器 | CPU 满载期间读数 | 含义 |
|---|---|---|
| `INFO_ACTIVITY_GFX`(19) | 仅 1–8% | GPU 完全空闲 |
| `CLK_GFXCLK`(1) | 恒 800 MHz | GPU 完全空闲 |
| `GFX_POWER`(30) | 24–34 W | 被 CPU 负载抬高 |
| `ASIC_POWER`(23) | 32–33 W | 与 30 几乎重合 |
| `TEMPERATURE_CPU`(32) | 75–79 °C | CPU 满载 |

- 结论：核显（APU）上 CPU 与 GPU **共享供电轨**，ADL2 的 `GFX_POWER` 会被 CPU 负载抬高到与 `ASIC_POWER` 几乎重合。把它当「GPU 功耗」显示，用户会在 **GPU 空载时看到 30 W**——属于会被误读的假数据。
- 因此本项目在**核显**上对 `gpu.power` 返回「不可用」，并给出理由原文：「核显与 CPU 共享供电轨：ADL2 GFX_POWER 在 GPU 空载、CPU 满载时仍报 24–34 W，无法作为 GPU 功耗（整片功耗见 cpu.power.package）」。独显上 `GFX_POWER` 作为 GFX 供电轨读数照常采用。
- 自洽性交叉验证（说明这些索引不是随机噪声）：`SOC_VOLTAGE`(16) × `SOC_CURRENT`(18) = 0.77 V × 2 A ≈ 1.5 W ≈ `SOC_POWER`(17) 的 1–3 W；`GFX_VOLTAGE`(21) × `GFX_CURRENT`(31) = 1.0 V × 26 A ≈ 26 W ≈ `GFX_POWER`(30) 的 26 W。
- **CPU 功耗口径**：核显上 `cpu.power.package` 取 `ASIC_POWER`(23)（整片功耗，含 GPU 与 uncore；实测 CPU 满载 32–33 W、GPU 满载 42 W，落在 Ryzen 7 H 255 的 28–45 W TDP 级），**不取 `CPU_POWER`(33)**（0–6 W，量级不符）。

**ACPI 热区作为兜底**（PDH 计数器集 `Thermal Zone Information`，免驱、免管理员）：

| 项 | 实测 |
|---|---|
| 本机热区 | 9 个：`_TZ.CPUZ`、`_TZ.GFXZ`、`_TZ.CHGZ`、`_TZ.PGTZ`、`_TZ.EXTZ`、`_TZ.LOCZ`、`_TZ.BAGZ`、`_TZ.BATZ`、`_TZ.MSHZ` |
| 换算 | `_TZ.CPUZ\High Precision Temperature` 原始值是 **0.1 K**：`°C = v/10 − 273.15`；非管理员即可读；分辨率约 1 °C |
| 与 ADL2 同时刻对照 | 8 线程 CPU 满载 22 秒：ACPI `48.1 → 60.1 °C`（1 °C 步进、明显滞后，约 1 °C / 2 秒）；ADL2 的 `TEMPERATURE_CPU` 在 1 秒内 `49 → 73–76 °C`，随后稳定在 54–67 °C |
| 可用性陷阱 | `_TZ.GFXZ` 恒 25.1 °C（ACPI 表里有、底层没接）→ **必须做「是否随负载变化」的可用性判断**，不能「有热区就当温度源」 |
| 哨兵值 | `2732`（＝0.0 °C）与 `0` 是「无数据」哨兵，不是温度 |
| 权限对比 | `MSAcpi_ThermalZoneTemperature` 需要管理员，而 `Thermal Zone Information` 计数器**不需要** |

结论：**ADL2 的 `TEMPERATURE_CPU` 响应快得多，作为 `cpu.temp.package` 的主路径；ACPI 仅作兜底**（provider 只在槽位仍为空时写入），并且在 Intel 或没有厂商 PM 表的机器上才轮到它。

**GPU 枚举（DXGI）与核显显存口径**：

- 用 DXGI（`CreateDXGIFactory1` + `EnumAdapters1`）而不是 WMI 的 `Win32_VideoController`：后者要 COM + WmiPrvSE 进程，冷启动开销与常驻内存远超 DXGI（本项目预算：冷启动 ≤ 300 ms、常驻 ≤ 15 MB）。
- 本机 DXGI 报 **5 条适配器**：4 条**除 LUID 外完全相同**的 AMD Radeon 记录（描述、vendorId `0x1002`、deviceId `0x1900`、专用显存 418 MB 全同，**LUID 甚至也是同一个 `0x137D8`**）+ 1 条 Microsoft Basic Render Driver（WARP，`dxgiIndex=4`）。
- 处理方式：枚举时折叠「vendorId + deviceId + 描述 + 专用显存 + software 标志」全同的记录，记 `duplicates=4`；折叠后本机 `gpu.selectedIndex` 取值域为 **2 项**（AMD 核显 + WARP）。不折叠的话设置页会出现 4 个一模一样的选项。
- 集成显卡判据：非软件适配器、专用显存 ≤ 1 GiB、且共享系统内存 ≥ 4× 专用显存（本机 418 MB 专用 / 11918 MB 共享 → 判为集成）。
- **显存占用率的分母**：核显用「专用 + 共享系统内存」（本机 418 + 11918 MB），独显用专用显存。理由：核显的「专用显存」只是 BIOS 切出的一小块，拿它当分母会得出「91% 显存已满」这种假警报（实测已用 381 MB / 专用 419 MB）。
- `ADL2_Adapter_DedicatedVRAMUsage_Get(ctx, iAdapterIndex, int* outMB)` 可用，本机返回约 **381–389 MB**（专用显存已用量）。
- **代价（2026-10-15 实测）**：这两个显存用量导出（`ADL2_Adapter_SharedVRAMUsage_Get` / `ADL2_Adapter_DedicatedVRAMUsage_Get`，同一后端）**首次调用会把 Windows 性能计数器表（perflib）拉进进程**——首次调用 278–289 ms、工作集 9.42 → 14.34/14.39 MB、私有 1.73 → 5.91/6.05 MB，且**不归还**。因此 `gpu.vram.used` / `gpu.vram.usage` 在 ADL2 路径上**默认不可用**（catalog 里 `defaultVisible=false`，默认视图不受影响），M4 再考虑做成显式 opt-in；`Reading.detail` 原文：「默认不启用：ADL2 显存用量导出首次调用即让性能计数器表常驻（+4.3 MB），且实测 GPU 满载时数值恒定，疑非实时用量」。
- **`ADL2_Adapter_VRAMUsage_Get` 的语义疑问**：它**很便宜**（0.08 ms、无内存代价），但本机返回 **27463**、单位不明——若按 **16 KB 粒度**换算约 **429 MB**，非常接近 DXGI 报告的专用显存**总量 418 MB**，说明它**很可能不是"实时用量"而是容量类读数**。在单位语义确证前**不能拿它顶替显存占用**（否则会在负载变化时给出恒定假值）。

**性能计数器表（perflib）归因实测（2026-10-15，`adl2mem.exe` 同进程顺序打点，输出为"工作集/私有"）**

决定性证据：**那笔 4.3 MB / 约 280–300 ms 的一次性开销来自 Windows 性能计数器表本身，不是某个具体 API 的"特性"。谁先碰它谁付账，且付过之后在同一进程内再不归还。**

| 打点 | 耗时 | 工作集（MB） | 私有（MB） |
|---|---|---|---|
| `PdhAddEnglishCounterW`（首次） | **270.38 ms** | 9.84 → 14.70 | 1.70 → 5.86 |
| `ADL2_Adapter_SharedVRAMUsage_Get`（首次） | 278–289 ms | 9.42 → 14.34 | 1.73 → 5.91 |
| `ADL2_Adapter_DedicatedVRAMUsage_Get`（首次，与上行同一后端） | 278–289 ms | 9.42 → 14.39 | 1.73 → 6.05 |
| **先调 ADL2 的显存用量导出，再调 `PdhAddEnglishCounterW`** | **0.05 ms** | — | — |
| `PdhOpenQueryW` | 0.00 ms | — | — |
| `PdhExpandWildCardPathW`（表已加载时） | 0.05–0.08 ms | — | — |

- **共用同一张表就是铁证**：同一进程里先付过 ADL2 显存用量导出的账，`PdhAddEnglishCounterW` 从 **270 ms 掉到 0.05 ms**（对照：全新进程里是 270 ms）。
- **此前把 4.3 MB 归因于 `PdhExpandWildCardPathW` 是误判**：它只是"第一个碰 PDH 的调用"而已（表已加载时只要 0.05–0.08 ms）。真正的代价是表本身的加载与常驻；`PdhCloseQuery` 也不会归还。
- **对本项目的直接后果**：ACPI 热区兜底链（`Thermal Zone Information` 计数器集）**连枚举热区都会触发这次加载**，因此 provider 必须**完全惰性**（只在真要读温度时才枚举；`--probe` 会主动触发一次）。ADL2 只在被 `CLK_*` / `TEMPERATURE_*` / `*_POWER` 这些 **PMLog 传感器**触碰时是便宜的（单次 0.76 ms）。
- **因此免驱默认路径不走 PDH**：`cpu.usage` 改走原生 `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`（单次 0.04–0.13 ms、**零常驻内存代价**），`cpu.freq.avg` 改走 `CLK_CPUCLK`(34)。本机实测：工作集 **11.88 MB / 私有 2.46 MB**、`startupMs = 45.8`、整轮采样平均 **0.92 ms**；对照 PDH 路径旧构建：工作集 18.04 MB / 私有 6.70 MB、`startupMs = 435.7`（其中 `stage.cpu.pdh = 381.2`）、整轮平均 23.65 ms/最大 295 ms。完整分段与阶梯见 `docs/winperfmon-dev-plan.md` §6。

**由本节带来的代码/契约变化**：

| 项 | 变化 |
|---|---|
| `Availability` | 新增编译期字段值 `AvVendorOrRing0`（CPU 封装级温度/功耗：AMD 上由 ADL2 的 PM 表免驱提供，Intel 上仍需 Ring0 的 MSR DTS / RAPL）。它是 `catalog.h` 的编译期辅助字段，**不是 config-schema.md 的文档字段** |
| 改用它标注的指标 | `cpu.temp.package`、`cpu.power.package`、`cpu.power.soc`（此前都标 `AvRing0`，在 AMD 免驱场景下与事实不符） |
| provider 优先级 | `memory` → **`cpu.native`（占用，零常驻内存）** → `adl2`（AMD 免驱：温度/功耗/GPU/`cpu.freq.avg`）→ **`cpu.pdh`（末位降级，首次触碰即让 perflib 常驻 +4.3 MB）** → `acpi-thermal`（兜底且**完全惰性**，只在槽位仍为空时才枚举热区） |
| 本机 `--once` 验收 | 50 项指标中**可用 13 项、本机无 10 项、待接入通道 27 项、错误 0**；可用项里含 `cpu.temp.package` **46 °C**、`cpu.power.package` **13.0 W**（ADL2 `ASIC_POWER`）、`cpu.freq.avg` **3775 MHz**（`CLK_CPUCLK`）、`cpu.usage` **6%**（原生通道）、`mem.usage` **69%**、`gpu.usage` **2%**、`gpu.freq.core` **800 MHz**、`gpu.freq.mem` **2800 MHz**、`gpu.temp.core` **46 °C**。`gpu.vram.used` / `gpu.vram.usage` **本轮起默认不可用**（见上"显存用量导出"节）——目录项仍在，只是 `defaultVisible=false` 且不出现在可用项里 |
| `Adl2Provider::Claims()` | 新增 `bool Claims(std::string_view metricId) const`：只有在**该适配器绑定存在**、**目标传感器索引 > 0**、且**该索引出现在本次 PMLog 的 `supported` 列表里**三者同时成立时才返回 true。Collector 用 `Claims("cpu.freq.avg")` 判断 ADL2 真能提供该指标**才跳过 PDH**；**不得用"ADL2 库存在"或"`ADL2_Main_Control_Create` 成功"当判据** |
| 分发红线 | **绝不随包分发这些 DLL / 头文件 / 导入库**，一律运行期 `LoadLibraryW` + `GetProcAddress`（遵守「无运行时依赖」铁律） |

**因此被改写的既有结论**（本文件与同批文档）：`cpu.temp.package` / `cpu.power.package` / `cpu.power.soc` 在 AMD 上**不需要 Ring0**，「免驱模式只能显示 N/A」的前提在 AMD 上不成立；CPU 温度在**免驱侧的第一来源从 ACPI 热区改为 ADL2 PM 表**，ACPI 退为兜底（Intel 或没有厂商 PM 表的机器才轮到它）；`gpu.power` 在核显上按假数据拒绝提供，是本目录里第一个**主动不提供**的指标。

## 与 `amd-cpu.md` 原文冲突的更正汇总

| # | 原文 | 已更正为 | 依据（节号） |
|---|---|---|---|
| 1 | §5.1「Zen5 疑似 `CpuFid × 50 MHz`」 | `CoreCOF = CpuFid × 5 MHz`（`CpuFid = bits[11:0]`，12 位） | 本文件 4：LHM `Amd17Cpu.cs:439-441` |
| 2 | §4 / §5.1「`IA32_APERF (0xE7)` / `IA32_MPERF (0xE8)` 为 AMD 有效频率首选」 | AMD 上不可用；首选改为 PM 表每核频率 | 本文件 3：Linux `aperfmperf.c:128-129`、`events/msr.c:81-85` |
| 3 | §5.2「温度上限 17h 95 °C、19h 95/115 °C」 | 无家族级常数与可读寄存器；按 SKU 取官方规格页（9950X 95、7800X3D 89、5800X3D 90、PRO 2700X 95 °C） | 本文件 7：amd.com 产品页；memtest86plus 仅对 Intel 设 TjMax |
| 4 | §5.2「X3D 型号的 Tctl offset 与普通型号不同」 | X3D 无专属 offset；差异体现在 Tjmax，offset 一律 0 | 本文件 2：k10temp / zenpower / SMCAMDProcessor / memtest86plus 表内均无 19h/1Ah/X3D 条目 |
| 5 | §6 示例 `smuMailbox { msg 0x3B10528, arg 0x3B10564, rsp 0x3B10998, pmTableCmd 0x66 }` | `msg 0x3B10528 / arg 0x3B10998 / rsp 0x3B10564`（原 arg 与 rsp 写反）；19h 桌面（Vermeer）命令号为 `0x06`，`0x66` 属 APU 组 | 本文件 5：RyzenAdj `nb_smu_ops.c:8-20`、`RyzenSMU.p` codename 分支 |
| 6 | §5.3「长期 ≈ 2.0× 则自动折半」 | 改为校验式诊断（禁止跨域/跨线程求和 + `ESU` 动态取），不自动折半 | 本文件 8：无 AMD 硬件 2× 的一手来源 |
| 7 | §3 / §5.5「PCI `0x60/0x64` 与 PawnIO SMN IOCTL 二选一（IOCTL 名称待核实）」 | 二者是「接口与实现」关系：IOCTL 优先，`0x60/0x64` 作为自建备选 | 本文件 1：`AMDFamily17.p:193` |

## 对 `amd_tables.json` 的字段建议（可直接填进 §6 schema）

建议把「邮箱三元组」从单值改成**候选数组**（运行期择优），把命令号拆成三条，并给温度上限/offset 增加来源字段：

```json
{
  "family": "0x19", "models": ["0x21"], "codename": "vermeer",
  "brandMatch": ["5800X3D", "5800X"],
  "refClockMHz": 100,
  "coreCofFormula": { "kind": "fid_div_did", "fidBits": [7, 0], "didBits": [13, 8], "baseMHz200": true },
  "tctl": { "rangeShift49C": true, "offsetMode": "brand_table", "offsetC": 0 },
  "tempLimitC": 90, "tempLimitSource": "https://www.amd.com/.../amd-ryzen-7-5800x3d.html",
  "smuMailboxCandidates": [
    { "msg": "0x3B10528", "arg": "0x3B10998", "rsp": "0x3B10564" },
    { "msg": "0x3B10528", "arg": "0x3B10998", "rsp": "0x3B10578" },
    { "msg": "0x3B10928", "arg": "0x3B10998", "rsp": "0x3B10978" }
  ],
  "smuCmds": { "getVersion": "0x02", "pmTableVersion": "0x08", "pmTableBase": "0x06", "pmTableTransfer": "0x05" },
  "pmTable": { "version": "0x380804", "fields": { "tdie": 0, "svi2CoreV": 0, "ppt": 0, "coreClk": 0 } }
}
```

| 家族 | 邮箱候选（msg / arg / rsp） | PM 表命令（版本 / 基址 / 传输） | 温度上限 | offset |
|---|---|---|---|---|
| 17h 桌面 / HEDT（Summit–Matisse） | `0x3B10528 / 0x3B10998 / 0x3B10564` | `0x0D`→`0x08` / `0x0B`→`0x06` / `0x0A`→`0x05` | 95 °C（PRO 2700X 官方；逐 SKU） | 按品牌串表：1600X/1700X/1800X 20、2700X 10、TR19xx/29xx 27 |
| 17h APU（Raven/Dali/Picasso） | 同左；失败则试 `rsp 0x3B10578` | `0x0C` / `0x0A`,`0x3D`,`0x0B` / `0x3D`(arg=3) | 未取得一手来源（移动 APU 常为 105 °C，需逐 SKU 查） | 0（表内无条目） |
| 19h 桌面（Vermeer/Rome/Milan） | `0x3B10528 / 0x3B10998 / 0x3B10564` | `0x08` / `0x06` / `0x05` | 5800X3D 90 °C（官方，逐 SKU） | 0 |
| 19h 桌面（Raphael/DragonRange） | `0x3B10528 / 0x3B10998 / 0x3B10564` | `0x05` / `0x04` / `0x03` | 7800X3D 89 °C（官方，逐 SKU） | 0 |
| 19h APU（Renoir/Cezanne/Rembrandt/Phoenix） | `0x3B10528 / 0x3B10998 / 0x3B10578` | `0x06` / **`0x66`** / `0x65`(arg=3) | 未取得一手来源 | 0 |
| 1Ah（Granite Ridge / Strix Point / Strix Halo / Turin） | `0x3B10528 / 0x3B10998 / 0x3B10564`，Halo/Glinda 类试 `0x3B10928 / 0x3B10998 / 0x3B10978` | `0x05` / `0x04` / `0x03` | 9950X 95 °C、9800X3D 95 °C（官方/一致零售页） | 0 |

**仍然只能靠探测的字段**：

1. **邮箱三元组的家族映射**——按候选顺序试：先 `getVersion (0x02)`（只读探针），响应非 0 且 `SMU_OK` 者胜出，失败再试下一组；把胜出项写入诊断导出。
2. **Tjmax（`tempLimitC`）**——匹配不到 SKU 就显示「距上限」不可用；运行期可用「满负载长时间 Tctl 峰值 + HWiNFO 的 Tjmax 读数」对拍，但不得据此自动填表。
3. **Tdie offset**——启动时读 `THM_TCON_CUR_TMP` 的 bit19 / bits[17:16] 判定 `−49 °C` 量程；型号常数表只对 17h 生效；X3D 与 19h/1Ah 默认 0，与 HWiNFO 对拍偏差 > 5 °C 时记诊断并回退显示 Tctl。
4. **有效频率（`cpu.freq.avg`）**——PM 表每核频率为首选；`0xC00000E7/0xC00000E8` 差分只在探测阶段试读（两次差分 × 基频，与 PM 表值比对，偏差 < 3% 才启用），否则一律走 PM 表。
5. **RAPL 能量单位与聚合**——`ESU` 从 `0xC0010299 bits[12:8]` 动态取；只用 `0xC001029B` 单域差分；与 PPT 长期比值只作为诊断信号。
