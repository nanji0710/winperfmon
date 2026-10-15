# Windows 极致轻量化性能监控工具 — 开发方案

> 目标产品：常驻后台的轻量硬件监控 + 游戏帧率统计工具，覆盖 **CPU/GPU 频率 · 功耗 · 温度 · 风扇转速 · FPS / 1% Low FPS**。
> 核心设计原则：**不注入、不挂钩、不自研内核驱动、单 EXE、无运行时依赖**。

---

## 1. 产品目标与硬性指标（"极致轻量化"的可验证定义）

| 维度 | 目标值 | 说明 |
|---|---|---|
| 常驻内存 | ≤ 15 MB（纯监控）/ ≤ 25 MB（含 FPS 采集） | 固定缓冲，无持续分配 |
| CPU 占用 | ≤ 0.3%（1 Hz 采样）/ ≤ 1.0%（游戏内 FPS 采集） | ETW 由 PresentMon 服务承担 |
| 冷启动 | ≤ 300 ms | 无运行时初始化（**实测 45.8 ms（免驱全量）/ 10–23 ms（`--version`，不采集）；原先 435.7 ms 的瓶颈是 perflib 计数器表加载，见 §3.1、§6**） |
| 安装包/体积 | EXE ≤ 5 MB；发布物为**便携 ZIP**（EXE + PawnIO 官方安装包 + GPL-2.0／LGPL-2.1 合规文本，决策 ③=B） | `/MT` 静态 CRT，无 .NET/WebView2/Python |
| 系统支持 | Win10 1809+ / Win11（x64、ARM64 后续） | 兼容内存完整性(HVCI)开启状态 |
| 数据能力 | CPU：每核频率、每核温度、封装温度、封装/核/uncore/DRAM 功耗<br>GPU：核心/显存频率、功耗、核心/hotspot/显存温度、风扇 %<br>主板：各 FAN 转速 RPM<br>帧率：FPS、1% Low、0.1% Low、帧时间、卡顿次数、呈现模式 | 每项都需有"不可用"降级路径 |

**本机实测（2026-10-15，免驱路径全量：ADL2 PM 表 + 原生 CPU + 内存 + ACPI 兜底）**

| 维度 | 预算 | 实测（本机） | 余量 |
|---|---|---|---|
| 常驻内存 | ≤ 15 MB | **工作集 11.88 MB / 私有 2.46 MB / 峰值 12.07 MB** | 工作集余量约 3 MB |
| 冷启动 | ≤ 300 ms | **`startupMs = 45.8 ms`**、首帧 `0.91 ms` | 约 6× |
| 采样成本 | ≤ 0.3%（1 Hz）/ ≤ 1.0%（游戏内） | **整轮平均 0.92 ms/次、最大 1.54 ms/次**，逐源见 §6 | — |

对照旧构建（PDH 计数通路，`winperfmon-pdh.exe`）：**工作集 18.04 MB / 私有 6.70 MB**，整轮采样平均 **23.65 ms/次、最大 295 ms**（`startupMs = 435.7`、其中 `stage.cpu.pdh = 381.2`）。差距全部来自**性能计数器表（perflib）是否被拉进进程**，机制与证据见 §6 的"为什么默认路径不用 PDH"。

---

## 2. 总体架构

```
┌────────────────────────────────────────────────────────────────┐
│ UI 层    主窗口(卡片/曲线) │ 托盘 │ Overlay(置顶分层窗口) │ RTSS OSD(可选后端) │
├────────────────────────────────────────────────────────────────┤
│ 会话与统计  会话管理 │ 帧时间环形缓冲 │ 百分位统计 │ CSV/JSON 导出 │
├────────────────────────────────────────────────────────────────┤
│ 采集调度器  分级定时器(100ms~5s) │ 空闲降频 │ 异常值校验 │ 设备热插拔 │
├────────────────────────────────────────────────────────────────┤
│ ISensorProvider 抽象                                           │
│  ├ CpuProvider  Intel MSR / AMD SMN+MSR                        │
│  │              免驱: cpu.native(占用) + ADL2 频率 (PDH 末位)    │
│  ├ GpuProvider  NVML / ADLX / IGCL(含 Level Zero 备用)          │
│  ├ FanProvider  SuperIO(Nuvoton/ITE) / EC / GPU 风扇            │
│  └ FpsProvider  PresentMon Service API │ 子进程 │ 自建 ETW      │
├────────────────────────────────────────────────────────────────┤
│ 访问层  Ring0: PawnIO 模块(MSR / PortIO / SMN)                  │
│         User: NtQuerySystemInformation / DXGI / ADL2 / NVML.dll /    │
│               ADLX.dll / IGCL.dll（PDH 仅作末位降级，见 §3.1）        │
│         ETW : PresentMon 服务(SYSTEM) 或本进程 ETW 会话          │
└────────────────────────────────────────────────────────────────┘
```

**进程模型**
- **v1**：单进程 + `requireAdministrator`（Ring0 与 ETW 都需要），实现最简单。**Ring0 侧已在真机上确认必须提权**：PawnIO 驱动给设备对象设的 ACL 只授权 SYSTEM 与 Administrators，非管理员只会拿到 `ring0.unavailableReason=打不开 PawnIO 设备（错误 5（拒绝访问。））`；免驱路径则完全不需要提权（见 §4 与 `docs/ring0-pawnio.md` §3.1、§7）。
- **v2（推荐尽早拆）**：`pmsvc`(SYSTEM 服务，独占所有 Ring0/驱动访问) + `pmui`(普通权限 UI/Overlay)，命名管道 + 共享内存 IPC。好处：UI 不需要提权（Overlay 在游戏上更安全）、驱动访问与 UI 崩溃隔离、反作弊面更小。

---

## 3. 各指标技术路线（核心表）

### 3.1 CPU

| 指标 | 首选方案（需 Ring0） | 寄存器/接口细节 | 免驱降级 |
|---|---|---|---|
| 频率（每核/平均） | MSR `IA32_MPERF(0xE7)` / `IA32_APERF(0xE8)` 差分比值 × 基频 | 有效频率 = 基频 × ΔAPERF/ΔMPERF；基频比取 `MSR_PLATFORM_INFO(0xCE)` bits[15:8] × BCLK(100 MHz)；读某核的 MSR 需 `SetThreadAffinityMask` 绑核 | **AMD：ADL2 PM 表 `CLK_CPUCLK`(34)（免驱、免管理员、零额外内存，本机 3775→4625 MHz）**；其余：PDH `\Processor Information(_Total)\% Processor Performance` × 基频，或 `\Processor Information(*)\Processor Frequency`（MHz）——**PDH 是末位选择，因为首次触碰会加载性能计数器表，见 §6** |
| 温度（每核/封装） | Intel：`IA32_THERM_STATUS(0x19C)`（每核，绑核读）、`IA32_PACKAGE_THERM_STATUS(0x1B1)`（封装）；TjMax 取 `MSR_TEMPERATURE_TARGET(0x1A2)` bits[23:16]，温度 = TjMax − DTS(bits[22:16]) | AMD：SMN `THM_TCON_CUR_TMP(0x00059800)` bits[31:21] × 0.125 → Tctl（注意 Tctl offset 随型号）；Tdie 需读 SMU PM 表（LHM 已实现的反向工程方案）作为增强 | **AMD：ADL2 PM 表 `TEMPERATURE_CPU`(32)（免驱、免管理员、实测响应最快 → 见 §3.3.1 T0）**；其余走 WMI `MSAcpi_ThermalZoneTemperature`（多数台式机/新笔记本不支持，仅兜底） |
| 功耗（封装/核/uncore/DRAM） | RAPL MSR：`MSR_RAPL_POWER_UNIT(0x606)`（能量单位 = 1/2^ESU J，ESU=bits[12:8]；功率单位 = bits[3:0]）、`MSR_PKG_ENERGY_STATUS(0x611)`、`MSR_PP0_ENERGY_STATUS(0x639)`、`MSR_PP1(0x641)`、`MSR_DRAM_ENERGY_STATUS(0x619)`、`MSR_PLATFORM_ENERGY_STATUS(0x64D)`；**功耗墙**：`MSR_PKG_POWER_LIMIT(0x610)`（PL1 = bits[14:0]、PL2 = bits[46:32]）、`MSR_CONFIG_TDP_NOMINAL(0x614)` | 功耗 = ΔEnergy/Δt（nwinfo 口径：Δ能量(厘焦)×1000/Δms = 厘瓦，注意单位换算）；32 位计数器按 2^32 处理回绕；Intel 与 AMD Zen 均支持 RAPL（**AMD 是 `0xC0010299` 单位 + `0xC001029B` 能量**，fam ≥ 17h） | **AMD：ADL2 PM 表 `ASIC_POWER`(23)（核显上即整片功耗）/ `SOC_POWER`(17)（免驱、免管理员；`CPU_POWER`(33) 量级不符不用）**；Intel 无可靠方案（Windows "Power Meter" 计数器仅部分机型存在） |
| 频率/倍频（补充寄存器） | `MSR_PERF_STATUS(0x198)` 倍频 bits[15:8]、`MSR_TURBO_RATIO_LIMIT(0x1AD)` 最大倍频、`MSR_FSB_FREQ(0xCD)`（BSEL 表：266.67/133.33/200/166.67/333.33/100/400 MHz） | AMD：`0xC0010063` 当前 PState、`0xC0010064–0xC001006B` = PState0–7；17h–19h 倍频 = fid/did×2（[13:8]=did、[7:0]=fid），Zen5(1Ah) = fid×0.05 | — |
| 功耗（AMD 增强） | SMU PM 表（PPT/TDC/EDC、STAPM、TDP、SVI2 电压电流），更接近 Ryzen Master 读数 | 家族相关（17h/19h/1Ah 索引不同），需型号表；**Tctl 是控制温度不是结温**，温度 = `0.001 × ((reg>>21) × 125) + SMU 偏移`，偏移随型号/AGESA 变化（5800X3D/7800X3D 上 Tctl 与 CCD 差很多）→ **禁止硬编码偏移** | — |

> 注意 1：RAPL 需 BIOS 开启（部分 OEM 关闭）；读不到时标记为 N/A 而非显示 0。
> 注意 2：社区有 **RAPL 功耗读数翻倍**的案例（System Informer issue #2001）——用 HWiNFO 或墙插功耗计做一次绝对值校验，异常时降级标注而不是照抄。
> 注意 3：本节寄存器地址/位域建议直接以 **nwinfo**（Unlicense＝公共领域）`libnw/cpu/intel_cpu.c`、`amd_cpu.c` 为准，它还附带 PawnIO 调用范式与驱动回退链（见 §4）。

**为什么默认路径不碰 PDH（已实测，2026-10-15）**

- **那笔一次性开销是 Windows 性能计数器表（perflib）本身，不是某个 API 的"特性"**：`PdhAddEnglishCounterW` 首次调用 270.38 ms、工作集 9.84 → 14.70 MB、私有 1.70 → 5.86 MB；`ADL2_Adapter_SharedVRAMUsage_Get` / `ADL2_Adapter_DedicatedVRAMUsage_Get` 首次调用 278–289 ms、同样 +4.3 MB 私有提交、+4.9 MB 工作集。**在同一进程里先调 ADL2 的显存用量导出之后，`PdhAddEnglishCounterW` 只要 0.05 ms**（全新进程里它是 270 ms）→ 两者共用同一张计数器表，这就是铁证。**谁先碰它谁付账，付过之后在同一进程内不归还**（`PdhCloseQuery` 也不会归还）。
- **此前把 4.3 MB 归因于 `PdhExpandWildCardPathW` 是误判**：它只是"第一个碰 perflib 的调用"；表已加载时 `PdhExpandWildCardPathW` = 0.05–0.08 ms、`PdhOpenQueryW` = 0.00 ms。真正的代价是表本身的加载与常驻。
- 因此默认路径**完全不碰计数器表**：`cpu.usage` 走原生 `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`（单次系统调用，实测 **0.04–0.13 ms/次、零常驻内存代价**；APERF/MPERF 与 PDH 都做不到"零内存"）；`cpu.freq.avg` 在 AMD 上走 ADL2 PM 表 `CLK_CPUCLK`(34)。只有**拿不到 ADL2** 时才退回 `CpuPdhProvider`，此时 perflib 的 +4.3 MB 与首帧跳变才出现。
- `--probe` 是诊断命令，会**主动**触发一次 perflib（用于列出 ACPI 热区，`acpi.zones=` 行）；正常 `--once` / `--watch` 路径不触发。

**原生 CPU 通道的设计与口径**

- `cpu.usage` 的原始量：每核 `KernelTime + UserTime − IdleTime` 差分 ÷ 总时间（两次相邻采样），即"核心时间占比"；**再乘当轮 `cpu.freq.avg ÷ 基频`**（基频取注册表 `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` 的 `~MHz`，本机 **3801 MHz**），得到**近似**任务管理器的 **% Processor Utility** 口径。本项目读到的原生值、基频与品牌串由 `src/sensors/cpu_info.{h,cpp}` 提供（`ReadBaseClockMHz` / `ReadCpuBrand` / `ReadLogicalCoreCount`）。
- **口径差异必须如实标注**：Utility 是**逐核按各自的频率**加权，我们只有一个"全核平均频率"因子，因此**部分核心满载时本值偏低几个百分点**（本机实测 8/16 线程满载：本值 **56–57%**、PDH Utility **59–62%**）。拿不到可信的当前频率时**退回未加权值并如实标注**，不静默混用。
- provider 名 `cpu.native`（`src/sensors/cpu_native.{h,cpp}`）；纯函数 `BusyPercentFromDelta(idle0, busy0, idle1, busy1)` 单独可测（`tests/test_cpu_native.cpp`，5 个用例）。

### 3.2 GPU

| 厂商 | 接口 | 可用指标 |
|---|---|---|
| NVIDIA | **NVML**（`nvml.dll` 随驱动，无需管理员） | `nvmlDeviceGetClockInfo(SM/MEM)`、`nvmlDeviceGetPowerUsage`(mW)、`nvmlDeviceGetPowerManagementLimit`、`nvmlDeviceGetTemperature(GPU)`、`nvmlDeviceGetFieldValues(NVML_FI_DEV_MEMORY_TEMP …)` 取 hotspot/显存、`nvmlDeviceGetFanSpeed`(%)、`nvmlDeviceGetCurrentClocksThrottleReasons`(功耗/热/PM 限制降频原因)、显存占用 |
| AMD | **ADLX**（随驱动安装，`IADLXGPUMetrics`；先用 `IADLXGPUMetricsSupport` 探测支持项） | GPU 使用率、GFX/显存频率、温度（edge/hotspot/VRAM）、风扇转速、功耗、电压、显存占用。旧卡降级 **ADL2** `ADL2_New_QueryPMLogData_Get`（`ADL_PMLOG_*`）——**实测 APU 上新驱动同样走 ADL2 即可**（见下方 bullet 与 §3.3.1 T0） |
| Intel (Arc/Iris Xe) | **IGCL**：`ctlPowerTelemetryGet`（功耗/频率/温度）、`ctlEnumTemperatureSensors`、`ctlFanGetSpeed`/`ctlFanGetConfig`、`ctlFrequencyGetState`；备用 **Level Zero Sysman**（`zesPowerGetEnergyCounter` 等） | 功耗、频率、温度、风扇 RPM、显存 |

- 多 GPU/笔记本混合输出：枚举全部设备并标注 iGPU/dGPU，默认选中"当前正在渲染的 GPU"（优先用 PresentMon 报告的 GPU，其次用使用率最高者）；用户可用 `gpu.selectedIndex` 覆盖（`-1` = 自动），枚举顺序与虚拟适配器排序规则见 `docs/config-schema.md` §3.6.1，UI 见 `docs/ui-settings.md` §6.2。
- NVIDIA 一般不提供风扇 RPM（仅 %），AMD/Intel 可给 RPM。
- **AMD 免驱通道已实测**（本机 APU，非管理员、未装 PawnIO、HVCI 开启）：ADL2 PMLog（`atiadlxx.dll` 运行期加载）给出 `gpu.usage`、`gpu.freq.core/mem`、`gpu.temp.core`，并额外承担 CPU 封装温度与整片功耗（见 §3.3.1 的 T0）。**核显上不提供 `gpu.power`**：ADL2 的 `GFX_POWER` 会被 CPU 负载抬高（GPU 空载、CPU 满载时仍报 24–34 W），当 GPU 功耗显示会被误读 → 按 §3.3.1 的反伪造规则返回"不可用 + 理由"。逐项采用判定见 `docs/amd-facts.md` §9。
- **`gpu.vram.used` / `gpu.vram.usage` 在 ADL2 路径上默认不启用**（2026-10-15 实测后定，`defaultVisible=false`，默认视图不受影响）：取显存用量的那两个 ADL2 导出（`ADL2_Adapter_SharedVRAMUsage_Get` / `DedicatedVRAMUsage_Get`）**首次调用即把 perflib 计数器表拉进进程（+4.3 MB 私有提交且不归还）**，且实测 GPU 满载时数值恒定、疑非实时用量 → M4 再评估做成"用户显式开启 + 接受 +4.3 MB"的 opt-in（§11）。详见 §6 与 `docs/amd-facts.md` §9。

### 3.3 主板风扇转速（Ring0 必需）

- 原理：Super I/O 芯片经 LPC 总线，端口 `0x2E/0x2F` 或 `0x4E/0x4F` 的 index/data 对访问，**必须 Ring0 端口 I/O**。
- 优先支持：
  - **Nuvoton**：NCT6776F / NCT6779D / NCT6798D / NCT6687D / NCT6799D（chip ID 探测、bank select、FANIN 计数 → `RPM = 1350000 / (count × 分频)`）
  - **ITE**：IT8688E / IT8689E / IT8792E / IT8625E / IT8665E
- **可直接照搬的寄存器细节**：
  - ITE（LHM `IT87XX.cs`）：地址寄存器 = base+0x05、数据寄存器 = base+0x06、`VENDOR_ID 0x58`（取值 0x90 / 0x7F）、温度基址 `0x29`（sbyte）、电压 `{0x20..0x28, 0x2f, 0x2c, 0x2d, 0x2e}` × 增益（0.012 / 0.011 / 0.0109 / 默认 0.016）、风扇计数低位 `{0x0d,0x0e,0x0f,0x80,0x82,0x4c}` + 高位 `{0x18,0x19,0x1a,0x81,0x83,0x4d}`、16 位使能 `0x0C`（bit4/5/2）、8 位分频 `0x0B`；**16 位模式 `RPM = 1.35e6 / (count × 2)`**，8 位模式 `divisor = 1 << ((0x0B >> (3×i)) & 7)`；PWM 控制 `0x15/0x16/0x17/0x7f/0xa7/0xaf`（bit7 = 自动模式，不可读）。
  - Nuvoton（LHM `Nct677X.cs`，约 71 KB 常数表）：进入序列 `outb(0x2E,0x87)` 连续两次、退出 `outb(0x2E,0xAA)`、解锁寄存器 `0x28` 清 bit4；Chip-ID → 型号全表（`0xC3/0x30`=NCT6776F、`0xC5/0x60`=NCT6779D、`0xD4/0x2B`=NCT6798D、`0xD5/0x92`=NCT6687D、`0xD8/0x02`=NCT6799D…）。
  - 访问流程（LHM `LpcIO.cs`、nwinfo `lpc_nuvoton.c`）：**两个端口 0x2E 与 0x4E 都要探测**、写后回读校验、写后 dummy read 冲刷总线、**ISA 总线全局互斥**（`Mutexes.WaitIsaBus(10)`）——多进程同时访问 Super I/O 会互相破坏状态（和 HWiNFO/AIDA 共存时必须处理）。
- **必须记住的坑**：
  - IT87xx 只能在 `0x2E` 进入、IT8792 在 `0x4E`；若 `0x4E` 回读不是 `0xFFFF`（说明已被别家进入），强行进入会让 IT8792 **谎报 chip ID `0x8883`**。
  - IT8688E 写完地址寄存器后回读不匹配，**不能用回读校验，必须特判**。
  - IT8665E / IT8655E **每次 update 前必须 SelectBank(0)**，否则温度与转速乱跳；bank 选择在休眠/恢复后失效。
  - 型号识别必须按主板特判：X870E Nova WiFi 上 "NCT6796D" 实为 **NCT5585D**、"NCT6799D" 实为 **NCT6796DS**；MSI B840/B850/B860/X870/Z890 上 "NCT6687D" 实为 **NCT6687DR**。**只认 chip ID 会读错** → 做成"chip ID + 主板特判表"的数据驱动层。
- 笔记本：**没有独立 Super I/O**，多数机型风扇走 ACPI EC（端口 `0x62/0x66`）或厂商 WMI，寄存器映射因 OEM 而异 → 见 **§3.3.1**（温度与风扇的完整分层降级链）。
- 常数来源与许可：**nwinfo（Unlicense）可直接搬**；**LibreHardwareMonitor（MPL-2.0）** `LibreHardwareMonitorLib/Hardware/Motherboard/Lpc/*.cs` 是文件级 copyleft——移植的文件须保留 MPL 声明并保证源码可获取；纯常数可自由使用，但别照抄文件结构。交叉校验可用 `siomon`（Rust/MIT）、`VirtualSMC/SMCSuperIO`（C++）。
- 不可读时显示 `N/A`，不要伪造。
- **现实提醒**：Windows 上**不存在独立的 Super I/O 库**（只有 Linux `it87`/`nct6775` 与 macOS 实现；LHM 也没有官方 C++/Rust 移植，现存全是"包一层 .NET"的桥）。这块**没有整套现成代码可抄**，必须自建并随新主板持续维护——本项目最大的工程风险。

### 3.3.1 笔记本温度与风扇（无独立 Super I/O 的机型）

> 本节结论来自 `docs/laptop-telemetry-facts.md`（逐条带 URL 与 `repo:path:line` 的证据综述）。**本机（HP 战 66 14 G8a / AMD Ryzen 7 H 255）正是这一类机型**，故本节不是补充而是主路径之一。

**前提事实（决定了架构，不是可选项）**

1. **Windows 上不存在用户态可打开的 EC 设备对象。** `\Device\ACPIEC` 是内核对象；`IOCTL_ACPI_*` 在 DDK 头里标 `Environment: NT Kernel Model Driver only`；grep.app 全库无 `\\.\ACPIEC` 用户态先例。→ **EC 直读只能走 Ring0**，这与 §3.3 的 Super I/O 是同一个结论，风扇转速在笔记本上没有"免驱直读"这条路。
2. **免驱的厂商 WMI 通路确实存在，但多数需要管理员**：Dell 现代机型走 `root\dcim\sysman\diagnostics` → `LegacyDiags.Execute`（FanControl 维护者在 2026 Dell Pro Max 16 实测通过，无需内核驱动）；HP `root\hp\InstrumentedBIOS` → `HPBIOS_BIOSNumericSensor` 暴露**真 RPM**（`BaseUnits` ValueMap 19 = RPM、`SensorType` 12 = Air Flow）。两条路都要求提权——v1 本来就是 `requireAdministrator`，正好可用。
3. **ACPI 热区是最后兜底，且有两条路径**：`root\WMI` 的 `MSAcpi_ThermalZoneTemperature`（**十分之一开尔文**，需提权）与 `root\CIMV2` 的 `Win32_PerfFormattedData_Counters_ThermalZoneInformation`（**开尔文**，通常免提权可读）/ PDH 计数器集 GUID `{52bc5412-dac2-449c-8bc2-96443888fe6b}`。后者是免驱动的关键。
4. **ACPI 热区 provider 必须"完全惰性"（2026-10-15 实测后确定）**：连「本机有哪些热区」都推迟到**真的要读温度**时才枚举——`PdhExpandWildCardPathW` / `PdhAddEnglishCounterW` 共用同一张 perflib 计数器表，**枚举热区这一步本身就会把表拉进进程（+4.3 MB 私有提交且不归还）**。`--probe` 是诊断命令，会主动触发一次以便列出热区（`acpi.zones=` 行）；正常采样路径不触发（本机该源单次成本 0.00 ms）。

**温度分层降级链（可信度降序）**

| # | 来源 | 前置条件 | 输出 | 失败判定 |
|---|---|---|---|---|
| **T0** | **厂商免驱 PM 表（AMD ADL2）**：`ADL2_New_QueryPMLogData_Get` 的 `TEMPERATURE_CPU`(32)（绑定方式与逐项采用判定见 `docs/amd-facts.md` §9） | 已装 AMD 显卡驱动（`atiadlxx.dll`，运行期 `LoadLibraryW` 加载） | CPU 封装温度；**免驱、免管理员、无 Ring0** | `supported == 0`、读数长期不变或命中哨兵 → 丢弃，继续向下取（ACPI 兜底） |
| T1 | **MSR/SMN via PawnIO**：Intel `IA32_THERM_STATUS 0x19C` / `IA32_PACKAGE_THERM_STATUS 0x1B1`（`TjMax` 取 `MSR_TEMPERATURE_TARGET 0x1A2` bits[23:16]，温度 = `TjMax − DTS`）；AMD SMN `THM_TCON_CUR_TMP 0x00059800` bits[31:21] × 0.125 | 已装 PawnIO | CPU 温度（每核；**封装级**在 AMD 上已由 T0 提供） | 读数为 0 或长期不变 → 丢弃，**不要显示 0** |
| T2 | **GPU 温度**：NVML / ADLX / IGCL | 无 | ✅ 免驱 | 库缺失 → 跳过 |
| T3 | **厂商 WMI**：HP `HPBIOS_BIOSNumericSensor`；Dell `LegacyDiags` `eax=0x10a3`；Lenovo `LENOVO_GAMEZONE_DATA` 18/19；Gigabyte `getCpuTemp`/`getGpuTemp1`；Acer `AcerGamingFunction`；ASUS `DSTS 0x00120094/0x00120097` | 已提权 | 温度 + **来源标签** | 传感器缺失（HP `OperationalStatus` 12/13）、类不存在（MOF 未注册）→ 降级 |
| T4 | **ACPI 热区两层**：① `MSAcpi_ThermalZoneTemperature`（需提权）→ ② `Win32_PerfFormattedData_Counters_ThermalZoneInformation` / PDH 计数器集，**counter id 3「High Precision」优先于 id 0**（`id 3 = value/10 − 273`，`id 0 = value − 273`） | 无 | 温度（兜底）；provider **惰性**：只在真要读温度时才枚举热区（枚举即触发 perflib 加载） | 见下方"探测-再信任"判据 |
| T5 | **外部监视器共享内存**：Afterburner `MAHMSharedMemory` → HWiNFO `Global\HWiNFO_SENS_SM2`（需 Pro）→ CoreTemp | 对方正在运行 | 温度，**必须标注「来源=外部程序」** | 时间戳 > 15 s、`FLT_MAX`、名字不匹配 → 丢弃 |

**T0 的定位与反伪造规则（AMD 实测，2026-10-14）**

- AMD 上 T0 **先于 T1（Ring0 MSR/SMN）与 T4（ACPI 热区）生效**：`cpu.temp.package` 由 ADL2 PM 表提供，无需 PawnIO、无需管理员（本机实测 IsAdmin=False、未装 PawnIO、HVCI 开启）。Ring0 在 AMD 上不再是封装温度/功耗的唯一路径。
- **T1（Ring0 SMN）的温度口径是 Tctl，不可与 T0 的 ADL2 直接比较（提权实测）**：同一 20 s 并发实验里，SMN `THM_TCON_CUR_TMP`（`0x59800`）随负载 66.75 °C → 峰值 **88.1 °C**（t ≈ 9 s）→ 衰减回 72.75 °C，而同窗口 ADL2 `TEMPERATURE_CPU` 只有 **57–66 °C**（ACPI 热区读数更低且滞后约 20 s）—— **相差 10–20 °C**（SMN 是 Tctl/控制温度，ADL2 PM 表是厂商 die/skin 口径）。因此 T0 在场时不让位给 T1，且 Ring0 给出的 `cpu.temp.package` 必须在 detail 里写明这是 Tctl。实测记录见 `docs/ring0-pawnio.md` §5.4、§7。
- 响应速度实测（8 线程 CPU 满载 22 秒同时刻对照）：ADL2 在 1 秒内 `49 → 73–76 °C`；ACPI 热区 `48.1 → 60.1 °C`（1 °C 步进、约 1 °C / 2 秒，明显滞后）→ **ACPI 只作兜底**（provider 仅在槽位仍为空时写入），Intel 或无厂商 PM 表的机器才轮到它。
- 同一通道的功耗口径：`cpu.power.package` 取 `ASIC_POWER`（APU 上即整片功耗，含 GPU 与 uncore）、`cpu.power.soc` 取 `SOC_POWER`；**不取 `CPU_POWER`**（0–6 W，量级与 28–45 W 的 TDP 级不符）。
- **反伪造规则（GPU 功耗）**：核显（APU）上 CPU 与 GPU 共享供电轨，ADL2 的 `GFX_POWER` 在 **GPU 完全空闲**（`INFO_ACTIVITY_GFX` 仅 1–8%、`CLK_GFXCLK` 恒 800 MHz）、**CPU 满载**时仍报 24–34 W，与 `ASIC_POWER`（32–33 W）几乎重合 → **核显上不得把 `GFX_POWER` 当作 `gpu.power`**：一律返回"不可用"并给出理由原文「核显与 CPU 共享供电轨：ADL2 GFX_POWER 在 GPU 空载、CPU 满载时仍报 24–34 W，无法作为 GPU 功耗（整片功耗见 cpu.power.package）」。**独显**上 `GFX_POWER` 作为 GFX 供电轨读数照常采用。
- 判据用"GPU 空载 + CPU 满载时该读数是否仍与整片功耗重合"，**不要相信传感器名字**；凡"CPU 与 GPU 共享供电轨"的 APU 都适用。
- **该通道不引入 perflib**：ADL2 只被 `CLK_*` / `TEMPERATURE_*` / `*_POWER` 这些 **PMLog 传感器**触碰时是便宜的（单次 0.76 ms）；**一旦调 `ADL2_Adapter_SharedVRAMUsage_Get` / `DedicatedVRAMUsage_Get`，perflib 计数器表立刻被拉进进程（+4.3 MB 私有提交且不归还）**——所以显存用量项默认不启用（见 §11 与 `docs/amd-facts.md` §9）。
- **`AskForSupported` 式判定进代码**：`Adl2Provider::Claims(std::string_view metricId) const` 只有在①该适配器绑定存在、②目标传感器索引 > 0、③该索引出现在**本次** PMLog 的 `supported` 列表里时才返回 true；Collector 据此判断"ADL2 真能提供 `cpu.freq.avg`"才跳过 PDH（不要用"ADL2 库存在"当判据）。

**风扇转速分层降级链**

| # | 来源 | 输出语义 | 前置条件 |
|---|---|---|---|
| F1 | **PawnIO EC**：`LpcACPIEC.p`（仅放行端口 `0x62`/`0x66`）+ 机型寄存器表 | 真 RPM；**若该表给出的是原始标量，必须按标量呈现，不得标成 RPM** | 已装 PawnIO **且**机型在表中 |
| F2 | **确认是 RPM 的厂商 WMI**：HP 方法 `0x11`（大端 uint16，输出偏移 2..3）；HP Victus-S 查询 `0x2D`（**返回百转，不是 RPM**）；Dell `LegacyDiags` `eax=0x02a3` × `fan_mult`（30 或 1）；Lenovo `LENOVO_FAN_METHOD.Fan_GetCurrentFanSpeed` / `CAPABILITY_DATA_00.fanX_input`；Gigabyte `getRpm1`/`getRpm2`（**字节序按 DMI product family 决定**） | 真 RPM | 已提权 |
| F3 | **只有百分比的厂商 WMI**：ASUS `DSTS 0x00110013/0x00110014` | **百分比，不是 RPM** | **不做换算**；UI 标为「转速百分比（厂商固件）」 |
| F4 | **GPU 风扇**：NVML percent / ADLX / IGCL | 百分比或 RPM | 免驱 |
| F5 | **N/A** | — | 见下 |

**EC 路径的硬性实现要求（缺一不可）**

1. 先取 `Global\Access_EC` 互斥体（World ACL 创建，失败则 `OpenMutex`），超时 200 ms 级；**必须校验对象位于 `\BaseNamedObjects`，绝不接受会话本地回退**（WinMonitor2 commit `abbbfbc` 的真实教训）。
2. 只做精确字节读（实测一次 EC 字节事务 1.5–2 ms），轮询周期 ≥ 1 s；0.5 s 级的多字节扫描实测带来 +≈1 W 功耗与 uncore 频率下降。
3. 读失败**保留上次好值并标 `Stale`，不归零**。
4. **只读，不写**：v1 完全不碰 ThinkPad `0x93`/`0x94` 这类"写入即改变状态"的寄存器。
5. PawnIO `LpcACPIEC.p` **只提供 `ioctl_pio_read`/`ioctl_pio_write` 端点，模块内不实现 IBF/OBF 轮询与 RDEC/WRDC 事务**——EC 协议握手必须由我方用户态完成。
6. 机型寄存器表走 `sensor-tables/<vendor>-<model>.json` + `[issues]` 收集流程（与 `docs/release-github.md` §11 的既有约定打通），字段至少含：EC 寄存器、宽度、字节序、公式，以及**「这是否是真转速表」的显式标记**（反面案例：Acer PH315 的"风扇转速"实为 0..6122 原始标量，FanControl 仍标成 RPM）。

**探测-再信任（所有来源在显示前必须通过的判据）**

1. **值非 0**——`0` 表示无数据，不是 0 RPM。
2. **不等于哨兵值**——`2732`（ACPI 十分之一开尔文 = 0.0 °C）、`273.15`、`0xffff`、Dell 的 `0x99`（153，瞬时假值需重试）/`0xc1`（193，GPU 已关闭）。
3. **同机多传感器不应全同值**——ACPI 热区尤其要先判定"是不是所有热区都报同一个常量"。
4. **数值应随负载变化**——加载后仍恒定 → 不显示。

**诚实降级文案（UI 必须明说原因，不留空、不显示 0）**

| 情形 | 提示要点 |
|---|---|
| 未装 PawnIO 且厂商无 WMI 类（多数 ThinkPad / Clevo / 白牌） | 「本机风扇转速需 PawnIO 驱动读取 EC，当前不可用」 |
| AMD 笔记本未装 PawnIO 且无厂商 WMI，**且 ADL2 通道也不可用**（非 AMD 显卡 / 显卡驱动未装 / `TEMPERATURE_CPU` 的 `supported == 0`） | 「本机 CPU 温度需要内核态读取（PawnIO），当前不可用」——**不要显示 0**；ADL2 可用时不应出现本提示（见上方 T0） |
| 厂商 WMI 类未注册（MSI `MSI_ACPI`、Gigabyte `GB_WMIACPI_Set`） | 「厂商 WMI 类未注册（需厂商软件运行一次）」 |
| ACPI 热区不存在 / 全部同值 / 恒为 2732 | 「本机固件未提供可用的 ACPI 热区」 |
| Microsoft Surface 全部机型 | 「Windows 无 Surface 风扇/温度公开接口」，**永久 N/A** |
| ASUS 只给百分比 | 显示百分比并标注「厂商固件百分比，非转速」 |

**明确没有一手来源的路径（编码时不要假设其存在）**

- ThinkPad **完全没有传感器 WMI 类**（只能走 EC；TPFanControl 需要 TVicPort 内核驱动，且存在两套互不兼容的风扇等级编码 0–7 与 255–0）。
- Surface 在 Windows 侧**无公开接口**（SAM/EC 仅内核内部）。
- `DCIM_TemperatureProbe` 类、MSI `micommon` 类名：均未找到证据。
- **Windows 上从 MSI `MSI_ACPI` 读风扇 RPM 无任何实例**（该 WMI 类需厂商 MOF 注册后才存在，且上游警告其方法**非线程安全** → 所有调用必须串行化）。

**跨切面规则**

- `Global\Access_EC`、`Global\Access_ISABUS.HTP.Method`、`Global\Access_PCI` 三把互斥体都要按 World-ACL 配方创建并 **fail-closed**。
- Ring0 只走 PawnIO。除已有的 `LpcIO.p`/`IntelMSR.p`/`AMDFamily17.p`/`RyzenSMU.p` 外，笔记本路径还需 **`LpcACPIEC.p`（EC）、`DellSMM.p`（Dell SMM）、`IntelPCHThermal.p`（Intel PCH 温度）**。
- **SMM 调用必须钉在 CPU 0**（Dell 接口只在 CPU 0 定义）；`DellSMM.p` 已在模块内做亲和性处理，自行实现须复刻。
- 不要把"外部程序共享内存"与"本机传感器"混在同一数值列，来源必须可见。
- v2 服务拆分后由 SYSTEM 服务持有 PawnIO 句柄与全部互斥体，普通权限 UI 经 IPC 取数。


### 3.4 FPS / 帧时间 / 1% Low

**采集路径（按推荐顺序）**

1. **PresentMon Service + SDK（首选）**
   - PresentMon 2.3.1+ 安装后，Windows 服务 `PresentMonSharedService` 以 **SYSTEM** 运行（ETW 由它跑），随安装部署 `PresentMonAPI2.dll`；SDK 头/库默认在 `C:\Program Files\Intel\PresentMon\SDK`（`PresentMonAPI.h`、可选 `PresentMonAPI2Loader.lib/.dll`）。
   - 我方作为客户端：动态加载 `PresentMonAPI2.dll` → 打开会话 → 注册帧查询 → **轮询消费帧数据**；所有 API 返回 `PM_STATUS` 枚举（详细诊断见 `PresentMonDiagnostics.h`）。服务同时提供它自己聚合的硬件遥测（GPU 温度/功耗/占用，走 NVAPI 等），可作为 GPU 指标的备用来源。
   - 优点：**客户端零提权、不注入、不挂钩**，反作弊风险为零，维护成本最低。
   - 必须注意：
     - 只能动态加载系统已安装的那份 DLL —— 官方明确：客户端自带 `PresentMonAPI2.dll` 时**不保证与服务二进制兼容**。
     - 数据链路是 ETW → 分析库 → 服务缓冲 → 客户端查询，**存在固有秒级延迟**（Intel 自家 Capture Application 的 overlay 也是低频刷新）；维护者亦确认数据源自 ETW 有延迟。因此 Overlay 只做 **1 Hz 级聚合显示**，帧级精确统计放到**会话结束后**计算，**不要承诺逐帧实时曲线**。
     - 服务异常退出由安装器配置的 SCM recovery 自动重启（2 s → 15 s → 60 s → 5 min，第 5 次后保持停止；30 min 无失败后计数重置）；**旧会话不会重连**，下一次调用返回 `PM_STATUS_SESSION_NOT_OPEN`，必须由我方自动重开会话。
2. **子进程 `PresentMon.exe`（没装服务时的降级路径）**：`PresentMon.exe --process_name xxx.exe --output_stdout --v2_metrics --no_console_stats` 输出**逐帧 CSV 到 stdout**（官方没有 JSON 输出选项），逐行解析即可；另有 `--output_file`、`--multi_csv`、`--hotkey ALT+SHIFT+F11`、`--timed N`、`--session_name`、`--stop_existing_session`、`--restart_as_admin`、`--exclude_dropped` 等可用。
   - 代价：多一个进程 + 文本解析；且**需要管理员权限，或把用户加入 `Performance Log Users` 组**，否则报 `failed to start trace session (access denied)`。因此只作为"服务不可用"时的兜底。
3. **自建 ETW 会话**（零依赖，需管理员）：`StartTrace/EnableTraceEx2/OpenTrace/ProcessTrace` 订阅 `Microsoft-Windows-DxgKrnl` 的 Present 事件。自研等于重新实现 D3D9/DXGI/D3D12 的 present 语义（PresentMode 判定、Dropped、失焦、Runtime 识别），代码量最大，仅作长期选项；Rust 侧可参考 `n4r1b/ferrisetw`（MIT）。
4. **不采用：DLL 注入 / Present() 挂钩** —— 反作弊封号风险、需要签名 DLL、版本适配成本高（RTSS 那套）。注意：真独占全屏下想显示 Overlay，注入几乎是唯一路径（见 §7）。

**关键帧字段**（PresentMon 2.x 口径）：`MsBetweenPresents`(帧时间)、`MsBetweenDisplayChange`、`MsUntilDisplayed`、`DisplayLatency`、`MsCPUBusy`/`MsCPUWait`、`MsGPUTime`/`MsGPUBusy`/`MsGPUWait`、`VideoBusy`、`MsAnimationError`、`ClickToPhotonLatency`/`MsPCLatency`、`Dropped`、`PresentRuntime`(DXGI/D3D9)、`AllowsTearing`、`PresentMode`。

- `PresentMode` 取值（可直接用来做性能诊断）：`Hardware: Legacy Flip`（真独占全屏，外部窗口无法覆盖）、`Hardware: Independent Flip`（iFlip，延迟最低）、`Hardware Composed: Independent Flip`（拿到 MPO 硬件 overlay plane）、`Composed: Flip`（走 DWM 合成，延迟更高）、`Composed: Copy with GPU/CPU GDI`（最差）。
- **自检价值**：记录"我们的 Overlay 打开前 / 打开后 `PresentMode` 是否从 `Hardware: Independent Flip` 掉到 `Composed: Flip`"——这是"我们的 Overlay 有没有伤害游戏"的客观指标（见 §7）。

**派生指标口径（必须在 UI/关于页写清，避免与别家对不上）**

- 平均 FPS = `帧数 × 1000 / Σ帧时间`（调和平均）；**不能**用帧时间算术平均再取倒数。
- **1% Low**：帧时间降序，取最差 1% 帧的**平均帧时间** `T1`，`1% Low = 1000 / T1`；0.1% Low 同理。
  - **移植注意口径差异**：CapFrameX（MIT，`source/CapFrameX.Statistics.NetStandard/FrametimeStatisticProvider.cs`）的实现是**分位阈值选样**——`1000 / seq.Where(e => e >= Quantile(0.99)).Average()`，即"≥ P99 分位的所有帧取平均"，与"最差 `ceil(N×0.01)` 个"在样本少时不等价。建议两种都算，并在 tooltip 标注当前口径。
  - 同一文件还有注释自称"MSI Afterburner 等价"的**时间加权 integral 口径**（`GetPercentageHighIntegralSequence`）—— 要和 Afterburner 对拍时切到它。
- 双口径并存：**会话累计**（整段游戏）+ **滚动窗**（最近 10 s，供 Overlay 显示）。
- 异常帧剔除：`frameTime ≤ 0`、`> 1000 ms`、加载/失焦期帧标记无效，不参与统计并记录剔除数量；亦可参考 CapFrameX 的离群剔除（`TAU = 0.999`、`ceil(N×0.001 − 1e-9)`，按值删除最慢的 0.1% 且保持帧序）。
- **卡顿（stutter）**：别拍脑袋定阈值，用 CapFrameX 的无魔数定义 `100 × count(ft > factor × avg) / count`；其"时间版"以**移动平均**为基线，窗口 `round(sqrt(avgFrameTime) × 10)`（16.7 ms 平均 → 40 帧）。可再加"帧节奏直方图"：按 `|ft[i] − ft[i−1]|` 分 <2 / <4 / <8 / <12 / 其它 五档。
- 统计实现：环形缓冲 16k~64k 帧时间（128~512 KB）→ 每秒排序一次（≤0.5 ms）算均值/百分位；同时维护 0.1 ms 桶、上限 200 ms 的直方图供实时曲线。全程零动态分配。
  - **别照抄 CapFrameX 的热路径**：它的分箱是 `O(bins × N)`，只能离线跑；实时侧要用排序 + 部分和。

**参考项目的可直接借用项**

- **CapFrameX**（[CXWorld/CapFrameX](https://github.com/CXWorld/CapFrameX)，★1331，MIT，C#）：统计口径金矿，上述 1% Low / 0.1% Low / Afterburner integral / stutter / 帧节奏直方图 / 离群剔除全部可直接移植为 C++（注意其 `[ThreadStatic]` 缓冲复用思路 = 我方"零分配"要求）。
- **PresentMon**（[GameTechDev/PresentMon](https://github.com/GameTechDev/PresentMon)，★2626，MIT，C++）：SDK 接入 + 字段语义权威来源；其 `PresentMode` 字段同时是我们 Overlay 自检的依据。
- **MangoHud**（[flightlessmango/MangoHud](https://github.com/flightlessmango/MangoHud)，★9129，MIT，C）：HUD 元素化布局（文字/折线/直方图）与表驱动配置键的设计可借；实现是 Vulkan layer 注入，不可移植。
- **ferrisetw**（[n4r1b/ferrisetw](https://github.com/n4r1b/ferrisetw)，MIT，Rust）：日后若自研 ETW 消费者可参考其会话/回调封装。
- **不采用**：Special K（[SpecialKO/SpecialK](https://github.com/SpecialKO/SpecialK)，GPL-3.0）—— 仅借用其"MPO plane 检测"思路，代码有 GPL 传染性，不可并入。

> **实现现状（M2 已完成的那一半）**：解析器、统计口径与子进程采集链路已落地并通过 71 项单测与夹具端到端验证；**尚未验证**的是真机 PresentMon + 真游戏下的 1% Low 对比与 CPU 开销。口径速查、容错清单、诚实边界与"为什么选子进程 CSV 而不是 SDK"写在 `docs/fps-presentmon.md`，本节仍是**设计与数值的权威**；两者冲突时以本节为准并回头修正实现。

---

## 4. Ring0 访问与驱动方案（本项目的最大风险项）

**现状（决定了不能踩的坑）**

- **自研驱动不可行**：Win11 22H2 起，新的内核模式驱动必须 **WHQL 签名**（证明签名/attestation 不再适用于 22H2 及以后版本），个人/小团队走不通。
- **绝对不要用 WinRing0**：已被微软列入易受攻击驱动阻止列表（CVE-2020-14979），Defender 直接报 `HackTool:Win32/Winring0` / `VulnerableDriver:WinNT/Winring0`；LHM issue #1660、#1873，FanControl issue #3016（原话：R0FanControl 服务因文件含病毒无法启动）都是实例。LHM 由 namazso 亲自提 PR #1857，于 **v0.9.5（2025-09-16）彻底换成 PawnIO**，FanControl V238 同步 —— 这条路已经死了。
- **推荐 PawnIO**：namazso 的"可脚本化通用内核驱动"（https://pawnio.eu/ ，已签名、专为硬件访问设计、兼容内存完整性）。接入方式：`\\.\PawnIO` 打开设备 → 加载模块 → IOCTL 执行读写。**我们只写用户态调用代码，不碰内核编译与签名。**
  - 需要的模块（来自 `PawnIO.Modules`）：`IntelMSR.p`（MSR 读写）、`AMDFamily0F/10/17.p`（按 CPU family 选）、`RyzenSMU.p`（PM 表）、`LpcIO.p`（端口 I/O / Super I/O）。
  - 调用范式可直接照 nwinfo：`WR0_ExecPawn(h, &h->pio_intel, "ioctl_read_msr", &in, 1, &out, 1, NULL)`，并配 `--driver=CPUZ162|NwHwIo|PawnIO|NONE` 式回退链（找不到驱动 → 自动退免驱模式）。
  - 用 PawnIO 仍需**安装驱动 + 模块**，Windows 会弹"安装驱动"类安全提示 → 必须像 LHM PR #1777 那样提供**"禁用 Ring0 驱动安装"开关**，让用户能选纯免驱模式。
  - **必须提权（实测根因）**：PawnIO 驱动的设备对象 ACL 来自其 INF（本机 `...\DriverStore\FileRepository\pawnio.inf_amd64_a72a2f969b8b7496\pawnio.inf`）里的 `HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)"` —— **只授权 SYSTEM 与 Administrators**，交互式用户没有任何权限位。非管理员运行 `--probe` 会拿到 `pawnio=present` / `ring0Backend=PawnIO`，但 `ring0.unavailableReason=打不开 PawnIO 设备（错误 5（拒绝访问。））`。因此 **Ring0 通道只能在提权进程里跑**（与 §2 的 v1 `requireAdministrator` 一致）；**免驱路径（ADL2 + 原生 CPU 采样 + 惰性 ACPI）不受影响、无需提权**。详见 `docs/ring0-pawnio.md` §3.1、§7。
- **许可处理（决策 ③=B 已定）**：`PawnIO`（驱动／安装包）为 **GPL-2.0**，`PawnIO.Modules`（`.bin` 模块）为 **LGPL-2.1**（GitHub REST API `license.spdx_id = LGPL-2.1`；仓库 `COPYING` 记 LGPL-2.1-or-later）。我们**不链接、不复制其源码**，只在运行期经 IOCTL 调用；**官方签名安装包随发布包附带**，首次运行检测未装则**引导用户确认后静默安装**（可拒绝 → 免驱模式），并随包提供其许可证原文与**对应源码获取说明**（驱动 GPL-2.0、模块 LGPL-2.1；若随包提供模块还须保证模块可被用户替换；发布前须做法务确认）。**不**拆发驱动 `.bin`/`.sys`，也**不**自研安装器与驱动签名。合规动作逐条见 `docs/release-github.md` §3.5。
- **免驱动"轻量模式"（必须有）**：仅 PDH CPU 频率 + NVML/ADLX/IGCL GPU 全指标；CPU 温度/功耗/主板风扇显示 `N/A` 并给出明确原因提示。这条路径让工具在 Vanguard 等强反作弊环境下依然可用。
  - **AMD 例外（已实测）**：AMD 显卡驱动自带的 `atiadlxx.dll` 提供免驱 PM 表，**非管理员、无 Ring0** 即可读封装温度（`TEMPERATURE_CPU`）与整片功耗（`ASIC_POWER`）→ `cpu.temp.package` / `cpu.power.package` / `cpu.power.soc` 在 AMD 上**不属于**"显示 N/A"的那一类；Intel 上仍为 N/A（需 MSR DTS / RAPL）。反之核显上的 `gpu.power` 因共享供电轨被污染而**拒绝提供**。实测数据见 `docs/amd-facts.md` §9，温度链见 §3.3.1 的 T0。

---

## 5. 技术栈选型

**主推：C++20 / MSVC / CMake**
- `/MT` 静态 CRT，`/O2 /GL /LTCG /GS`，产物 2–4 MB，工作集 ~15 MB。
- UI：纯 **Win32 + Direct2D/DirectWrite** 自绘（卡片 + 帧时间曲线），无 Qt/MFC/Electron。
- 依赖：配置读写用**自研极简 JSON**（零第三方依赖，见 `docs/config-schema.md` 不变量 6）；PresentMon 数据是**逐帧 CSV**（官方无 JSON 输出），无需 JSON 库。`wil`（MIT，RAII/资源）是否引入留待编码前拍板；能用 `std::format` 就不引 fmt。
- 日志：内存环形 + 异步落盘 CSV（双缓冲，采集线程零阻塞）。

**备选 A：C# .NET 9 + NativeAOT + `LibreHardwareMonitorLib`（MPL-2.0）+ PresentMon SDK P/Invoke**
- 优点：CPU/GPU/风扇/功耗/温度**一次到位**，1–2 天就能出全功能原型，是所有竞品数值的"黄金对照实现"。
- 缺点：体积 25–40 MB、内存 60–100 MB、占用偏高 → **不满足"极致轻量"目标，建议只作为内部原型/对照工具**。

**备选 B：Rust**（`windows-rs` + egui/winit）
- NVML/ADLX/IGCL/PawnIO 都要手写 FFI，工作量与 C++ 相当；内存安全性更好、二进制略大（PerfWindow 就是这个栈）。

**C++ 侧的"参考实现"（调研结论，很重要）**

- **nwinfo**（[a1ive/nwinfo](https://github.com/a1ive/nwinfo)，★698，**Unlicense＝公共领域**，C）：Intel/AMD MSR 全表与位域、Super I/O 探测流程、PawnIO 调用范式、驱动回退链 —— 与我们的实现同源（C → C++），**几乎可以整段搬**，是本项目最省事的起点。
- **注意**：并不存在 LHM 的官方 C++/Rust 移植（LHM discussion #1489、issue #1576 均无产出），现存项目（LHM-CppExport、lhm-service）都是"包一层 .NET"的桥 —— 所以 CPU 温度/功耗/主板风扇这部分**必须自己写**，只能搬常数表。

> 结论：正式产品用 C++20，以 nwinfo 为骨架；先用 C# + LHM 花两天做"数值基准原型"用于校准；常数表取 nwinfo（Unlicense）优先、LHM（MPL-2.0，需文件级隔离）补充。

---

## 6. 采样策略与性能预算

| 采样项 | 周期 | 单次成本预算 |
|---|---|---|
| MSR 读取（频率/温度/RAPL） | 500 ms（频率 200 ms，仅性能视图开启时） | 1–5 µs/次 |
| NVML | 500 ms | 5–50 µs/次 |
| ADLX | 1 s（初始化重，缓存 `IADLXPerformanceMonitoringServices` 实例） | 0.1–1 ms |
| SuperIO | 1 s | 10–100 µs |
| PresentMon 帧事件 | 事件驱动（帧级） | 回调 + 每 200 ms 拉一次遥测 |
| UI 刷新 / Overlay | 4 Hz / 2–4 Hz | 仅在可见时重绘 |

**实测采样成本（本机 `winperfmon --watch 5`，11 次采样）**：整轮**平均 0.92 ms/次、最大 1.54 ms/次**。

| 源 | 平均单次耗时 | 说明 |
|---|---|---|
| `adl2` | **0.76 ms** | 唯一有实感的源（`ADL2_New_QueryPMLogData_Get` 单次取 20 个传感器） |
| `cpu.native` | **0.13 ms** | `NtQuerySystemInformation` 单次系统调用；**零常驻内存**（实测 0.04–0.13 ms） |
| `memory` | **0.02 ms** | `GlobalMemoryStatusEx` |
| `acpi-thermal` | **0.00 ms** | 惰性 provider：没轮到它就不做任何工作 |

对照旧构建（PDH 路径）：`cpu.pdh` 单源 **0.48 ms**，整轮**平均 23.65 ms、最大 295 ms**——PDH 的"便宜"只是单次采样，代价在**首次触碰时的 perflib 加载**与随后的常驻内存（§3.1 与下方冷启动）。
（上表为 2026-10-15 那轮的记录值；复测一轮为 `adl2 0.88`、`cpu.native 0.18`、`memory 0.01`、整轮平均 `1.09 ms / 最大 3.28 ms`——**同一结论看量级即可，末位逐轮抖动**。）

**实测冷启动（`--probe` 打点）**：`startupMs = 45.8`、`firstStepMs = 0.91`；分段（ms）：`stage.gpu-enum 24.6`、`stage.adl2 20.4`、`stage.cpu.native 0.4`、`stage.memory 0.0`、`stage.acpi-thermal 0.0`、`stage.首帧发布 0.0`（启动类数字逐次抖动几毫秒，复测一轮 `startupMs 44.1`；判据看量级）。`--version`（不采集）进程墙钟 5 次：**23.3 / 11.2 / 11.5 / 10.6 / 10.9 ms**。
对照旧构建（PDH 路径）：`startupMs = 435.7`，其中 **`stage.cpu.pdh = 381.2`** —— 这就是 perflib 计数器表的加载代价，占整轮启动的 87%。

**实测内存阶梯（工作集 / 私有，MB）**：进程入口 5.96 / 1.22 → 配置前 6.14 / 1.27 → 配置加载后 6.19 / 1.27 → DXGI 枚举后 8.73 / 1.80 → `provider:memory` 8.77 / 1.80 → `provider:adl2` **11.75 / 2.46** → `provider:cpu.native` 11.76 / 2.46 → `provider:acpi-thermal` 11.77 / 2.46 → provider 初始化后 11.77 / 2.46 → **首帧后 11.83 / 2.46**。

- **首帧不再有 +4.6 MB 的跳变**：有 perflib 计数器表时，那次跳变出现在"首个碰表的调用"处（本机实测为 ACPI 初始化或首帧发布）；默认路径不碰表，因此首帧只 +0.06 MB。
- 常驻大头是 DXGI 枚举（+2.5 MB 工作集，免驱 GPU 枚举的最低成本）与 ADL2（+3.0 MB 工作集 / +0.66 MB 私有）。**这两项都不可省**，其余源基本免费。

- 空闲降频：无前台全屏应用/窗口隐藏 → 采样降到 2 s；最小化到托盘 → 停止 UI 重绘（若在录制则保留 FPS）。
- 线程模型：1 个采集线程（`CreateWaitableTimerEx` 高精度定时器 + 事件等待，不用轮询 Sleep 循环）+ UI 线程 + PresentMon 回调线程；共享数据用双缓冲快照 + `std::atomic` 序号发布，读侧无锁。
- 权限：v1 用 `requireAdministrator` 清单；v2 拆服务后 UI 降为普通权限。

---

## 7. Overlay 方案（含独占全屏的现实约束）

- **无注入置顶 Overlay**：`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST` + Direct2D 绘制，`UpdateLayeredWindow`/`SetLayeredWindowAttributes` 做逐像素 alpha；用 `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 重新置顶；鼠标穿透、无任务栏项、可配置位置/透明度/项目组合。
- **能做到**：所有"无边框窗口化"游戏（D3D12 的所谓"独占全屏"实为 borderless flip，也在其中）、桌面、浏览器视频。
- **做不到**：**真独占全屏 `Hardware: Legacy Flip`（DXGI FSE / D3D9 Fullscreen）** —— 置顶窗口无法覆盖。这不是我们的实现问题：PresentMon 维护者已在 issue [#212](https://github.com/GameTechDev/PresentMon/issues/212) 明确确认 —— FSE 下不允许外部窗口置顶，Steam / Special K / RTSS 能显示**正是因为它们注入**。
- **⚠️ 必须正视的代价（比"看不见"更严重）**：**我们自己的置顶窗口会把游戏踢出 independent flip** —— `PresentMode` 从 `Hardware: Independent Flip` 变为 `Composed: Flip`，DWM 介入 → **延迟升高、VRR/FreeSync 失效**。这不是理论推测，Intel 自家的 PresentMon Overlay 就是活案例：issue [#235 "Lose iFlip if the overlay is brought up"](https://github.com/GameTechDev/PresentMon/issues/235)、[#367 "Opening overlay changes PresentMode"](https://github.com/GameTechDev/PresentMon/issues/367)、[#689 "Overlay disables VRR"](https://github.com/GameTechDev/PresentMon/issues/689)。
  - **对策 1（必做）**：内置**降级自检** —— 读 PresentMon 的 `PresentMode`，记录 Overlay 打开前/后的变化；一旦检测到 `Independent Flip → Composed: Flip`，明确提示"本 Overlay 正在增加延迟/影响 VRR"并支持一键关闭（另提供"游戏中自动隐藏"）。
  - **对策 2（竞技/全屏场景推荐默认）**：把 **RTSS OSD 后端作为首选** —— RTSS 自己注入，因此**不触发 iFlip 降级**；我们只写文本，不承担注入责任。
  - **对策 3**：Overlay 刷新压到 1–2 Hz（数据链路本来也只有这个粒度，见 §3.4），减少 DWM 合成负担。
- **RTSS OSD 后端实现要点**：共享内存映射名 `RTSSSharedMemoryV2`，结构体 `RTSS_SHARED_MEMORY` / `RTSS_SHARED_MEMORY_OSD_ENTRY`；按 `dwSignature`、`dwVersion`、`dwAppEntrySize`、`dwOSDEntrySize`、`dwOSDArrOffset`、`dwOSDFrame` 协商；写入方把文本写进 `szOSD` 并递增 `dwOSDFrame` 即可，**无需注入**（RTSS 自己负责注入渲染）。HWiNFO / AIDA64 / CapFrameX 用的都是这个机制。
- 热键：`RegisterHotKey`（不要用低级键盘钩子）切换 Overlay / 锁定位置；提供"仅游戏前台时显示"选项。

---

## 8. 里程碑与验收标准

| 里程碑 | 工作量 | 内容 | 验收(DoD) |
|---|---|---|---|
| **M0 骨架 + 免驱指标** | 2–3 天 | CMake 工程、Win32+Direct2D 窗口/托盘、采集调度器、CPU 频率与占用（已改为 **`cpu.freq.avg` 走 ADL2 `CLK_CPUCLK` + `cpu.usage` 走原生 `NtQuerySystemInformation`**，PDH 退为末位降级）、NVML 全套 GPU 指标、JSON 配置、CSV 日志 | 双显卡机器上数值与 HWiNFO 一致（±1 °C / ±50 MHz / ±5% 功耗）；内存 ≤15 MB（**本机实测 11.88 MB 工作集 / 2.46 MB 私有**）、冷启动 ≤300 ms（**实测 45.8 ms**） |
| **M1 Ring0 传感器** | 3–5 天 | PawnIO 集成、MSR 模块（APERF/MPERF、THERM_STATUS、RAPL）、AMD SMN 温度、SuperIO 探测 + 风扇 RPM、笔记本 EC / 厂商 WMI 降级链（§3.3.1）、豁免降级逻辑 | Intel 12–15 代 + AMD Zen3–5 上 CPU 温度/功耗/风扇全部可用；HVCI 开启不报错；笔记本上「读不到」时给出准确原因而非 0 |
| **M2 FPS 引擎与统计** | 3–5 天 | PresentMon Service 客户端（含子进程/自建 ETW 两条备选路径探测）、帧环形缓冲、会话统计、1%/0.1% Low、stutter 计数、CSV 导出 | 与 CapFrameX 同局对比 1% Low 偏差 ≤0.5 FPS；FPS 采集额外 CPU ≤1% |
| **M3 Overlay 与交互** | 4–7 天 | **RTSS OSD 后端（主路径）**、自绘无注入 Overlay（回退）、布局配置、热键、**PresentMode 降级自检**、RTSS OSD 后端、深色主题、异常提示 UI | 无边框游戏中稳定显示 4 h 无闪烁/掉帧；FSE 下正确提示"需 RTSS 后端"；检测到 `Independent Flip → Composed: Flip` 时能提示并一键关闭 |
| **M4 打包/兼容/文档** | 3–6 天 | 单 EXE 打包、PawnIO 检测与引导安装、兼容矩阵测试、崩溃转储、README/口径说明 | 干净系统免安装运行；降级路径全部走通并给出准确原因 |

合计约 **4–7 周**（单人，含联调；已含决策 ③=B/④=B 带来的 M3 +1–2 天、M4 +1–2 天）。

> **M1 进度注（2026-10-15，替代 2026-10-14 版）**：M1 的**免驱部分已完成且已重做冷启动/内存预算**——三个免驱通道都在，且都**不碰 perflib 计数器表**：①封装温度 `cpu.temp.package`（ADL2 `TEMPERATURE_CPU`）、整片功耗 `cpu.power.package`（`ASIC_POWER`）、SoC 功耗（`SOC_POWER`）；②`cpu.freq.avg`（ADL2 `CLK_CPUCLK`(34)，本机空闲 3775 → 满载 4625 MHz）；③`cpu.usage`（原生 `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`，≈ `% Processor Utility` 口径，见 §3.1）；ACPI 热区 provider 已改为**完全惰性**（只作 Intel 或无厂商 PM 表机器的兜底）。
>
> 本机实测（免驱全量）：**工作集 11.88 MB / 私有 2.46 MB / 峰值 12.07 MB**，**`startupMs = 45.8`**，整轮采样**平均 0.92 ms、最大 1.54 ms**；`--once` 汇总**可用 13 / 本机无 10 / 待接入通道 27 / 错误 0（合计 50）**。逐源成本、冷启动分段与内存阶梯见本文件 §6；验收步骤见 `docs/build-and-run.md` §6；ADL2 侧实测证据（含 perflib 归因）见 `docs/amd-facts.md` §9。
>
> **M1 Ring0 验收注（替代上一条里「仍未开始的是 Ring0 部分」的判断）**：Ring0 通道已在**本机（HP ZHAN66 14 G8a / Ryzen 7 H 255 / Win11 build 26300）以管理员身份**、用 **PawnIO 2.2.0 + 官方 AMD 模块**跑通并通过验收 —— 每核频率 **16/16 核可用**（`perCoreMHzMin 3713` / `perCoreMHzMax 4529`）、SMN 封装温度（`tempRaw 0x774B0000` → **70.2 °C**）、MSR 封装能耗（`PWR_UNIT 0x000A1003` → ESU = 16 → 能量单位 `2^-16 J`，`pkgPowerW 25.8`）；`--all` 汇总 **可用 13 / 本机无 12 / 待接入通道 25 / 错误 0（合计 50）**；`--watch 12` **平均 1.66 ms/次、最大 3.34 ms/次**（25 次），工作集 **11.96 MB** / 私有 **2.47 MB** / 峰值 **12.05 MB**，逐源 `cpu.ring0 0.65 ms`，`startupMs = 43.4`；单测 **43 passed / 0 failed**（新增 8 个 Ring0 解码口径用例）。验收步骤见 `docs/build-and-run.md` §7，逐字输出与三条口径结论见 `docs/ring0-pawnio.md` §5.3–§5.5、§7。
>
> **两条产品级结论（已同步进 §3.3.1 与 §4）**：①**Ring0 通道必须提权** —— PawnIO 设备对象 ACL 只授权 SYSTEM 与 Administrators，非管理员只会拿到 `ring0.unavailableReason=打不开 PawnIO 设备（错误 5（拒绝访问。））`，免驱路径不受影响；②**SMN 温度是 Tctl 口径**，同负载下比 ADL2 高 10–20 °C，`cpu.temp.package` 的 detail 必须写明、不可直接比较。
>
> **仍未开始的**：**每核**温度与核心电压（通道已实现：AMD SMN 每核、SVI2 —— 但本机 SVI2 恒为 0，如实判「不可用」；Intel 侧只有单测口径、未在真机实测）、RAPL 每核功耗、Super I/O 与风扇 RPM、笔记本 EC 与厂商 WMI 链（EC 只读通道已跑通，寄存器地址仍未确定）。
>
> **M2 进度注（2026-10-15）**：M2 的**免驱可测部分已完成**（用户选择先做这一半，不等 PresentMon 到手）——PresentMon 逐帧 CSV 解析器（列按名字认、CRLF/BOM/引号/任意分片/末行无换行全部容错、表头缺失 200 行后致命）+ 帧统计（**调和平均**、1% / 0.1% Low **两条口径都算**、无魔数卡顿率、异常帧与 Dropped 分开计数、定长环形缓冲）+ 子进程采集链路（`PresentMon.exe --output_stdout`，stdout+stderr 同管道；跟随前台进程；`--v2_metrics` 失败自动去掉重试一次；3 s 无新帧判 `Stale`；**`--probe` 不启动子进程**）。单测从 43 增至 **71 passed / 0 failed**。夹具端到端（scratch 里的假 PresentMon 冒充 `PresentMon.exe`）：`--watch` 打印 `FPS 59（1% Low 35）`，逐源 `fps 0.77 ms`、工作集 13.10 MB；`--once` 汇总因此变为 **可用 13 / 本机无 20 / 待接入通道 17 / 错误 0**（10 个 `fps.*` 改由 FPS provider 认领，本机没有 PresentMon 故判「本机无」并列出找过的路径 —— **与上一条 13/10/27 口径不同，不得混用**）。
> **M2 尚未验证的**（需要真 PresentMon + 真游戏，本机两者都没有）：与 CapFrameX 同局对比 1% Low 偏差 ≤0.5 FPS、FPS 采集额外 CPU ≤1%、`--v2_metrics` 自动重试路径。设计与诚实边界见 `docs/fps-presentmon.md`，验收步骤见 `docs/build-and-run.md` §8。

---

## 9. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 内核驱动签名/封锁（Win11 22H2+ WHQL，WinRing0 被拦） | 无法读 CPU 温度/功耗/主板风扇 | 走 PawnIO（已签名、LHM 同款）；引导用户自装规避许可问题；永久保留免驱轻量模式 |
| 反作弊（Vanguard/EAC/BattlEye）拦截 Ring0 驱动 | 游戏内监控失效 | 检测到受保护进程 → 自动切 GPU-only + PresentMon FPS，绝不注入；UI 明确提示 |
| AMD 型号差异（Tctl offset、SMN 家族偏移） | 温度读数偏差 | 建立型号/家族表 + 未知型号显示 `~` 前缀；SMU PM 表作增强 |
| SuperIO 型号繁多 | 部分主板读不到风扇 | 先覆盖 Nuvoton/ITE 主流芯片；未识别显示 N/A 并上报芯片 ID 供后续支持 |
| PresentMon 未安装/版本不符 | 无帧率数据 | 三级探测：SDK DLL → 子进程 → 提示安装；封装安装向导链接 |
| 笔记本混合显卡/多 GPU | 显示错误 GPU | 枚举全部 GPU 并标注，自动选中渲染 GPU，允许手动切换 |
| 传感器返回 0/异常值 | 误导用户 | 每次读取做合理性校验（温度 −40…125 °C、功耗 0…1000 W、频率 ≤ 睿频上限），异常值打标"N/A(异常)"并记录一次诊断 |
| 读数口径与竞品不一致引发质疑 | 信任度 | 在关于页公开每项指标的读取方式与 1% Low 算法；提供 HWiNFO/CapFrameX 对照测试报告 |

---

## 10. 竞品定位

| 工具 | 强项 | 短板 | 我们的差异点 |
|---|---|---|---|
| HWiNFO64 | 传感器最全、最准 | 重、启动慢、UI 陈旧、无 FPS | 一体化 + 极轻 + 现代 UI |
| MSI Afterburner + RTSS | Overlay 强、独占全屏可用 | 注入式、组件多、超频导向 | 无注入、零反作弊风险 |
| CapFrameX | 帧率分析专业 | .NET、仅帧率、无硬件监控 | 硬件 + 帧率一体 |
| LibreHardwareMonitor | 开源库完善、常数表权威 | 无 FPS、C#/.NET、风扇与功耗依赖内核驱动、UI 简陋 | 借鉴其常数表，做原生轻量实现 |
| [FanControl](https://github.com/Rem0o/FanControl.Releases)（★21.1k） | 风扇曲线控制最流行，.NET + 服务 + UI，已迁 PawnIO | 主职是控风扇不是监控；.NET 体积；无 FPS | 零 .NET、单 EXE、带 FPS |
| [nwinfo](https://github.com/a1ive/nwinfo)（★698） | 纯 C、公共领域、MSR/SuperIO 覆盖全 | 控制台工具、无 UI/无 FPS/无历史 | 我们用它的寄存器知识做图形化产品 |
| [PerfWindow](https://github.com/lukastojiljkovic/PerfWindow) | Rust+egui 面板、架构清晰（服务拆分 + PawnIO + 按需启动） | Rust + .NET 自包含服务，体积进不了 25 MB；**完全没有 FPS** | FPS/1% Low + 更轻体积 |

---

## 11. v2 可选增强

- 帧时间曲线与 stutter 报告、会话对比与导出分享图、GPU 降频原因解读、能耗比（FPS/W）、游戏自动识别 + 每游戏配置档案、RTSS OSD 后端、插件化（Pawn 脚本自定义传感器）、多机汇总看板。
- **HWiNFO 共享内存作为兜底数据源**：只读 `Global\HWiNFO_SENS_SM2`（回退 `HWiNFO_SENS_SM2`），`OpenFileMappingA(FILE_MAP_READ)` 免提权；`HWiNFO_SENSORS_STRING_LEN = 128`。结构参考：namazso gist `namazso/0c37be5a53863954c8c8279f66cfb1cc`、[Seraksab/Hwinfo.SharedMemory.Net](https://github.com/Seraksab/Hwinfo.SharedMemory.Net)、Rust 示例 sunshine-control-panel `src-tauri/src/hwinfo.rs`。注意这是**非稳定 ABI**：必须按 signature/version/size 协商，字段对不上就放弃。测试可用伪造器 [humza-khalid/12vhpwr-guard](https://github.com/humza-khalid/12vhpwr-guard) 的 `tools/fake_hwinfo_sm2.py` 造一个假共享内存，不装 HWiNFO 也能做 CI。
- **数据驱动层**：把"PawnIO 模块清单 + Super I/O 芯片特判表 + AMD SMU 偏移表"做成可热更新的数据包，不重新编译就能支持新主板/新型号。
- **`gpu.vram.used` / `gpu.vram.usage` 的显式 opt-in（M4 再定）**：在 ADL2 路径上这两项**默认不可用**（catalog 里 `defaultVisible=false`，默认视图不受影响）。原因见 §3.2 与 `docs/amd-facts.md` §9：ADL2 的显存用量导出（`ADL2_Adapter_SharedVRAMUsage_Get` / `DedicatedVRAMUsage_Get`）**首次调用即让 perflib 计数器表常驻（+4.3 MB 私有提交）**，且实测 GPU 满载时数值**恒定**、疑似非实时用量。M4 可做成"用户显式开启 + 接受 +4.3 MB"的开关，而非默认。
- **`ADL2_Adapter_VRAMUsage_Get` 的语义待查**：很便宜（0.08 ms、无内存代价），但本机返回 **27463**、单位不明（若按 16 KB 粒度换算约 **429 MB**，接近 DXGI 报告的专用显存总量 418 MB）→ **很可能不是"实时用量"**，因此不能拿它顶替显存占用。

---

## 12. 决策点与状态

> 本节已按用户最新指令更新；细化文档见 `docs/`（索引：仓库根 `README.md`）。

| # | 决策点 | 状态 | 结论 / 影响 |
|---|---|---|---|
| 1 | 是否必须覆盖 AMD CPU | ✅ **已决：是** | M1 按「Intel + AMD 双路」估算；AMD 侧设计见 `docs/amd-cpu.md`（SMN/SMU PM 表、Tctl/Tdie、RAPL、PawnIO AMD 模块） |
| 2 | GPU 是三家都做还是先 NVIDIA | ✅ **已决：三家都做** | v1 覆盖 NVIDIA（NVML）、AMD（ADLX，旧卡回退 ADL2）、Intel（IGCL，备用 Level Zero Sysman）；三家的能力探测与真机验证都进 M1，无 Intel 真机的部分代码标 `unverified`（见本文 §9 风险表）。**现状补充（2026-10-14）**：AMD 侧当前已实现并实测可用的是 **ADL2 PMLog 免驱通道**（本机 APU、非管理员、无 PawnIO：给出 GPU 占用/频率/温度与显存用量，并承担 CPU 封装温度与整片功耗；见本文 §3.2 与 `docs/amd-facts.md` §9），ADLX 接入仍未开始 |
| 3 | PawnIO 分发方式 | ✅ **已决：随发布包附带官方安装包并静默安装**（决策 ③=B） | 发布物 = 便携 ZIP（`winperfmon.exe` + PawnIO 官方签名安装包 + GPL-2.0／LGPL-2.1 许可证文本与源码获取说明）；构建期下载并按 SHA-256 锁定；首次运行检测未装则引导用户确认后静默安装（一次 UAC，可拒绝），拒绝即落免驱模式（CPU 温度/功耗/风扇显示 N/A 并说明原因；**AMD 上封装温度与整片功耗由 ADL2 免驱提供，不受此决定影响**，见 §3.3.1 T0）；我方代码仍为 MIT（mere aggregation，合规动作见 docs/release-github.md §3.5，发布前须做法务确认） |
| 4 | 真独占全屏（FSE）下 Overlay 可见性 | ✅ **已决：硬需求 → RTSS OSD 共享内存后端为主路径**（决策 ④=B） | 自绘无注入 Overlay 在真 FSE（`Hardware: Legacy Flip`）下架构上不可见（DWM 不参与合成）；主路径改为写 `RTSSSharedMemoryV2`（我方不注入，注入由用户自装的 RTSS 完成）；无 RTSS 时回退自绘 Overlay 并明确提示；RTSS 模式下字号/颜色只能按 RTSS OSD 标签近似映射，位置/不透明度/点击穿透由 RTSS 控制（见 docs/ui-settings.md §6.1） |
| 5 | 是否需要发布给他人使用 | ✅ **已决：是（发布到 GitHub）** | 需 LICENSE / NOTICE / 隐私声明 / CI / Release 流程，见 `docs/release-github.md`；首版 portable zip + 单 EXE，不做安装器与驱动签名 |
| 6 | 运行时用户自定义设置 | ✅ **已决：要** | 排版方式（横向/纵向）、指标可见性与排序、置顶策略、文字大小与颜色；数据结构与读写接口见 `docs/config-schema.md`，交互行为见 `docs/ui-settings.md` |
| 7 | 产品名 / 仓库名 | ✅ **已决：winperfmon** | EXE = `winperfmon.exe`、配置目录 = `%APPDATA%\winperfmon\config.json`、仓库名 = `winperfmon`；GitHub 同名仓库已有 2 个（`jusu1234/WinPerfMon`、`AronZhi/WinPerfMonDll`，均 0★、无许可证、非活跃），以「用户名/winperfmon」全名区分；商标检索建议发布前补做 |
| 8 | 依赖策略（配置读写 / PresentMon 数据解析） | ✅ **已决** | 配置读写用**自研极简 JSON（零第三方依赖）**；PresentMon 数据是逐帧 CSV（官方无 JSON 输出）；`wil`（MIT）是否引入留待编码前拍板 —— 见 `config-schema.md` §0 不变量 6 |

---

## 13. GitHub 参考项目与借用清单（调研汇总）

> 调研方式：两路子代理分别深挖"传感器侧"与"帧率/Overlay 侧"，数据经 `api.github.com` / `ungh.cc` / 仓库源码实测核实（非搜索快照）。

### 13.1 传感器侧

| 项目 | ★ | 语言 | 许可 | 用途 |
|---|---|---|---|---|
| [nwinfo](https://github.com/a1ive/nwinfo) | 698 | C | **Unlicense** | **首选搬运源**：Intel/AMD MSR 全表 + Super I/O 探测 + PawnIO 调用范式 |
| [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) | 9.2k | C# | MPL-2.0 | Super I/O 芯片表/寄存器映射权威源（**文件级隔离**后移植） |
| [PawnIO](https://github.com/namazso/PawnIO) + [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) | 422 / 111 | Pawn | GPL-2.0（驱动）/ LGPL-2.1（模块） | Ring0 通道；只做运行期 IOCTL 调用 |
| [RyzenAdj](https://github.com/FlyGoat/RyzenAdj) | 2.4k | C | LGPL-3.0 | Ryzen PM 表字段偏移（只搬常数） |
| [ZenStates-Core](https://github.com/irusanov/ZenStates-Core) | 60 | C# | GPL-3.0 | 同上（只看思路） |
| [siomon](https://github.com/level1techs/siomon) | — | Rust | MIT | 交叉校验 NCT67xx / ITE87xx 常数 |
| [VirtualSMC / SMCSuperIO](https://github.com/acidanthera/VirtualSMC) | — | C++ | — | 再一份交叉校验表 |
| [nvml-wrapper](https://github.com/rust-nvml/nvml-wrapper) | — | Rust | — | `libloading` 动态加载 `nvml.dll` 的做法 |
| [FanControl](https://github.com/Rem0o/FanControl.Releases) | 21.1k | 二进制 | 闭源 | 竞品 + 已迁 PawnIO 的架构参照 |
| [PerfWindow](https://github.com/lukastojiljkovic/PerfWindow) | 0 | Rust | MIT | 服务拆分 / 按需启动 / mini strip 等工程细节 |
| ~~OpenHardwareMonitor~~ · ~~physmem_drivers~~ | — | — | — | 已停维护 / 已过时，**不用** |

### 13.2 帧率 / Overlay 侧

| 项目 | ★ | 语言 | 许可 | 用途 |
|---|---|---|---|---|
| [PresentMon](https://github.com/GameTechDev/PresentMon) | 2.6k | C++ | MIT | 帧数据底座（SDK + 服务，客户端免提权） |
| [CapFrameX](https://github.com/CXWorld/CapFrameX) | 1.3k | C# | MIT | 统计口径金矿（1%/0.1% Low、Afterburner integral、stutter、帧节奏） |
| [MangoHud](https://github.com/flightlessmango/MangoHud) | 9.1k | C | MIT | HUD 元素化布局与表驱动配置 |
| [ferrisetw](https://github.com/n4r1b/ferrisetw) | — | Rust | MIT | 日后自研 ETW 消费者的封装参考 |
| [Special K](https://github.com/SpecialKO/SpecialK) | 2.1k | C++ | GPL-3.0 | **只借思路（MPO plane 检测），代码不可并入** |
| RTSS OSD 共享内存 | — | — | 结构公开 | `RTSSSharedMemoryV2`（见 §7） |

### 13.3 许可红线（写进代码规范）

1. **可自由搬运**：nwinfo（Unlicense）、ferrisetw / nvml-wrapper / siomon（MIT）、PresentMon / CapFrameX / MangoHud（MIT，保留版权声明）。
2. **文件级隔离**：LHM 的 `IT87XX.cs` / `Nct677X.cs` / `LpcIO.cs` 常数表 → 独立文件 + 保留 MPL 声明 + 保证源码可获取。
3. **运行期调用、不链接源码**：PawnIO（GPL-2.0，驱动）与 PawnIO.Modules（LGPL-2.1，`.bin` 模块）走 IOCTL；若随包分发模块二进制则随附其源码或有效要约、并允许用户替换。
4. **只搬常数、不搬代码**：RyzenAdj（LGPL-3.0）、ZenStates-Core（GPL-3.0）。
5. **绝不并入**：Special K（GPL-3.0）。
6. 关于页统一列出所有第三方项目的名称、许可证与源码链接。

### 13.4 调研得到的关键结论（已并入正文）

- **帧率侧**：`PresentMon Service + SDK` 是唯一"官方支持 + 免提权 + 免注入"路径，**没有更轻的官方替代**（自研 ETW 必须提权，且等于重做 present 语义）；但数据链路有秒级延迟 → Overlay 只做 1 Hz 级聚合（§3.4）。
- **Overlay 侧**：非注入 Overlay 在真独占全屏（`Hardware: Legacy Flip`）**无解**（PresentMon issue #212，维护者确认）；更麻烦的是置顶窗口会把游戏从 iFlip 降到 `Composed: Flip`，抬高延迟、破坏 VRR —— Intel 自家 Overlay 就吃了这个亏（issue #235 / #367 / #689）。因此**必须内置 `PresentMode` 降级自检**并允许用户关闭自绘 Overlay（§7）。
- **传感器侧**：Ring0 只能走 PawnIO（WinRing0 已被 Defender 拦死）；**Windows 上不存在独立的 Super I/O 库可抄**，芯片特判表必须自建并随新主板长期维护（§3.3、§4）。
- **差异化**：**单 EXE ≤ 5 MB / 常驻 ≤ 25 MB / 零 .NET / 硬件 + FPS 一体** —— 目前没有任何竞品是这个组合。
