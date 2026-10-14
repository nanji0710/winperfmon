# Intel CPU 采集设计：频率、温度、功耗与降级矩阵

> 本文由 winperfmon-dev-plan.md §3.1 拆出并细化（Intel 部分）；上游冲突时以本文为准，并回头修正主方案。
> 关联：`architecture.md`（线程/采样预算）、`amd-cpu.md`（AMD 侧设计，接口必须一致）、`config-schema.md` §4（指标 ID 目录）。
> 术语：Ring0 通道 / 免驱模式 / PM 表 / 1% Low 见 `README.md` §五。**所有 MSR 地址、位域、端口号必须与本文字面一致，不得"大概化"。**

## 0. 指标 ID 与对外契约

`CpuProvider` 对外只暴露 `config-schema.md` §4 的这套 ID（Intel/AMD 必须同源，UI 与配置不感知厂商）：

| 指标 ID | 分组 | 默认名称 | 单位 | 精度 | 默认可见 | Intel 首选 → 降级 |
|---|---|---|---|---|---|---|
| `cpu.usage` | cpu | CPU | % | 0 | ✅ | PDH / `NtQuerySystemInformation` |
| `cpu.freq.avg` | cpu | CPU 频率 | MHz | 0 | ✅ | APERF/MPERF 差分 → PDH `% Processor Performance` × 基频 |
| `cpu.freq.perCore` | cpu | 每核频率 | MHz | 0 | ❌ | 绑核读 MSR |
| `cpu.temp.package` | cpu | CPU 温度 | °C | 0 | ✅ | `IA32_PACKAGE_THERM_STATUS(0x1B1)` |
| `cpu.temp.cores` | cpu | 每核温度 | °C | 0 | ❌ | 每核 `IA32_THERM_STATUS(0x19C)` |
| `cpu.temp.headroom` | cpu | 距温度上限 | °C | 0 | ❌ | `TjMax − DTS` |
| `cpu.power.package` | cpu | CPU 功耗 | W | 1 | ✅ | RAPL `MSR_PKG_ENERGY_STATUS(0x611)` 差分 |
| `cpu.power.cores` | cpu | 核心功耗 | W | 1 | ❌ | RAPL PP0（`0x639`） |
| `cpu.power.dram` | cpu | 内存功耗 | W | 1 | ❌ | RAPL DRAM（`0x619`） |
| `cpu.voltage` | cpu | CPU 电压 | V | 3 | ❌ | `0x198`（位域见 §4.3，**待核实**） |
| `cpu.throttle.reason` | cpu | 降频原因 | — | — | ❌ | 热/功耗/电流限制位（来源 **待核实**） |

## 1. 频率：APERF/MPERF 差分

| 项 | 值 |
|---|---|
| 有效频率公式 | **有效频率 = 基频 × ΔAPERF/ΔMPERF** |
| 计数器 | MSR `IA32_MPERF(0xE7)` / `IA32_APERF(0xE8)`（各 64 位，读两次求差分） |
| 基频来源 | `MSR_PLATFORM_INFO(0xCE)` bits[15:8] × **BCLK(100 MHz)** |
| 读某核的 MSR | 需 `SetThreadAffinityMask` **绑核**后再读，否则读到的是当前核的计数 |
| 采样周期 | 500 ms（频率 200 ms，仅性能视图开启时），单次成本预算 1–5 µs（`architecture.md` §6.1） |
| 免驱降级 | PDH `\Processor Information(_Total)\% Processor Performance` × 基频，或 `\Processor Information(*)\Processor Frequency`（MHz） |

**实现要点**

1. 差分窗口 = 两次采样的时间差；**先用 `QueryPerformanceCounter` 测真实 Δt**，不要假定正好等于定时器周期（高精度定时器有抖动）。
2. 绑核读 N 个核 = N 次线程亲和性切换 → 只在 `cpu.freq.perCore` 可见时才做（默认不可见，见上表），否则只读一次 `_Total` 级比值。
3. 大小核（P-core / E-core）机器上，「平均频率」必须说明是**所有核的算术平均**还是按核数加权 —— 目前设计为**每核比值先算再平均**；拓扑（哪些是 P 核、哪些是 E 核）需要额外识别 → **待核实**（`GetLogicalProcessorInformationEx` + CPUID 能否稳定区分 P/E 核未在源文档中确认）。
4. 频率合理性校验：**频率 ≤ 睿频上限**，超出上限的值打标 `N/A(异常)`（主方案 §9）。
5. 计算基频时 BCLK 固定取 100 MHz；若 `0xCE` 读取失败（无 Ring0），走 PDH 降级路径而不是猜一个基频。

## 2. 温度：DTS 与 TjMax

| 项 | 值 |
|---|---|
| 每核温度 | `IA32_THERM_STATUS(0x19C)`（每核一个 MSR，**绑核读**） |
| 封装温度 | `IA32_PACKAGE_THERM_STATUS(0x1B1)` |
| TjMax | `MSR_TEMPERATURE_TARGET(0x1A2)` **bits[23:16]** |
| 温度公式 | **温度 = TjMax − DTS(bits[22:16])** |
| 余量指标 | `cpu.temp.headroom` = `TjMax − DTS` |
| 兜底（免驱） | WMI `MSAcpi_ThermalZoneTemperature` —— **多数台式机/新笔记本不支持**，仅作最后兜底 |

**实现要点**

1. DTS 是"距 TjMax 的度数"，读数是正值表示低于上限；**DTS = 0 是"到达上限"，不是"0 °C"** —— 公式写反会得到荒谬的负数/巨大值，必须在单测里覆盖 DTS = 0、DTS = TjMax 两个边界。
2. TjMax 必须从 `0x1A2` 读，不要硬编码 100 °C。
3. 状态色阈值（`config-schema.md` §3.4）：**CPU 温度 80 / 95 °C → warn / critical**，且状态色必须**同时打印数值**（不允许只用颜色表达状态）。
4. 合理性区间：**温度 −40…125 °C**，超出打标 `N/A(异常)` 并记录一次诊断（主方案 §9）。

## 3. 功耗：RAPL 能量差分

| 项 | 寄存器 | 说明 |
|---|---|---|
| 单位寄存器 | `MSR_RAPL_POWER_UNIT(0x606)` | 能量单位 = **1/2^ESU J**，`ESU = bits[12:8]`；功率单位 = `bits[3:0]` |
| 封装能量 | `MSR_PKG_ENERGY_STATUS(0x611)` | 32 位计数器 |
| 核心能量 | `MSR_PP0_ENERGY_STATUS(0x639)` | → `cpu.power.cores` |
| 核显能量 | `MSR_PP1(0x641)` | 无对应指标 ID（未纳入目录） |
| 内存能量 | `MSR_DRAM_ENERGY_STATUS(0x619)` | → `cpu.power.dram` |
| 平台能量 | `MSR_PLATFORM_ENERGY_STATUS(0x64D)` | 无对应指标 ID（未纳入目录） |
| 公式 | **功耗 = ΔEnergy / Δt** | nwinfo 口径：`Δ能量(厘焦) × 1000 / Δms = 厘瓦`，注意单位换算 |
| 回绕 | **32 位计数器按 2^32 处理回绕** | 差分负数时按 `(2^32 − prev + cur)` 修正 |
| AMD 对照 | AMD 是 `0xC0010299` 单位 + `0xC001029B` 能量（fam ≥ 17h） | 见 `amd-cpu.md` |

**实现要点**

1. 三个能量寄存器**共用** `0x606` 的单位换算，缓存一次即可（单位不会变）。
2. 差分窗口与频率采样同拍最好（同一时刻读能量与时间戳），否则 200 ms 频率采样与 500 ms 功耗采样混用时间戳会引入误差。
3. **RAPL 需 BIOS 开启（部分 OEM 关闭）**：读不到时标记 `N/A` 而非显示 `0`。
4. **读数翻倍风险**：社区有 RAPL 功耗读数翻倍的案例（System Informer issue #2001）→ 用 HWiNFO 或**墙插功耗计**做一次绝对值校验；异常时降级标注而不是照抄。
5. 合理性区间：**功耗 0…1000 W**（主方案 §9）。
6. 功耗墙（Power Limit）读取：`MSR_PKG_POWER_LIMIT(0x610)`（**PL1 = bits[14:0]、PL2 = bits[46:32]**）、`MSR_CONFIG_TDP_NOMINAL(0x614)`。这些值属于**「什么是上限」的上下文**，可用于把功率换算成"打到墙的百分比"，但当前指标目录里没有对应 ID → 若要展示需先在 `config-schema.md` §4 增补 ID。

## 4. 补充寄存器与倍频

### 4.1 倍频 / BCLK

| 寄存器 | 用途 |
|---|---|
| `MSR_PERF_STATUS(0x198)` | **倍频 bits[15:8]** |
| `MSR_TURBO_RATIO_LIMIT(0x1AD)` | 最大倍频（睿频上限，可用于频率合理性校验） |
| `MSR_FSB_FREQ(0xCD)` | BSEL 表：**266.67 / 133.33 / 200 / 166.67 / 333.33 / 100 / 400 MHz**（查表得 BCLK，替代固定 100 MHz 的假设） |

### 4.2 与 AMD 的接口一致性

| 关注点 | 规则 |
|---|---|
| 指标 ID | 完全共用 §0 表；AMD 侧的 SMN/SMU 实现细节只出现在 `amd-cpu.md` |
| 缺失语义 | 「Intel 没有这个读数」与「AMD 没有这个读数」用**同一个缺失原因 id 体系**（`architecture.md` §1）；例如 AMD 无核级 RAPL → `cpu.power.cores` 的缺失原因是 `vendor_unsupported`，不是 `no_ring0` |
| 单位与精度 | 频率 MHz、温度 °C、功耗 W、电压 V（目录给定精度），厂商侧不做四舍五入，精度交给渲染层 |
| 采样对齐 | 两家都在同一拍输出同一套 ID，UI 与配置不感知厂商分支 |

### 4.3 电压（`cpu.voltage`）

- `config-schema.md` §4 把 Intel 来源写作 **`0x198`**，而主方案 §3.1 对 `0x198` 的定义是**倍频 bits[15:8]** —— 同一 MSR 承担两个用途在本项目文档中**未给出电压位域**。→ **待核实**：需查 nwinfo `libnw/cpu/intel_cpu.c` 确认 `0x198` 中电压（比值/VID）的位域与换算；**在核实前 `cpu.voltage` 在 Intel 上保持 N/A**，绝不显示推算值。
- 降频原因位（`cpu.throttle.reason`）同样缺少源文档位域定义 → **待核实**；核实前该指标在 Intel 上保持 N/A。

## 5. 采样与线程（与 `architecture.md` 对齐）

| 项 | 值 |
|---|---|
| 执行线程 | 采集线程（1 个），`CreateWaitableTimerEx` + 事件等待，不用轮询 `Sleep` |
| MSR 读取周期 | 500 ms；频率 200 ms（仅性能视图开启时）；ADLX/SuperIO 不在此路径 |
| 单次成本预算 | **1–5 µs/次** |
| 绑核 | 读每核 MSR 前 `SetThreadAffinityMask`；读完全部核后恢复原亲和性 |
| 写共享 | 写「传感器快照」双层缓冲 + `std::atomic` 序号发布；读侧无锁 |
| 免驱路径 | 只走 PDH（无 `SetThreadAffinityMask`、无 MSR），成本更低 |

## 6. 降级矩阵

| 能力 | ① 有 Ring0 通道 | ② 无 Ring0（免驱模式） | ③ 虚拟机 / 宿主机隐藏 MSR |
|---|---|---|---|
| `cpu.freq.avg` | APERF/MPERF 差分 × 基频 | PDH `% Processor Performance` × 基频 | PDH 可用（若计数器存在） |
| `cpu.freq.perCore` | 绑核读 MSR ✅ | ❌ 缺（PDH 每核 `Processor Frequency` 可选，标 `approximate`） | ❌ 缺 |
| `cpu.temp.package` | `0x1B1` ✅ | WMI 兜底（多数不支持）→ 通常 N/A | N/A |
| `cpu.temp.cores` | `0x19C` 每核 ✅ | N/A | N/A |
| `cpu.temp.headroom` | `TjMax(0x1A2) − DTS` ✅ | N/A | N/A |
| `cpu.power.package` | RAPL `0x611` ✅ | N/A（无可靠方案） | N/A |
| `cpu.power.cores` / `cpu.power.dram` | RAPL `0x639` / `0x619` ✅ | N/A | N/A |
| `cpu.voltage` | **待核实**（§4.3） | N/A | N/A |
| `cpu.throttle.reason` | **待核实**（§4.3） | N/A | N/A |
| `cpu.usage` | PDH / `NtQuerySystemInformation` | 同左 ✅ | 同左 ✅ |

**呈现规则**：缺失项按 `layout.hideMissing` 处理（`true` = 整项隐藏；`false` = 显示 `N/A` 并把颜色降为 `colors.unit`，避免误读为异常值）。免驱模式下 `ui-settings.md` 建议用 `N/A` + 原因说明，让用户理解为什么空。

## 7. 驱动与许可约束（Intel 侧）

| 项 | 内容 |
|---|---|
| Ring0 通道 | **只能走 PawnIO**（WinRing0 已被微软列入易受攻击驱动阻止列表、Defender 报 `HackTool:Win32/Winring0` / `VulnerableDriver:WinNT/Winring0`，CVE-2020-14979） |
| 所需模块 | `IntelMSR.p`（MSR 读写）、`LpcIO.p`（端口 I/O，风扇用），见 `gpu-fans.md` |
| 调用范式 | `WR0_ExecPawn(h, &h->pio_intel, "ioctl_read_msr", &in, 1, &out, 1, NULL)`，配驱动回退链（`--driver=CPUZ162\|NwHwIo\|PawnIO\|NONE`） |
| 许可 | PawnIO（驱动）为 **GPL-2.0**、PawnIO.Modules（`.bin` 模块）为 **LGPL-2.1**：运行期调用已安装驱动的 IOCTL、不链接其源码；若随包分发安装包／模块须履行各自许可义务（`release-github.md` §3.5）；再保险 = 引导用户自行安装 |
| 用户开关 | 必须提供「禁用 Ring0 驱动安装」开关（照抄 LHM PR #1777 的做法），让用户能选纯免驱模式 |
| 常数/位域来源 | **以 nwinfo（Unlicense＝公共领域）`libnw/cpu/intel_cpu.c`、`amd_cpu.c` 为准**（它还附带 PawnIO 调用范式与驱动回退链） |

## 8. 本机不可用时的用户文案（按缺失原因 id）

| 缺失原因 id | 触发条件 | 文案要点 |
|---|---|---|
| `no_ring0` | 用户未安装/拒绝 PawnIO | 「需要驱动支持」+「了解如何启用」链接（**不自动安装**） |
| `sensor_absent` | MSR 存在但该机型不暴露（如 OEM 关 RAPL） | 「本机硬件未暴露此读数」 |
| `vendor_unsupported` | 指标在该厂商无对应实现（如 AMD 无核级 RAPL） | 「该平台不提供此指标」 |
| `value_invalid` | 合理性校验失败 | 「读数异常，已忽略」+ 打标 `N/A(异常)` |

## 9. 验收与自测要点

| 项 | 标准 | 方法 |
|---|---|---|
| 频率准确度 | 与 HWiNFO 对拍 **±50 MHz** | 双显卡机器，跑 `Prime95` 小 FFT 与轻载两档 |
| 温度准确度 | 与 HWiNFO 对拍 **±1 °C**（M0 免驱对照）/ AMD 侧 ±2 °C（见 `roadmap.md`） | 稳态负载下取 30 s 均值 |
| 功耗准确度 | 与 HWiNFO 对拍 **±5%** | 同时用墙插功耗计做一次绝对值校验（防 RAPL 翻倍） |
| 内存占用 | ≤ 15 MB | 任务管理器 + `Get-Process` 私有工作集（口径见 `architecture.md` §7） |
| HVCI | 内存完整性开启状态下不报错、不触发驱动加载失败 | Windows 安全中心开启「内存完整性」后全流程回归 |
| 边界单测 | DTS = 0 / DTS = TjMax、RAPL 32 位回绕、`ΔMPERF = 0` 除零 | 单元测试（纯函数化：把 MSR 值当输入） |

## 10. 已知陷阱清单

1. **DTS 语义**：`DTS = 0` 表示到达 TjMax，不是 0 °C。
2. **不绑核读 MSR**：拿到的是调度到的那个核的值 —— 每核频率/温度全错。
3. **固定 100 MHz 假设**：非 100 MHz BCLK 的机型需要 `MSR_FSB_FREQ(0xCD)` 的 BSEL 表。
4. **RAPL 回绕**：32 位计数器不做 2^32 修正会出现巨大负值。
5. **RAPL 翻倍**：必须做绝对值校验（System Informer issue #2001）。
6. **OEM 关闭 RAPL**：读不到要标 N/A，不要填 0 —— 填 0 会让用户以为 CPU 不耗电。
7. **免驱模式假装有数据**：PDH 只能给平均频率，**不要**用它伪造 `cpu.freq.perCore`。
8. **`0x198` 双重语义**：倍频 bits[15:8] 是已确认的；电压位域未核实前不要输出数值。
