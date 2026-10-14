# Windows 极致轻量化性能监控工具 — 开发方案

> 目标产品：常驻后台的轻量硬件监控 + 游戏帧率统计工具，覆盖 **CPU/GPU 频率 · 功耗 · 温度 · 风扇转速 · FPS / 1% Low FPS**。
> 核心设计原则：**不注入、不挂钩、不自研内核驱动、单 EXE、无运行时依赖**。

---

## 1. 产品目标与硬性指标（"极致轻量化"的可验证定义）

| 维度 | 目标值 | 说明 |
|---|---|---|
| 常驻内存 | ≤ 15 MB（纯监控）/ ≤ 25 MB（含 FPS 采集） | 固定缓冲，无持续分配 |
| CPU 占用 | ≤ 0.3%（1 Hz 采样）/ ≤ 1.0%（游戏内 FPS 采集） | ETW 由 PresentMon 服务承担 |
| 冷启动 | ≤ 300 ms | 无运行时初始化 |
| 安装包/体积 | EXE ≤ 5 MB；发布物为**便携 ZIP**（EXE + PawnIO 官方安装包 + GPL-2.0／LGPL-2.1 合规文本，决策 ③=B） | `/MT` 静态 CRT，无 .NET/WebView2/Python |
| 系统支持 | Win10 1809+ / Win11（x64、ARM64 后续） | 兼容内存完整性(HVCI)开启状态 |
| 数据能力 | CPU：每核频率、每核温度、封装温度、封装/核/uncore/DRAM 功耗<br>GPU：核心/显存频率、功耗、核心/hotspot/显存温度、风扇 %<br>主板：各 FAN 转速 RPM<br>帧率：FPS、1% Low、0.1% Low、帧时间、卡顿次数、呈现模式 | 每项都需有"不可用"降级路径 |

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
│  ├ CpuProvider  Intel MSR / AMD SMN+MSR  (+PDH 降级)           │
│  ├ GpuProvider  NVML / ADLX / IGCL(含 Level Zero 备用)          │
│  ├ FanProvider  SuperIO(Nuvoton/ITE) / EC / GPU 风扇            │
│  └ FpsProvider  PresentMon Service API │ 子进程 │ 自建 ETW      │
├────────────────────────────────────────────────────────────────┤
│ 访问层  Ring0: PawnIO 模块(MSR / PortIO / SMN)                  │
│         User: PDH / WMI / NVML.dll / ADLX.dll / IGCL.dll        │
│         ETW : PresentMon 服务(SYSTEM) 或本进程 ETW 会话          │
└────────────────────────────────────────────────────────────────┘
```

**进程模型**
- **v1**：单进程 + `requireAdministrator`（Ring0 与 ETW 都需要），实现最简单。
- **v2（推荐尽早拆）**：`pmsvc`(SYSTEM 服务，独占所有 Ring0/驱动访问) + `pmui`(普通权限 UI/Overlay)，命名管道 + 共享内存 IPC。好处：UI 不需要提权（Overlay 在游戏上更安全）、驱动访问与 UI 崩溃隔离、反作弊面更小。

---

## 3. 各指标技术路线（核心表）

### 3.1 CPU

| 指标 | 首选方案（需 Ring0） | 寄存器/接口细节 | 免驱降级 |
|---|---|---|---|
| 频率（每核/平均） | MSR `IA32_MPERF(0xE7)` / `IA32_APERF(0xE8)` 差分比值 × 基频 | 有效频率 = 基频 × ΔAPERF/ΔMPERF；基频比取 `MSR_PLATFORM_INFO(0xCE)` bits[15:8] × BCLK(100 MHz)；读某核的 MSR 需 `SetThreadAffinityMask` 绑核 | PDH `\Processor Information(_Total)\% Processor Performance` × 基频，或 `\Processor Information(*)\Processor Frequency`（MHz） |
| 温度（每核/封装） | Intel：`IA32_THERM_STATUS(0x19C)`（每核，绑核读）、`IA32_PACKAGE_THERM_STATUS(0x1B1)`（封装）；TjMax 取 `MSR_TEMPERATURE_TARGET(0x1A2)` bits[23:16]，温度 = TjMax − DTS(bits[22:16]) | AMD：SMN `THM_TCON_CUR_TMP(0x00059800)` bits[31:21] × 0.125 → Tctl（注意 Tctl offset 随型号）；Tdie 需读 SMU PM 表（LHM 已实现的反向工程方案）作为增强 | WMI `MSAcpi_ThermalZoneTemperature`（多数台式机/新笔记本不支持，仅兜底） |
| 功耗（封装/核/uncore/DRAM） | RAPL MSR：`MSR_RAPL_POWER_UNIT(0x606)`（能量单位 = 1/2^ESU J，ESU=bits[12:8]；功率单位 = bits[3:0]）、`MSR_PKG_ENERGY_STATUS(0x611)`、`MSR_PP0_ENERGY_STATUS(0x639)`、`MSR_PP1(0x641)`、`MSR_DRAM_ENERGY_STATUS(0x619)`、`MSR_PLATFORM_ENERGY_STATUS(0x64D)`；**功耗墙**：`MSR_PKG_POWER_LIMIT(0x610)`（PL1 = bits[14:0]、PL2 = bits[46:32]）、`MSR_CONFIG_TDP_NOMINAL(0x614)` | 功耗 = ΔEnergy/Δt（nwinfo 口径：Δ能量(厘焦)×1000/Δms = 厘瓦，注意单位换算）；32 位计数器按 2^32 处理回绕；Intel 与 AMD Zen 均支持 RAPL（**AMD 是 `0xC0010299` 单位 + `0xC001029B` 能量**，fam ≥ 17h） | 无可靠方案（Windows "Power Meter" 计数器仅部分机型存在） |
| 频率/倍频（补充寄存器） | `MSR_PERF_STATUS(0x198)` 倍频 bits[15:8]、`MSR_TURBO_RATIO_LIMIT(0x1AD)` 最大倍频、`MSR_FSB_FREQ(0xCD)`（BSEL 表：266.67/133.33/200/166.67/333.33/100/400 MHz） | AMD：`0xC0010063` 当前 PState、`0xC0010064–0xC001006B` = PState0–7；17h–19h 倍频 = fid/did×2（[13:8]=did、[7:0]=fid），Zen5(1Ah) = fid×0.05 | — |
| 功耗（AMD 增强） | SMU PM 表（PPT/TDC/EDC、STAPM、TDP、SVI2 电压电流），更接近 Ryzen Master 读数 | 家族相关（17h/19h/1Ah 索引不同），需型号表；**Tctl 是控制温度不是结温**，温度 = `0.001 × ((reg>>21) × 125) + SMU 偏移`，偏移随型号/AGESA 变化（5800X3D/7800X3D 上 Tctl 与 CCD 差很多）→ **禁止硬编码偏移** | — |

> 注意 1：RAPL 需 BIOS 开启（部分 OEM 关闭）；读不到时标记为 N/A 而非显示 0。
> 注意 2：社区有 **RAPL 功耗读数翻倍**的案例（System Informer issue #2001）——用 HWiNFO 或墙插功耗计做一次绝对值校验，异常时降级标注而不是照抄。
> 注意 3：本节寄存器地址/位域建议直接以 **nwinfo**（Unlicense＝公共领域）`libnw/cpu/intel_cpu.c`、`amd_cpu.c` 为准，它还附带 PawnIO 调用范式与驱动回退链（见 §4）。

### 3.2 GPU

| 厂商 | 接口 | 可用指标 |
|---|---|---|
| NVIDIA | **NVML**（`nvml.dll` 随驱动，无需管理员） | `nvmlDeviceGetClockInfo(SM/MEM)`、`nvmlDeviceGetPowerUsage`(mW)、`nvmlDeviceGetPowerManagementLimit`、`nvmlDeviceGetTemperature(GPU)`、`nvmlDeviceGetFieldValues(NVML_FI_DEV_MEMORY_TEMP …)` 取 hotspot/显存、`nvmlDeviceGetFanSpeed`(%)、`nvmlDeviceGetCurrentClocksThrottleReasons`(功耗/热/PM 限制降频原因)、显存占用 |
| AMD | **ADLX**（随驱动安装，`IADLXGPUMetrics`；先用 `IADLXGPUMetricsSupport` 探测支持项） | GPU 使用率、GFX/显存频率、温度（edge/hotspot/VRAM）、风扇转速、功耗、电压、显存占用。旧卡降级 **ADL2** `ADL2_New_QueryPMLogData_Get`（`ADL_PMLOG_*`） |
| Intel (Arc/Iris Xe) | **IGCL**：`ctlPowerTelemetryGet`（功耗/频率/温度）、`ctlEnumTemperatureSensors`、`ctlFanGetSpeed`/`ctlFanGetConfig`、`ctlFrequencyGetState`；备用 **Level Zero Sysman**（`zesPowerGetEnergyCounter` 等） | 功耗、频率、温度、风扇 RPM、显存 |

- 多 GPU/笔记本混合输出：枚举全部设备并标注 iGPU/dGPU，默认选中"当前正在渲染的 GPU"（优先用 PresentMon 报告的 GPU，其次用使用率最高者）。
- NVIDIA 一般不提供风扇 RPM（仅 %），AMD/Intel 可给 RPM。

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
- 笔记本：多数机型风扇走 ACPI EC（端口 `0x62/0x66`），寄存器映射因 OEM 而异 → 优先级最低，先只支持常见游戏本映射。
- 常数来源与许可：**nwinfo（Unlicense）可直接搬**；**LibreHardwareMonitor（MPL-2.0）** `LibreHardwareMonitorLib/Hardware/Motherboard/Lpc/*.cs` 是文件级 copyleft——移植的文件须保留 MPL 声明并保证源码可获取；纯常数可自由使用，但别照抄文件结构。交叉校验可用 `siomon`（Rust/MIT）、`VirtualSMC/SMCSuperIO`（C++）。
- 不可读时显示 `N/A`，不要伪造。
- **现实提醒**：Windows 上**不存在独立的 Super I/O 库**（只有 Linux `it87`/`nct6775` 与 macOS 实现；LHM 也没有官方 C++/Rust 移植，现存全是"包一层 .NET"的桥）。这块**没有整套现成代码可抄**，必须自建并随新主板持续维护——本项目最大的工程风险。

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

---

## 4. Ring0 访问与驱动方案（本项目的最大风险项）

**现状（决定了不能踩的坑）**

- **自研驱动不可行**：Win11 22H2 起，新的内核模式驱动必须 **WHQL 签名**（证明签名/attestation 不再适用于 22H2 及以后版本），个人/小团队走不通。
- **绝对不要用 WinRing0**：已被微软列入易受攻击驱动阻止列表（CVE-2020-14979），Defender 直接报 `HackTool:Win32/Winring0` / `VulnerableDriver:WinNT/Winring0`；LHM issue #1660、#1873，FanControl issue #3016（原话：R0FanControl 服务因文件含病毒无法启动）都是实例。LHM 由 namazso 亲自提 PR #1857，于 **v0.9.5（2025-09-16）彻底换成 PawnIO**，FanControl V238 同步 —— 这条路已经死了。
- **推荐 PawnIO**：namazso 的"可脚本化通用内核驱动"（https://pawnio.eu/ ，已签名、专为硬件访问设计、兼容内存完整性）。接入方式：`\\.\PawnIO` 打开设备 → 加载模块 → IOCTL 执行读写。**我们只写用户态调用代码，不碰内核编译与签名。**
  - 需要的模块（来自 `PawnIO.Modules`）：`IntelMSR.p`（MSR 读写）、`AMDFamily0F/10/17.p`（按 CPU family 选）、`RyzenSMU.p`（PM 表）、`LpcIO.p`（端口 I/O / Super I/O）。
  - 调用范式可直接照 nwinfo：`WR0_ExecPawn(h, &h->pio_intel, "ioctl_read_msr", &in, 1, &out, 1, NULL)`，并配 `--driver=CPUZ162|NwHwIo|PawnIO|NONE` 式回退链（找不到驱动 → 自动退免驱模式）。
  - 用 PawnIO 仍需**安装驱动 + 模块**，Windows 会弹"安装驱动"类安全提示 → 必须像 LHM PR #1777 那样提供**"禁用 Ring0 驱动安装"开关**，让用户能选纯免驱模式。
- **许可处理（决策 ③=B 已定）**：`PawnIO`（驱动／安装包）为 **GPL-2.0**，`PawnIO.Modules`（`.bin` 模块）为 **LGPL-2.1**（GitHub REST API `license.spdx_id = LGPL-2.1`；仓库 `COPYING` 记 LGPL-2.1-or-later）。我们**不链接、不复制其源码**，只在运行期经 IOCTL 调用；**官方签名安装包随发布包附带**，首次运行检测未装则**引导用户确认后静默安装**（可拒绝 → 免驱模式），并随包提供其许可证原文与**对应源码获取说明**（驱动 GPL-2.0、模块 LGPL-2.1；若随包提供模块还须保证模块可被用户替换；发布前须做法务确认）。**不**拆发驱动 `.bin`/`.sys`，也**不**自研安装器与驱动签名。合规动作逐条见 `docs/release-github.md` §3.5。
- **免驱动"轻量模式"（必须有）**：仅 PDH CPU 频率 + NVML/ADLX/IGCL GPU 全指标；CPU 温度/功耗/主板风扇显示 `N/A` 并给出明确原因提示。这条路径让工具在 Vanguard 等强反作弊环境下依然可用。

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
| **M0 骨架 + 免驱指标** | 2–3 天 | CMake 工程、Win32+Direct2D 窗口/托盘、采集调度器、PDH CPU 频率、NVML 全套 GPU 指标、JSON 配置、CSV 日志 | 双显卡机器上数值与 HWiNFO 一致（±1 °C / ±50 MHz / ±5% 功耗）；内存 ≤15 MB |
| **M1 Ring0 传感器** | 3–5 天 | PawnIO 集成、MSR 模块（APERF/MPERF、THERM_STATUS、RAPL）、AMD SMN 温度、SuperIO 探测 + 风扇 RPM、豁免降级逻辑 | Intel 12–15 代 + AMD Zen3–5 上 CPU 温度/功耗/风扇全部可用；HVCI 开启不报错 |
| **M2 FPS 引擎与统计** | 3–5 天 | PresentMon Service 客户端（含子进程/自建 ETW 两条备选路径探测）、帧环形缓冲、会话统计、1%/0.1% Low、stutter 计数、CSV 导出 | 与 CapFrameX 同局对比 1% Low 偏差 ≤0.5 FPS；FPS 采集额外 CPU ≤1% |
| **M3 Overlay 与交互** | 4–7 天 | **RTSS OSD 后端（主路径）**、自绘无注入 Overlay（回退）、布局配置、热键、**PresentMode 降级自检**、RTSS OSD 后端、深色主题、异常提示 UI | 无边框游戏中稳定显示 4 h 无闪烁/掉帧；FSE 下正确提示"需 RTSS 后端"；检测到 `Independent Flip → Composed: Flip` 时能提示并一键关闭 |
| **M4 打包/兼容/文档** | 3–6 天 | 单 EXE 打包、PawnIO 检测与引导安装、兼容矩阵测试、崩溃转储、README/口径说明 | 干净系统免安装运行；降级路径全部走通并给出准确原因 |

合计约 **4–7 周**（单人，含联调；已含决策 ③=B/④=B 带来的 M3 +1–2 天、M4 +1–2 天）。

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

---

## 12. 决策点与状态

> 本节已按用户最新指令更新；细化文档见 `docs/`（索引：仓库根 `README.md`）。

| # | 决策点 | 状态 | 结论 / 影响 |
|---|---|---|---|
| 1 | 是否必须覆盖 AMD CPU | ✅ **已决：是** | M1 按「Intel + AMD 双路」估算；AMD 侧设计见 `docs/amd-cpu.md`（SMN/SMU PM 表、Tctl/Tdie、RAPL、PawnIO AMD 模块） |
| 2 | GPU 是三家都做还是先 NVIDIA | ✅ **已决：三家都做** | v1 覆盖 NVIDIA（NVML）、AMD（ADLX，旧卡回退 ADL2）、Intel（IGCL，备用 Level Zero Sysman）；三家的能力探测与真机验证都进 M1，无 Intel 真机的部分代码标 `unverified`（见本文 §9 风险表） |
| 3 | PawnIO 分发方式 | ✅ **已决：随发布包附带官方安装包并静默安装**（决策 ③=B） | 发布物 = 便携 ZIP（`winperfmon.exe` + PawnIO 官方签名安装包 + GPL-2.0／LGPL-2.1 许可证文本与源码获取说明）；构建期下载并按 SHA-256 锁定；首次运行检测未装则引导用户确认后静默安装（一次 UAC，可拒绝），拒绝即落免驱模式（CPU 温度/功耗/风扇显示 N/A 并说明原因）；我方代码仍为 MIT（mere aggregation，合规动作见 docs/release-github.md §3.5，发布前须做法务确认） |
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
