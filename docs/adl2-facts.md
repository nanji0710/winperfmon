# AMD GPU 遥测（ADL2 / PMLog）：传感器索引与技术事实核实记录

> ⚠️ **本文件的"支持性判定"部分已被真机复核推翻，读第 10/13/14 节前必读本节。**
>
> ## 0. 更正公告（2026-10-14 晚，真机复核；权威结论见 `docs/amd-facts.md` §9）
>
> 本文原稿把「本次采样值非零」当成了「驱动支持该传感器」。`ADL2_New_QueryPMLogData_Get`
> 返回的 `ADLSingleSensorData` 有**两个**字段：`int supported`（能力）与 `int value`（本次值）。
> 原稿 §10 的样本表是按**值非零**筛选的，于是**那些空闲态天然为 0 的传感器被误判成"不支持"**——
> 最典型的就是 GPU 占用率。以下四条立即生效：
>
> | # | 原稿位置与表述 | 更正 | 本次证据 |
> |---|---|---|---|
> | 0.1 | §10 末段、§13 行 3、§14 项 4：本机 `19 INFO_ACTIVITY_GFX`（以及 8/14/15/20/27）不支持 | **错误**。`s19.supported != 0`，空闲值 0、GPU 满载 0→98/99%、纯 8 线程 CPU 满载时保持 0——**它是本机唯一干净的 GPU 占用信号**。原稿 §10 段末给出的替代路径 `ADL2_OverdriveN_PerformanceStatus_Get.iGPUActivityPercent` **不再需要**（留作 M4 备选）。14/15/27/8/20 的真实支持性仍未确证，不得按原稿断言"不支持" | `--probe` 的 `[gpu.N]` 段；`adl2watch.exe` / `adl2cs.exe` 时序探针；`gpuload.exe`（D3D11 计算着色器满载）与 `cpuload.exe`（8 线程）对照 |
> | 0.2 | §13 行 9、§14 项 9：支持能力的权威来源是 `ADL2_Adapter_PMLog_Support_Get` 的 `usSensors[256]`（或 `ADL2_Overdrive8_PMLogSenorType_Support_Get`） | **本机该导出不可用**。按 `int[256]` 形状传参虽得 `rc=0`，但 `sup[1..9]` 全部非零，与真实支持集（20 项）互相矛盾；故**支持性一律以 `sensors[i].supported != 0` 为准**（与 LHM 的做法一致）。旁证：本机 `C:\Windows\System32\atiadlxx.dll` 能搜到 `ADL2_Adapter_PMLog_Support_Get` / `ADL2_New_QueryPMLogData_Get` / `ADL2_Adapter_PMLog_Start`，**搜不到** `ADL2_New_QueryPMLogSupport_Get` 这个导出名 | 独立探针 `adl2probe2.exe`；`atiadlxx.dll` 全文 ASCII 导出名扫描 |
> | 0.3 | §13 行 5：`ADL2_New_QueryPMLogData_Get` 已被 AMD 正式弃用 | 「弃用」属实但不影响使用：本机非管理员、无需显卡正在渲染即取数成功（`rc=0`）。M1 **有意**继续用它（共享内存路径 `ADL2_Overdrive8_PMLog_ShareMemory_*` 需额外的映射与采样率协商，收益不抵复杂度）。日后若要降 CPU 开销再评估 | 本机实测 |
> | 0.4 | §10 的空闲样本（`23=25 W`、`30=18 W`、`34=3931`） | 这是**另一个时刻、另一负载状态**的采样，与后续批次（`23=7–8 W`、`30=1 W`、`34=3300–3850`）不可混用比较。跨批次只能比"同一时刻的同一字段" | 多批次 CSV 时序探针 |
>
> 其余结论（索引 0–45 的名称映射、`supported` 是布尔非位图、缩放系数、结构体布局、
> `iVendorID = 0x03EA` 的成因、5 个重复适配器的去重、VRAM 不在 PMLog 里）**本次复核未发现冲突，仍然有效**。

> 本文件是 ADL2 `PMLog` 传感器索引表的核实记录，日期 **2026-10-14**。规则：**每条结论必须带证据；证据形式＝仓库 + 文件路径 + 行号，或官方文档 URL + 章节**；查不到可靠来源的一律写「未找到可靠来源」并给探测式实现建议，不填猜测值。

核实手段：`raw.githubusercontent.com` 取 ADL SDK 头文件与各消费方源码全文（本地镜像在 `build/_adlres/`，**非交付物**）、`grep.app` API、AMD 官方公开 Doxygen 文档站 <https://gpuopen-librariesandsdks.github.io/adl/>（自称 *ADL (AMD Display Library) Public APIs Version 18.1*，页脚 `Copyright © 2009-2024 Advanced Micro Devices, Inc.`）、本机 `atiadlxx.dll` 与 WMI 实测。

被核实的 SDK 头文件副本（三份，互相独立）：

① `menloresearch/cortex.cpp:dev:engine/utils/hardware/gpu/adl/adl_defines.h`
② `Pulse-Eight/libcec:master:src/libcec/platform/adl/adl_defines.h`
③ `GameTechDev/PresentMon:main:IntelPresentMon/ControlLib/adl/adl_defines.h`

官方结构体头文件：`GPUOpen-LibrariesAndSDKs/display-library:include/adl_structures.h`（本机副本 `build/_adlres/sdk_adl_structures.h`，其中 `ADL_SDK_MAJOR_VERSION 17` / `ADL_SDK_MINOR_VERSION 1`）。

**路径缩写对照**（下文出现的简写一律展开为下表左侧全路径，可被 grep）：

| 下文简写 | 完整引用（仓库 : 分支 : 路径） |
|---|---|
| `AmdGpu.cs` | `LibreHardwareMonitor/LibreHardwareMonitor:master:LibreHardwareMonitorLib/Hardware/Gpu/AmdGpu.cs` |
| `AtiAdlxx.cs` | `LibreHardwareMonitor/LibreHardwareMonitor:master:LibreHardwareMonitorLib/Interop/AtiAdlxx.cs` |
| `AmdGpuGroup.cs` | `LibreHardwareMonitor/LibreHardwareMonitor:master:LibreHardwareMonitorLib/Hardware/Gpu/AmdGpuGroup.cs` |
| `adl_defines_cortex.h` | `menloresearch/cortex.cpp:dev:engine/utils/hardware/gpu/adl/adl_defines.h` |
| `adl_defines_libcec.h` | `Pulse-Eight/libcec:master:src/libcec/platform/adl/adl_defines.h` |
| `PresentMon_adl_defines.h` | `GameTechDev/PresentMon:main:IntelPresentMon/ControlLib/adl/adl_defines.h` |
| `sdk_adl_structures.h` | `GPUOpen-LibrariesAndSDKs/display-library:include/adl_structures.h` |
| `nwinfo_adl.h` | `a1ive/nwinfo:master:libnw/gpu/adl.h` |
| `nwinfo_amd_gpu.c` | `a1ive/nwinfo:master:libnw/gpu/amd_gpu.c` |
| `ff_gpu_amd.c` | `fastfetch-cli/fastfetch:dev:src/detection/gpu/gpu_amd.c` |
| `ghelper_AmdAdl2.cs` | `seerge/g-helper:main:app/Gpu/AMD/AmdAdl2.cs` |

---

## 1. 索引 0–45：跨 SDK 版本稳定区

**结论**：`ADL_PMLOG_SENSORS` 枚举的 0–45 号在所有已核实的头文件副本中**逐项一致**（名称与数值）。单位在旧枚举 `ADLSensorType` 中以行尾注释给出，新枚举 `ADL_PMLOG_SENSORS` 只保留数值——**两者索引完全相同**，因此旧注释可原样用于新枚举。

| 索引 | 名称（`ADL_PMLOG_SENSORS`） | 含义 / 单位 | 本机 780M 实测 | 备注 |
|---|---|---|---|---|
| 0 | `ADL_SENSOR_MAXTYPES` | **终止标记，不是传感器** | 未出现 | 支持列表与 `ulValues` 都用它作结束标志；判断支持度时必须先排除 |
| 1 | `ADL_PMLOG_CLK_GFXCLK` | 图形核心时钟，**MHz** | **800** | 头文件有单位注释 |
| 2 | `ADL_PMLOG_CLK_MEMCLK` | 显存时钟，**MHz** | **2800** | 头文件有单位注释 |
| 3 | `ADL_PMLOG_CLK_SOCCLK` | SoC 时钟，MHz（按命名推断） | **400** | 头文件无单位注释 |
| 4 | `ADL_PMLOG_CLK_UVDCLK1` | UVD 时钟 1，MHz（推断） | 未出现 | |
| 5 | `ADL_PMLOG_CLK_UVDCLK2` | UVD 时钟 2，MHz（推断） | 未出现 | |
| 6 | `ADL_PMLOG_CLK_VCECLK` | VCE 时钟，MHz（推断） | 未出现 | |
| 7 | `ADL_PMLOG_CLK_VCNCLK` | VCN 时钟，MHz（推断） | **800** | 本机与 GFXCLK 同值 |
| 8 | `ADL_PMLOG_TEMPERATURE_EDGE` | 芯片边缘温度，**°C** | **未支持（0）** | 集显上可能整体缺失 |
| 9 | `ADL_PMLOG_TEMPERATURE_MEM` | 显存温度，°C | 未出现 | |
| 10 | `ADL_PMLOG_TEMPERATURE_VRVDDC` | 供电 VR VDDC 温度，°C | 未出现 | |
| 11 | `ADL_PMLOG_TEMPERATURE_VRMVDD` | 供电 VR MVDD 温度，°C | 未出现 | |
| 12 | `ADL_PMLOG_TEMPERATURE_LIQUID` | 液冷温度，°C | 未出现 | |
| 13 | `ADL_PMLOG_TEMPERATURE_PLX` | PLX 桥温度，°C | 未出现 | |
| 14 | `ADL_PMLOG_FAN_RPM` | 风扇转速，**RPM** | **未支持（0）** | 笔记本风扇走 EC，不经 ADL |
| 15 | `ADL_PMLOG_FAN_PERCENTAGE` | 风扇占空比，**%** | 未出现 | |
| 16 | `ADL_PMLOG_SOC_VOLTAGE` | SoC 电压，**mV** | **765** | 需换算见 §4 |
| 17 | `ADL_PMLOG_SOC_POWER` | SoC 功耗，**W** | **1** | |
| 18 | `ADL_PMLOG_SOC_CURRENT` | SoC 电流，**A** | **2** | |
| 19 | `ADL_PMLOG_INFO_ACTIVITY_GFX` | 图形活动率，**%** | **未支持（0）** | 集显上不可用，见 §10 |
| 20 | `ADL_PMLOG_INFO_ACTIVITY_MEM` | 显存活动率，**%** | 未出现 | |
| 21 | `ADL_PMLOG_GFX_VOLTAGE` | 图形核心电压，**mV** | **1235** | 需换算见 §4 |
| 22 | `ADL_PMLOG_MEM_VOLTAGE` | 显存电压，mV | 未出现 | |
| 23 | `ADL_PMLOG_ASIC_POWER` | ASIC 功耗，**W** | **25** | 头文件有单位注释 |
| 24 | `ADL_PMLOG_TEMPERATURE_VRSOC` | 供电 VR SoC 温度，°C | 未出现 | |
| 25 | `ADL_PMLOG_TEMPERATURE_VRMVDD0` | 供电 VR MVDD0 温度，°C | 未出现 | |
| 26 | `ADL_PMLOG_TEMPERATURE_VRMVDD1` | 供电 VR MVDD1 温度，°C | 未出现 | |
| 27 | `ADL_PMLOG_TEMPERATURE_HOTSPOT` | 芯片热点温度，**°C** | **未支持（0）** | 集显上缺失，只能退到 28/29 |
| 28 | `ADL_PMLOG_TEMPERATURE_GFX` | GFX 域温度，°C | **52** | EDGE 缺失时的替代 |
| 29 | `ADL_PMLOG_TEMPERATURE_SOC` | SoC 域温度，°C | **53** | |
| 30 | `ADL_PMLOG_GFX_POWER` | GFX 域功耗，W | **18 / 16 / 15** | |
| 31 | `ADL_PMLOG_GFX_CURRENT` | GFX 域电流，A | **14 / 13 / 12** | |
| 32 | `ADL_PMLOG_TEMPERATURE_CPU` | CPU 温度，°C | **62 / 61** | APU 特有：独显上无意义 |
| 33 | `ADL_PMLOG_CPU_POWER` | CPU 功耗，W | **3 / 2** | APU 特有 |
| 34 | `ADL_PMLOG_CLK_CPUCLK` | CPU 时钟，MHz | **3931 / 4046** | APU 特有 |
| 35 | `ADL_PMLOG_THROTTLER_STATUS` | **位图**：GPU 降频状态 | 未出现 | 新头注释仅 `// GFX`；语义见 §3 |
| 36 | `ADL_PMLOG_CLK_VCN1CLK1` | VCN1 时钟 1，MHz（推断） | 未出现 | |
| 37 | `ADL_PMLOG_CLK_VCN1CLK2` | VCN1 时钟 2，MHz（推断） | 未出现 | |
| 38 | `ADL_PMLOG_SMART_POWERSHIFT_CPU` | CPU SmartShift 状态/档位 | 未出现 | 语义未找到可靠来源 |
| 39 | `ADL_PMLOG_SMART_POWERSHIFT_DGPU` | dGPU SmartShift 状态/档位 | 未出现 | 语义未找到可靠来源 |
| 40 | `ADL_PMLOG_BUS_SPEED` | PCIe 当前速率（代数） | 未出现 | 头文件注释 `Current PCIE bus speed running` |
| 41 | `ADL_PMLOG_BUS_LANES` | PCIe 当前链路宽度 | 未出现 | 集显无 PCIe 上行，故缺失 |
| 42 | `ADL_PMLOG_TEMPERATURE_LIQUID0` | 液冷温度 0，°C | 未出现 | |
| 43 | `ADL_PMLOG_TEMPERATURE_LIQUID1` | 液冷温度 1，°C | 未出现 | |
| 44 | `ADL_PMLOG_CLK_FCLK` | Infinity Fabric 时钟，MHz（推断） | 未出现 | |
| 45 | `ADL_PMLOG_THROTTLER_STATUS_CPU` | CPU 降频状态位图（推断） | 未出现 | |

**证据**：枚举与数值 ——
① `adl_defines_cortex.h:2019-2064`（`ADL_PMLOG_SENSORS`，`ADL_PMLOG_CLK_GFXCLK = 1` … `ADL_PMLOG_THROTTLER_STATUS_CPU = 45`）；
② `Pulse-Eight/libcec` 同文件副本 `:2053-2100`，逐项同名同值；
③ `PresentMon_adl_defines.h:1959-2006`，逐项同名同值（**该副本止于 45**，`:2006` 即 `ADL_PMLOG_THROTTLER_STATUS_CPU = 45`）。
单位注释（旧枚举携带，索引完全对齐）——
① `adl_defines_cortex.h:1932-1933`（GFXCLK `// Current graphic clock value in MHz`、MEMCLK `// Current memory clock value in MHz`）、`:1939`（EDGE `// Current edge of the die temperature value in C`）、`:1945-1946`（FAN_RPM `// Current fan RPM value`、FAN_PERCENTAGE `// Current ratio of fan RPM and max RPM`）、`:1950-1951`（ACTIVITY_GFX `// Current graphic activity level in percentage`、ACTIVITY_MEM）、`:1952`（GFX_VOLTAGE `// Current graphic voltage in mV`）、`:1954`（ASIC_POWER `// Current ASIC power draw in Watt`）、`:1958`（HOTSPOT `// Current center of the die temperature value in C`）、`:1971-1972`（BUS_SPEED `// Current PCIE bus speed running`、BUS_LANES `// Current PCIE bus lanes using`）；
② 同一组注释在 `PresentMon_adl_defines.h:1902-1942` 与 `seerge/g-helper:main:app/Gpu/AMD/AmdAdl2.cs:78-151` 中逐字重复（g-helper 保留的是原始注释文本，可直接引用）。
第三方枚举副本：`LibreHardwareMonitor/LibreHardwareMonitor:LibreHardwareMonitorLib/Interop/AtiAdlxx.cs:517-594`、`a1ive/nwinfo:master:libnw/gpu/adl.h:446-524`。

**置信度**：索引 ↔ 名称：**已证实**（三份独立头文件 + 两份第三方副本一致）；有注释的单位（1/2/8/14/15/19/20/21/23/27/40/41）：**已证实**；无注释字段的单位（按命名与 1/2/3 类比推断为 MHz，温度类比为 °C，功耗为 W，电流为 A）：**推断**。

**对设计的影响**：0–45 可以直接硬编码为常量表，无需运行期探测名称。但**支持与否必须运行期探测**（见 §9、§12）——本机 780M 只支持 16 个索引。

---

## 2. 索引 46–74：SDK 版本相关区（**不要硬编码 52 以上**）

**结论**：索引 **46–51 与 73** 在所有副本中一致；**52–72 与 74 在不同 SDK 版本中含义完全不同**，出现了两套互不兼容的命名。本机驱动版本 `7.26.10.1609` 走哪一套需运行期判定（见 §12）。

| 索引 | 现代版（cortex.cpp 副本） | 旧版（libcec / nwinfo 副本） | 说明 |
|---|---|---|---|
| 46 | `ADL_PMLOG_SSPAIRED_ASICPOWER` | 同 | 头文件行尾注释 `// apuPower` |
| 47 | `ADL_PMLOG_SSTOTAL_POWERLIMIT` | 同 | `// Total Power limit` |
| 48 | `ADL_PMLOG_SSAPU_POWERLIMIT` | 同 | `// APU Power limit` |
| 49 | `ADL_PMLOG_SSDGPU_POWERLIMIT` | 同 | `// DGPU Power limit` |
| 50 | `ADL_PMLOG_TEMPERATURE_HOTSPOT_GCD` | 同 | GCD（图形芯粒）热点温度 |
| 51 | `ADL_PMLOG_TEMPERATURE_HOTSPOT_MCD` | 同 | MCD（显存芯粒）热点温度 |
| 52 | `ADL_PMLOG_THROTTLER_TEMP_EDGE_PERCENTAGE` | `ADL_PMLOG_THROTTLE_PERCENTAGE_TEMP_GFX` | **冲突** |
| 53 | `…_TEMP_HOTSPOT_PERCENTAGE` | `…_TEMP_MEM` | **冲突** |
| 54 | `…_TEMP_HOTSPOT_GCD_PERCENTAGE` | `…_TEMP_VR` | **冲突** |
| 55 | `…_TEMP_HOTSPOT_MCD_PERCENTAGE` | `…_POWER` | **冲突** |
| 56 | `…_TEMP_MEM_PERCENTAGE` | `…_TDC` | **冲突** |
| 57 | `…_TEMP_VR_GFX_PERCENTAGE` | `…_VMAX` | **冲突** |
| 58 | `…_TEMP_VR_MEM0_PERCENTAGE` | `ADL_PMLOG_BUS_CURR_MAX_SPEED` | **冲突** |
| 59–70 | `…_TEMP_VR_MEM1/_TEMP_VR_SOC/_TEMP_LIQUID0/_TEMP_LIQUID1/_TEMP_PLX/_TDC_GFX/_TDC_SOC/_TDC_USR/_PPT0/_PPT1/_PPT2/_PPT3_PERCENTAGE` | `ADL_PMLOG_RESERVED_1` … `RESERVED_12`（注释 `//Currently Unused`） | **冲突** |
| 71 | `ADL_PMLOG_THROTTLER_FIT_PERCENTAGE` | `ADL_PMLOG_CLK_NPUCLK`（`//NPU frequency`） | **冲突** |
| 72 | `ADL_PMLOG_THROTTLER_GFX_APCC_PLUS_PERCENTAGE` | `ADL_PMLOG_NPU_BUSY_AVG`（`//NPU busy`） | **冲突** |
| 73 | `ADL_PMLOG_BOARD_POWER` | 同 | **一致**（两个版本都是整板功耗） |
| 74 | —（下一项即 `ADL_PMLOG_MAX_SENSORS_REAL`） | `ADL_PMLOG_TEMPERATURE_INTAKE` | 版本相关 |
| — | `ADL_PMLOG_MAX_SENSORS_REAL` = 74 | `ADL_PMLOG_MAX_SENSORS_REAL` = 75 | **该常量不是固定值** |

**证据**：现代版 `adl_defines_cortex.h:2065-2094`（`:2071-2092` 为 `THROTTLER_*_PERCENTAGE` 52…72，`:2092` `ADL_PMLOG_BOARD_POWER = 73`；旧枚举同构，见 `:1977-2004`）；旧版 `adl_defines_libcec.h:2101-2129` 与 `a1ive/nwinfo:master:libnw/gpu/adl.h:494-522`（`:2101`/`nwinfo_adl.h:494` `ADL_PMLOG_SSPAIRED_ASICPOWER = 46, // apuPower`；`:2107-2112` 六项 `ADL_PMLOG_THROTTLE_PERCENTAGE_*`；`:2113` `ADL_PMLOG_BUS_CURR_MAX_SPEED = 58`；`:2114-2125` `ADL_PMLOG_RESERVED_1 = 59` … `ADL_PMLOG_RESERVED_12 = 70`，每行尾注释 `//Currently Unused`；`nwinfo_adl.h:519` `ADL_PMLOG_CLK_NPUCLK = 71,      //NPU frequency`、`:520` `ADL_PMLOG_NPU_BUSY_AVG = 72,    //NPU busy`、`:521` `ADL_PMLOG_BOARD_POWER = 73`、`:522` `ADL_PMLOG_TEMPERATURE_INTAKE = 74`）。第三方副本 `AtiAdlxx.cs:565-593` 采用现代版（`:592` `ADL_PMLOG_BOARD_POWER = 73`，`:593` `ADL_PMLOG_MAX_SENSORS_REAL`）。
**索引稳定性依据**：`hashcat/hashcat:master:include/ext_ADL.h` 顶部注释（pin 到 2021-01 的 `display-library` 提交 `209538e1`）逐字写明：`Every one of the 18 structures below was compared field by field against upstream master and all 18 are unchanged, ADL_PMLOG_MAX_SENSORS is still 256, and every sensor named in ADLSensorType still has the index it had. AMD appended 32 sensor types after the ones here rather than renumbering, which moved only the terminator, and nothing reads that.` ——**AMD 是追加而非重编号**，这正是 0–51/73 可硬编码的原因；但追加进去的 52–72 在两套 vintage 里语义不同，所以**新追加区不能用名字解释**（若驱动报 52=37，按现代版就是「边缘温度降频百分比」，按旧版则是「GFX 温度降频百分比」）。
**置信度**：46–51、73 跨版本一致：**已证实**；52–72、74 版本相关：**已证实**；「本机是哪一套」：**未找到可靠来源**（无公开资料给出 `atiadlxx.dll` 版本 ↔ SDK vintage 对照表）。

**对设计的影响**：只消费 1/2/3/8/14/15/16/17/18/19/21/23/28/29/30/31/32/33/34/40/41/73；**不要**消费 52–72 与 74，或至少在 UI 上标注「含义随驱动版本变化」。

---

## 3. `supported` 字段是**布尔**，不是位图

| 项 | 内容 |
|---|---|
| 结论 | `ADLSingleSensorData.supported` 的用法是**布尔可用性标志**（非 0 ＝ 本次采样该传感器有有效值），**不是位图**，也**不是**「支持能力位掩码」。三个互相独立的开源消费方都只做「非 0 / 真值」判断，**没有一处对它做 AND 或取特定位**。任务书里「某些项目会 AND 它或测试特定位」的说法**未获证实**。真正需要按位解释的字段是另一个：**索引 35 `ADL_PMLOG_THROTTLER_STATUS`**，其头文件注释明说是位图。另有**权威的「支持能力」通道**，与 `supported` 无关：`ADL2_Adapter_PMLog_Support_Get` 返回 `ADLPMLogSupportInfo.usSensors[256]` —— 一个**以 `ADL_SENSOR_MAXTYPES(0)` 结尾的传感器 ID 列表**，消费方用线性扫描判断。 |
| 证据 | ① 头文件结构体：`GPUOpen-LibrariesAndSDKs/display-library:include/adl_structures.h:3209-3215`（`typedef struct ADLSingleSensorData { int supported; int value; } ADLSingleSensorData;`）；副本 `build/_adlres/AtiAdlxx.cs:501-506`、`build/_adlres/nwinfo_adl.h:394-398`、`build/_adlres/ghelper_AmdAdl2.cs:36-38`（`public int Supported; public int Value;`）。② **AMD 官方文档没有定义该字段语义**：<https://gpuopen-librariesandsdks.github.io/adl/structADLSingleSensorData.html> 的 Detailed Description 只有 `Structure containing information about Performance Metrics data. This structure is used to store information about Performance Metrics data output`，全文无 boolean/bitmask 措辞（一手否定证据）。③ 消费方「布尔」用法的三处：`LibreHardwareMonitor/LibreHardwareMonitor:LibreHardwareMonitorLib/Hardware/Gpu/AmdGpu.cs:476` `bool supportedByOD8 = _overdrive8LogExists && i < od8Log.sensors.Length && od8Log.sensors[i].supported != 0;`；`a1ive/nwinfo:master:libnw/gpu/amd_gpu.c:84-85` `bool support_od8 = gpu->Od8LogExists && … && od8_log->sensors[sensor_type].supported;`；`fastfetch-cli/fastfetch:dev:src/detection/gpu/gpu_amd.c:313-315` `ADLSingleSensorData* sensor = &pmLogDataOutput.sensors[ADL_PMLOG_TEMPERATURE_HOTSPOT]; … if (sensor->supported) { *result.temp = sensor->value; }`（同样的 `if (sensor->supported)` 还出现在 `:319-321`、`:325-327`、`:337-339`）。④ 权威支持列表：`adl_structures.h:3297-3303` `typedef struct ADLPMLogSupportInfo { unsigned short usSensors[ADL_PMLOG_MAX_SUPPORTED_SENSORS]; int ulReserved[16]; }`；消费方扫描实现 `AmdGpu.cs:444-458` `private bool IsSensorSupportedByPMLog(AtiAdlxx.ADLPMLogSensors sensorType) { if (!_pmLogStarted \|\| (int)sensorType == 0) return false; for (int i = 0; i < AtiAdlxx.ADL_PMLOG_MAX_SENSORS; i++) { if (_adlPMLogSupportInfo.usSensors[i] == (int)sensorType) return true; } return false; }`，与 `nwinfo_amd_gpu.c:61-76` `is_pmlog_supported()` 逐行同构（后者多一句 `if (gpu->PMLogSupportInfo.usSensors[i] == (unsigned short)ADL_SENSOR_MAXTYPES) break;`）。⑤ 位图字段：`adl_defines_cortex.h:1966` `PMLOG_THROTTLER_STATUS = 35, // A bit map of GPU throttle information. If a bit is set, the bit represented type of thorttling occurred in the last metrics sampling period`，位定义在 `:2010-2015` `typedef enum ADL_THROTTLE_NOTIFICATION { ADL_PMLOG_THROTTLE_POWER = 1 << 0, ADL_PMLOG_THROTTLE_THERMAL = 1 << 1, ADL_PMLOG_THROTTLE_CURRENT = 1 << 2, }`（同样三行在 `PresentMon_adl_defines.h:1954-1956`、`ghelper_AmdAdl2.cs:158-160`）。 |
| 置信度 | 「`supported` 是布尔、消费方只测非 0」：**已证实**（三实现 + 官方文档无位掩码措辞）；「AMD 官方明确定义该字段」：**未找到可靠来源**（正面定义不存在）；「索引 35 是位图」：**已证实** |
| 对设计的影响 | 读值判据写成 `if (s.supported != 0)`；**不要写 `if (s.supported & 1)` 或跟任何掩码比较**——若某版驱动返回 2 或别的非 1 值，掩码式写法会误判为「不支持」。启动时另调 `ADL2_Adapter_PMLog_Support_Get` 拿权威支持列表，用它来决定「是否注册该指标」，用 `supported` 决定「本次是否有值」——两者语义不同，不能互相替代。索引 0 必须显式排除。 |

---

## 4. 缩放系数：只有电压要换算，且原始值本身就是 mV

| 项 | 内容 |
|---|---|
| 结论 | PMLog 原始值**绝大多数已是目标单位**（MHz / °C / W / A / % / RPM / 链路代数），**不需要任何换算**。只有三个电压字段（16 `SOC_VOLTAGE`、21 `GFX_VOLTAGE`、22 `MEM_VOLTAGE`）在 LHM 与 nwinfo 中被 **× 0.001**，但那**不是「修正」而是「把 mV 换成 V」**——AMD 头文件明说原始单位就是 mV。想直接以 mV 展示就**不要**乘 0.001。对照陷阱：**旧的 `ADLTemperature.iTemperature` 才是毫摄氏度**（`AmdGpu.cs:606` `temperatureCore.Value = 0.001f * temperature.iTemperature;`），与 PMLog 的整数 °C 是两套 API，勿混用。 |
| 证据 | ① 电压 ×0.001：`AmdGpu.cs:427-429` `GetAdlSensor(adlPMLogData, logDataOutput, AtiAdlxx.ADLPMLogSensors.ADL_PMLOG_GFX_VOLTAGE, _coreVoltage, 0.001f, false);` / `…ADL_PMLOG_SOC_VOLTAGE, _socVoltage, 0.001f…` / `…ADL_PMLOG_MEM_VOLTAGE, _memoryVoltage, 0.001f, false);`；`nwinfo_amd_gpu.c:507-510` `get_adl_sensor(gpu, pmlog_data, &od8_log, ADL_PMLOG_GFX_VOLTAGE, &val_f, 0.001)` 否则 `ADL_PMLOG_SOC_VOLTAGE … 0.001`。② 其余全部 1.0：`AmdGpu.cs:409-441` 的 `GetAdlSensor(...)` 调用的 `factor` 参数默认 `1.0f`（签名 `:471-472` `private bool GetAdlSensor(AtiAdlxx.ADLPMLogData adlPMLogData, AtiAdlxx.ADLPMLogDataOutput od8Log, AtiAdlxx.ADLPMLogSensors sensorType, Sensor sensor, float factor = 1.0f, bool reset = true)`），仅 `:427-429` 三处显式传 `0.001f`；`nwinfo_amd_gpu.c:512-521` 的功耗链（`BOARD_POWER` → `GFX_POWER` → `ASIC_POWER` → `SSPAIRED_ASICPOWER` → `SOC_POWER`）与 `:523-530` 的 `CLK_GFXCLK`/`CLK_MEMCLK`/`INFO_ACTIVITY_GFX` 全部因子 1.0。③ 原始单位是 mV（头文件）：`adl_defines_cortex.h:1952` `PMLOG_GFX_VOLTAGE = 21, // Current graphic voltage in mV`。④ 毫摄氏度陷阱：`AmdGpu.cs:606`（旧 `ADLTemperature` 路径）、`AmdGpu.cs:804` `Vddc: 0.001f * adlp.iVddc`（旧 `ADLPMActivity.iVddc`，mV→V）、`AmdGpu.cs:547` `_coreClock.Value = 0.01f * adlpmActivity.iEngineClock;`（旧 OD5 时钟单位是 10 kHz，故 ×0.01 得 MHz——**PMLog 的时钟没有这个问题**）。⑤ PMLog 电压值本就是 mV 的实测旁证：本机索引 21 = `1235`，×0.001 = 1.235 V，与 780M 的 GFX 电压量程吻合（见 §10 自洽性检验）。 |
| 置信度 | 电压 ×0.001（＝mV→V）：**已证实**；其余字段因子 1.0：**已证实**；「PMLog 温度不需 ×0.001」：**已证实**（LHM 只在旧 API 上乘 0.001） |
| 对设计的影响 | 采集层统一**保留原始整数**并附单位标签，换算放到展示层：`soc_mv`、`gfx_mv`、`mem_mv` 直接存 mV；`gfx_mhz`、`soc_mhz`、`mem_mhz`、`*_c`、`*_w`、`*_a`、`*_pct` 原样存。切勿沿用「PMLog 值都要 ×0.001」的误解。 |

---

## 5. 结构体布局与调用约定

| 项 | 内容 |
|---|---|
| 结论 | 官方 `ADLPMLogDataOutput` 的第一个字段就是 `int size`，之后紧跟 `ADLSingleSensorData sensors[256]`（每项 8 字节，共 2048 字节 + 4 字节 + 对齐）。**调用方手写的 `{ int iSize; ADLSingleSensorData sensors[256]; }` 与官方布局逐字节一致**，可安全使用（本机索引 1/2/3 分别对应 800/2800/400，与头文件 GFXCLK/MEMCLK/SOCCLK 顺序吻合，反证没有错位）。`size` 字段在 Windows 上填不填都能工作：只有 nwinfo 显式初始化，LHM 与 fastfetch 都是零初始化。调用约定为 **cdecl**，DLL 用 `LoadLibraryW(L"atiadlxx.dll")` + `GetProcAddress` 动态取符号即可（AMD 官方明确背书这种做法）。 |
| 证据 | ① 官方结构：`GPUOpen-LibrariesAndSDKs/display-library:include/adl_structures.h:3209-3219`（`typedef struct ADLSingleSensorData { int supported; int value; } ADLSingleSensorData;` + `typedef struct ADLPMLogDataOutput { int size; ADLSingleSensorData sensors[ADL_PMLOG_MAX_SENSORS]; }ADLPMLogDataOutput;`），本地副本 `build/_adlres/sdk_adl_structures.h:3209-3219`；官方文档页 <https://gpuopen-librariesandsdks.github.io/adl/structADLPMLogDataOutput.html> 显示成员 `ADLSingleSensorData sensors[256]`。② 第三方副本同样带 `size`：`AtiAdlxx.cs:508-515`（`:511` `public int size;`，`:513` `[MarshalAs(UnmanagedType.ByValArray, SizeConst = ADL_PMLOG_MAX_SENSORS)]`）、`ghelper_AmdAdl2.cs:43-47`。③ 只有 nwinfo 设置它：`nwinfo_amd_gpu.c:478` `ADLPMLogDataOutput od8_log = { .size = sizeof(ADLPMLogDataOutput) };`；LHM `AmdGpu.cs:396` `ADLPMLogDataOutput logDataOutput = new();`、fastfetch `ff_gpu_amd.c:308` `ADLPMLogDataOutput pmLogDataOutput = {};` 均零初始化。④ 调用约定：`AtiAdlxx.cs:43` `internal const string DllName = "atiadlxx.dll";`，每个入口都是 `[DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]` + `[DefaultDllImportSearchPaths(DllImportSearchPath.System32)]`（`:54-66`）；`hashcat/hashcat:include/ext_ADL.h` 中 `#if defined (__MSC_VER) #define HC_ADL_API_CALL __cdecl #elif defined (_WIN32) … __stdcall`。⑤ 动态加载：`a1ive/nwinfo:libnw/gpu/adl.c` 全篇为 `typedef int (*NAME)(...)` + `GetProcAddress(GetModuleHandleW/…LoadLibraryW(L"atiadlxx.dll"))`，符号缺失时包装函数返回 `ADL_ERR_NOT_SUPPORTED`。⑥ AMD 官方背书：<https://gpuopen-librariesandsdks.github.io/adl/overdrive8_8h.html> 对 `overdrive8.h` 的说明——`All functions found in this file can be used as a reference to ensure the specified function pointers can be used by the appropriate runtime dynamic library loaders.`。⑦ `ADL_PMLOG_MAX_SENSORS 256`：`adl_defines_cortex.h:1926`、`adl_defines_libcec.h:1962`、`PresentMon_adl_defines.h:1897`、`nwinfo_adl.h:99`、`AtiAdlxx.cs:41`、`ghelper_AmdAdl2.cs:405`。 |
| 置信度 | 布局与 256 上限：**已证实**；`size` 可省略：**已证实**（两实现零初始化照样工作）；cdecl：**已证实** |
| 对设计的影响 | 保持调用方现有结构体定义；建议仍照 nwinfo 写入 `iSize = sizeof(ADLPMLogDataOutput)`（成本为零，且对将来结构增长是防御）；所有 ADL 入口按 `__cdecl` 声明（`extern "C"` + `__cdecl`）。`ADL_PMLOG_MAX_SENSORS` 一律用 256，不要用 `MAX_SENSORS_REAL`（后者版本相关，74 或 75）。 |

---

## 6. 三条 PMLog API 路径，以及「一次性查询」已被 AMD 官方弃用

| 项 | 内容 |
|---|---|
| 结论 | PMLog 有三条并存路径：**(a) 一次性查询** `ADL2_New_QueryPMLogData_Get`（**调用方当前使用**，AMD 已正式标记 Deprecated）；**(b) 共享内存 Start/Stop 路径**（`Device_Create` → `Support_Get` → `Start` → 读共享内存 → `Stop` → `Device_Destroy`，LHM 与 nwinfo 都用它）；**(c) 更新的 `ADL2_Overdrive8_PMLog_ShareMemory_*` 家族**（AMD 推荐的替代）。官方弃用理由原文：性能低。对本项目而言 (a) 仍可用（本机实测 `ADL_OK`），但应把 (b)/(c) 作为演进方向，并用 `ADL_Method_Exists` 式符号探测做降级链。 |
| 证据 | ① 官方弃用原文（<https://gpuopen-librariesandsdks.github.io/adl/group__OVERDRIVE8API.html>，`ADL2_New_QueryPMLogData_Get` 条目）：`[Deprecated:] This API has been deprecated because it's inefficient when getting GPU metrics. Use ADL2_Overdrive8_PMLog_ShareMemory_Read API, when ADL2_Overdrive8_PMLog_ShareMemory_Support is supported.`；该函数签名同页为 `ADL2_New_QueryPMLogData_Get(ADL_CONTEXT_HANDLE context, int iAdapterIndex, ADLPMLogDataOutput *lpDataOutput)`，描述 `Function to retrieve the Overdrive8 current settings`，Supported Platforms `Windows 7, 10 and above; 32bit and 64bit`。② 共享内存路径的官方函数集（同页）：`ADL2_Adapter_PMLog_Support_Get(ctx, iAdapterIndex, ADLPMLogSupportInfo*)`（`Function to retrieve power management logging support.`）、`ADL2_Adapter_PMLog_Start(ctx, iAdapterIndex, ADLPMLogStartInput*, ADLPMLogStartOutput*, ADL_D3DKMT_HANDLE hDevice)`、`ADL2_Adapter_PMLog_Stop`、`ADL2_Device_PMLog_Device_Create(ctx, iAdapterIndex, ADL_D3DKMT_HANDLE* pDevice)`（`Adds MGPU support over legacy functions.`）、`ADL2_Device_PMLog_Device_Destroy`、`ADL2_Adapter_PMLog_SensorLimits_Get(ctx, iAdapterIndex, ADLPMLogSensorLimits*)`；结构体见 `adl_structures.h:3295-3362`（`:3311-3319` `ADLPMLogStartInput { unsigned short usSensors[256]; unsigned long ulSampleRate; /* Sample rate in milliseconds */ int ulReserved[15]; }`、`:3321-3333` `ADLPMLogData { … unsigned int ulValues[ADL_PMLOG_MAX_SUPPORTED_SENSORS][2]; … }`、`:3341-3351` `ADLPMLogStartOutput { union { void* pLoggingAddress; unsigned long long ptr_LoggingAddress; }; int ulReserved[14]; }`、`:3359-3362` `ADLPMLogSensorLimits { int SensorLimits[ADL_PMLOG_MAX_SENSORS][2]; //index 0: min, 1: max }`）。③ 共享内存数据布局的消费方解读：`AmdGpu.cs:500-518` 以 `for (int k = 0; k < adlPMLogData.ulValues.Length - 1; k += 2) { if (adlPMLogData.ulValues[k] == (ushort)AtiAdlxx.ADLPMLogSensors.ADL_SENSOR_MAXTYPES) break; if (adlPMLogData.ulValues[k] == i) { sensor.Value = adlPMLogData.ulValues[k + 1] * factor; … } }`；`nwinfo_amd_gpu.c:482-483` `pmlog_data = (ADLPMLogData*)gpu->PMLogStartOutput.pLoggingAddress;`。④ 采样率惯例 1000 ms：`AmdGpu.cs:65` `private readonly ushort _pmLogSampleRate = 1000;`，`nwinfo_amd_gpu.c:117-145` `PMLogStartInput.ulSampleRate = 1000`；来源自 LHM PR #1308（<https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/pull/1308>，标题 `Add ADL pmlog support, fix metrics on newer gpus`，正文 `Newer GPUs needs this to show performance metrics properly. Sample rate is hard coded (1000ms). Initially is set to be used with RX 7000 series to keep the current logic, probably would work on previous generations of gpus.`——**该页为 JS 外壳，只能作二手证据引用**）。⑤ 更新的家族（同页 group__OVERDRIVE8API）：`ADL2_Overdrive8_PMLog_ShareMemory_Support(ctx, iAdapterIndex, int *lpSupported, int option)`（`This function retrieves the supporting flag of shared memory way is supported by the driver to read sensor's real value`）、`ADL2_Overdrive8_PMLog_ShareMemory_Start(ctx, iAdapterIndex, int iSampleRate, int iNumofPMLogSendorList, int *lpPMLogSendorList, ADL_D3DKMT_HANDLE *lpHDevice, void **lppSharedMemory, int iOption)`、`ADL2_Overdrive8_PMLog_ShareMemory_Read(ctx, iAdapterIndex, int iNumSensor, int *lpSensorList, void **lppSharedMemory, ADLPMLogDataOutput *lpDataOutput)`、`ADL2_Overdrive8_PMLog_ShareMemory_Stop`、`ADL2_Overdrive8_PMLogSenorType_Support_Get(ctx, iAdapterIndex, int *lpNumOfSupportedSensorType, int **lppSenroTypesList)`（`Function to retrieve the PMLog sensor real value reading support flag from the driver`）。**注意 AMD 把 `Sensor` 拼成了 `Senor`，符号名照抄才能取到地址。** |
| 置信度 | 弃用状态与替代路径：**已证实**（AMD 官方页原文）；(b) 的字段语义与消费方式：**已证实**；`ShareMemory_Start` 的 `iSampleRate` 单位：**官方文档自相矛盾**（正文先说 `how many milli-seconds the driver needs to refresh the context of the shared memory`，紧接着写 `The unite is second.`）——按 (b) 的既有惯例取 **ms**，并把该矛盾记为已知风险 |
| 对设计的影响 | 采集器设计成**符号探测 + 三级降级**：(c) `ShareMemory_Support` 为真 → 用 (c)；(b) 三个符号都在 → 用 (b)；(a) 在 → 用 (a)。当前实现（a）可用，但要在诊断信息里标注 `pmlog_path=oneshot(deprecated)`，并预留 (b) 的采样线程。 |

---

## 7. 集显 / APU 是否支持 PMLog：**支持**，门槛是 ASIC 家族 ≥ Vega

| 项 | 内容 |
|---|---|
| 结论 | **PMLog 不限于独显**。两个权威消费方都用「ASIC 家族 ≥ `FAMILY_AI`（Vega，141）」作为启用条件，Phoenix / Rembrandt / Van Gogh 等集显全部满足。本机 `AMD Ryzen 7 H 255`（Hawk Point，Phoenix 系，GC 11.0.1）实测 `ADL2_New_QueryPMLogData_Get` 返回 `ADL_OK` 且给出 16 个有值传感器，**直接证实集显可用**。但**可用传感器集合与独显不同**：本机 `TEMPERATURE_EDGE(8)`、`TEMPERATURE_HOTSPOT(27)`、`FAN_RPM(14)`、`INFO_ACTIVITY_GFX(19)` 均**不支持**，取而代之的是 GFX/SOC 域温度 (28/29) 与 APU 侧的 CPU 温度/功耗/频率 (32/33/34)。 |
| 证据 | ① 家族门槛：`LibreHardwareMonitor/LibreHardwareMonitor:LibreHardwareMonitorLib/Interop/AtiAdlxx.cs:229-232` `public static bool UsePmLogForFamily(int familyId) { return familyId >= (int)GCNFamilies.FAMILY_AI; }`，调用点 `LibreHardwareMonitorLib/Hardware/Gpu/AmdGpu.cs:176-179`（`if (AtiAdlxx.UsePmLogForFamily(_adlGcnInfo.ASICFamilyId) && ADL_Method_Exists(nameof(AtiAdlxx.ADL2_Adapter_PMLog_Support_Get)) && ADL_Method_Exists(nameof(AtiAdlxx.ADL2_Device_PMLog_Device_Create)) && ADL_Method_Exists(nameof(AtiAdlxx.ADL2_Adapter_PMLog_Start)))`）；`a1ive/nwinfo:libnw/gpu/amd_gpu.c:117-119` `static void init_pmlog(ADL_CTX* ctx, ADL_GPU_INFO* gpu, int adapter_index) { … if (gpu->GcnInfo.ASICFamilyId < FAMILY_AI) return; … }`。② 家族编号：`AtiAdlxx.cs:596-615` `FAMILY_AI = 141, // Vega: 10, 20`、`FAMILY_RV = 142`、`FAMILY_VGH = 144, // Van Gogh (RDNA 2.0)`、`FAMILY_YC = 146, // Rembrandt (Yellow Carp, RDNA 2.0)`、`FAMILY_GC_11_0_1 = 148, // Phoenix (GC 11.0.1, RDNA 3.0)`、`FAMILY_GC_10_3_7 = 151, // Mendocino`；同表在 `nwinfo_adl.h:544-560`。③ ADL 自己的「是否集显」位掩码（**这才是位图**）：`adl_defines_cortex.h:1157-1167` `#define ADL_ASIC_DISCRETE (1 << 0)` / `#define ADL_ASIC_INTEGRATED (1 << 1)` / `ADL_ASIC_FUSION (1 << 5)` …；用法（两个 out 参数：类型集 + 有效性掩码）`nwinfo_amd_gpu.c:378-383` `if ((gpu->AsicTypeValid & ADL_ASIC_INTEGRATED) && (gpu->AsicType & ADL_ASIC_INTEGRATED)) info->Flags \|= NWLIB_GPU_FLAG_INTEGRATED; if ((gpu->AsicTypeValid & ADL_ASIC_FUSION) && (gpu->AsicType & ADL_ASIC_FUSION)) … \|= NWLIB_GPU_FLAG_INTEGRATED; if ((gpu->AsicTypeValid & ADL_ASIC_DISCRETE) && (gpu->AsicType & ADL_ASIC_DISCRETE)) info->Flags &= ~NWLIB_GPU_FLAG_INTEGRATED;`，接口 `nwinfo_adl.h:605` `int ADL2_Adapter_ASICFamilyType_Get(void* pContext, int iAdapterIndex, int* lpAsicTypes, int* lpValids);`。④ 本机实测：`ADL2_New_QueryPMLogData_Get` 返回 `ADL_OK`，16 个索引有值（见 §10 表）；GPU 为 `PCI\VEN_1002&DEV_1900`（Radeon 780M），驱动 `32.0.31041.1004`，`atiadlxx.dll` 版本 `7.26.10.1609`。 |
| 置信度 | 「集显可用 PMLog」：**已证实**（代码门槛 + 本机实测）；「门槛恰为 `FAMILY_AI`」：**已证实**（两实现一致，但属社区经验而非 AMD 文档）；「独显与集显传感器集合不同」：**已证实**（本机 8/14/15/19/27 缺失） |
| 对设计的影响 | 不要写「非独显则回退到旧 API」的分支；改用**能力探测**：先 `ADL2_Adapter_ASICFamilyType_Get` 判集显（给 UI 打标签），再以 `PMLog_Support_Get` 的实际列表决定采集哪些指标。温度优先级链按 LHM 写法：`EDGE(8)` → 失败退 `GFX(28)`；本机走的是后者。计划里「GPU 热点温度」在本机**拿不到**，需在 UI 上标注不可用而不是显示 0。 |

---

## 8. 管理员权限、`enumConnected` 与线程安全

| 项 | 内容 |
|---|---|
| 结论 | **(1) 不需要管理员**：本机以**非提权**身份（`WindowsPrincipal.IsInRole(Administrator)` 返回 `False`）成功完成 `Main_Control_Create` → `AdapterInfo_Get` → `QueryPMLogData_Get` 全链路。SDK 里确有 `ADL_ERR_NO_ADMINISTRATOR_PRIVILEGES = -23`（注释 `not running as administrator`），但**没有任何来源把它与 PMLog/Overdrive8 读取关联**；LHM / nwinfo / fastfetch 都不为此请求提权。**(2) `enumConnected` 0 与 1 有实质差别（AMD 官方定义）**：1 ＝ 只枚举「物理存在且已启用」的适配器，0 ＝ 枚举「曾经出现过」的全部适配器（含幽灵/已移除）。三个消费方一律传 **1**。**(3) ADL 默认非线程安全**：官方明说 `ADL2_Main_Control_Create` 不做序列化，调用方必须自己保证串行；需要库内加锁则用 `ADL2_Main_ControlX2_Create` / `X3_Create` 传入 `ADLThreadingModel`。 |
| 证据 | ① 非提权实测：本会话在调用方机器上执行 `([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)` → `False`，而用户提供的 ADL 调用序列全部返回 `ADL_OK`。② 错误码：`adl_defines_cortex.h:138-139`（`ADL_ERR_NO_ADMINISTRATOR_PRIVILEGES = -23, // not running as administrator`），同文在 `adl_defines_libcec.h:140-141`、`PresentMon_adl_defines.h:138-139`、`nwinfo_adl.h:86-87`；fastfetch 映射为字符串 `ff_gpu_amd.c:34` `FF_ADL_STATUS_CASE(ADL_ERR_NO_ADMINISTRATOR_PRIVILEGES)`。③ `enumConnected` 官方定义（<https://gpuopen-librariesandsdks.github.io/adl/adl_8h.html>，`ADL2_Main_Control_Create` 参数 `iEnumConnectedAdapters`）：`Specify a value of 0 to retrieve adapter information for all adapters that have ever been present in the system. Specify a value of 1 to retrieve adapter information only for adapters that are physically present and enabled in the system.`；同页给出 `ADL2_Main_Control_Refresh`：`This function generates an adapter index value for all logical adapters that have ever been present in the system`，并说明 refresh 后适配器索引与 UDID 映射不变。④ 消费方一律传 1：`ff_gpu_amd.c:106` `ffADL2_Main_Control_Create(ffAdlMainMemoryAlloc, 1 /*iEnumConnectedAdapters*/, &adlData.apiHandle);`、`AmdGpuGroup.cs:27` `AtiAdlxx.ADL2_Main_Control_Create(AtiAdlxx.Main_Memory_Alloc, 1, ref _context);`、`nwinfo_amd_gpu.c:233` `ctx->Result = ADL2_Main_Control_Create(1, &ctx->AdlHandle);`。⑤ 线程安全原文（同 adl_8h.html 页）：`ADL initialized using ADL2_Main_Control_Create will not enforce serialization of ADL API executions by multiple threads … Note that ADL library is not guaranteed to be thread-safe. Client that calls ADL2_Main_Control_Create have to provide its own mechanism for ADL calls serialization.`；变体 `ADL2_Main_ControlX2_Create(callback, iEnumConnectedAdapters, &context, ADLThreadingModel threadingModel)`、`ADL2_Main_ControlX3_Create(…, int adlCreateOptions)`（首个选项位决定驱动不兼容的 AMD 适配器是否仍报为 AMD）。 |
| 置信度 | 非提权可用：**已证实**（本机实测）；`enumConnected` 语义：**已证实**（AMD 官方文档）；线程安全约束：**已证实**（AMD 官方文档）；「某些 ADL 功能需要管理员」：**未找到可靠来源**（只知道错误码存在，不知道触发条件） |
| 对设计的影响 | 采集线程**单点串行**所有 ADL 调用（一把互斥锁或单一采集线程）——不要在多线程里并发调用 ADL。启动时若任一次调用返回 `-23`，**单独报诊断**（「ADL 返回 NO_ADMINISTRATOR_PRIVILEGES」）而不是静默降级到别的数据源，因为该错误码成因不明，值得暴露。`enumConnected` 保持 **1**。 |

---

## 9. 5 个重复适配器与 `iVendorID = 0x03EA` 的成因

| 项 | 内容 |
|---|---|
| 结论 | 两件事都是已知现象，且都可修：**(1) 重复** —— ADL 会为同一物理 GPU 报多条适配器记录（UMA/多显示器/虚拟显示驱动场景常见）。权威做法是 **按 `(iBusNumber, iDeviceNumber)` 去重**，并先跳过 `strUDID` 为空的条目。本机除 AMD 780M 外还装了 3 个虚拟显示适配器（`GameViewer Virtual Display Adapter`、`OrayIddDriver Device`、`Honor Virtual Display Device`），与「5 条同 GPU 记录」高度吻合（**推断**，未逐条核对 ADL 输出）。**(2) `iVendorID` 报 `0x03EA` 不是「不是 AMD」** —— AMD 的 PCI 厂商号是 `0x1002`，而 `1002` **十进制**恰好等于 `0x03EA`；驱动把十六进制文本当十进制解析了。nwinfo 因此直接从 `strUDID` 里正则提取 `PCI_VEN_` 后面的十六进制值覆盖该字段，并留下注释记录这一 bug。**副作用警告**：LHM 以 `VendorID == 0x1002` 过滤适配器，在本机这种驱动上会**一个 AMD 适配器都选不出来**；本机之所以能取到数据，正是因为没有按该字段过滤。 |
| 证据 | ① 去重实现：`nwinfo_amd_gpu.c:259-293`（`:264` `if (adapter->strUDID[0] == '\0') continue;`；`:280-293` 以 `(BusNumber, DeviceNumber)` 丢弃重复项；`:299-316` 再从 UDID 解析 `&DEV_` / `&SUBSYS_` / `&REV_`）；LHM 侧 `AmdGpuGroup.cs:114-125` 过滤条件 `!string.IsNullOrEmpty(adapterInfo[i].UDID) && adapterInfo[i].VendorID == AtiAdlxx.ATI_VENDOR_ID && !IsAlreadyAdded(adapterInfo[i].BusNumber, adapterInfo[i].DeviceNumber)`，`AmdGpuGroup.cs:150-159` `private bool IsAlreadyAdded(int busNumber, int deviceNumber) { foreach (AmdGpu g in _hardware) { if (g.BusNumber == busNumber && g.DeviceNumber == deviceNumber) { return true; } } return false; }`；`AtiAdlxx.cs:39` 定义 `ATI_VENDOR_ID = 0x1002`。② 厂商号 bug：`nwinfo_amd_gpu.c:266-275` 注释原文 `// AMD vendor id == 0x1002 (reported value is 0x3EA)`，随后 `char* id_start = strstr(adapter->strUDID, "PCI_VEN_"); … adapter->iVendorID = (int)strtol(id_start, NULL, 16); … if (adapter->iVendorID != PCI_VID_ATI) continue;`。③ 本机实测：`Win32_VideoController` 中 AMD 条目名 `AMD Radeon (TM) Graphics`、驱动 `32.0.31041.1004`，另有上述 3 个虚拟显示适配器；用户侧观察到 `ADL2_Adapter_NumberOfAdapters_Get` = 5 且 `strAdapterName` 全为 `AMD Radeon (TM) Graphics`、`iVendorID = 0x03EA`。④ 适配器选择策略（择优而非取第一条）：`AmdGpuGroup.cs:131-135` `potentialHardware.GroupBy(x => $"{x.BusNumber}-{x.DeviceNumber}")` + `OrderByDescending(x => x.Sensors.Length).FirstOrDefault()`。 |
| 置信度 | 「按 (BusNumber, DeviceNumber) 去重 + 跳过空 UDID」：**已证实**（两实现一致）；「`0x03EA` 是 1002 十进制/十六进制混读」：数值恒等式**已证实**（1002₁₀ = 0x03EA），成因归因**推断**；「5 条记录源于 3 个虚拟显示适配器」：**推断**，未逐条核对 |
| 对设计的影响 | 适配器选择写三步：跳过空 UDID → 按 `(BusNumber, DeviceNumber)` 去重 → 同组内选「PMLog 支持传感器数最多」的那条。**不要**用 `iVendorID == 0x1002` 过滤；若要校验厂商，从 `strUDID` 解析（或同时接受 `0x1002` 与 `0x03EA`）。加日志把 5 条记录的 `AdapterIndex / UDID / BusNumber / DeviceNumber / iPresent` 全部打印，便于下次诊断。 |

---

## 10. 对调用方实测 16 个索引的逐项判定

**实测样本**（空闲，`ADL2_New_QueryPMLogData_Get`，非零 `supported` 项）：
`1=800, 2=2800, 3=400, 7=800, 16=765, 17=1, 18=2, 21=1235, 23=25, 28=52, 29=53, 30=18, 31=14, 32=62, 33=3, 34=3931`（跨适配器波动：`34` 3931/4046、`30` 18/16/15、`31` 14/13/12、`32` 62/61、`33` 3/2）。

| 索引 | 判定 | 显示值 | 依据与自洽性检验 |
|---|---|---|---|
| 1 | **图形核心时钟** | 800 MHz | 头文件 `// Current graphic clock value in MHz`；空闲 800 MHz 与 780M 空闲频率量级吻合 |
| 2 | **显存时钟** | 2800 MHz | 头文件 `// Current memory clock value in MHz`；`2800 = 5600 MT/s ÷ 2`，与 DDR5-5600 的 I/O 时钟精确对应（**待运行期核对内存规格**，见 §12 第 6 步） |
| 3 | **SoC 时钟** | 400 MHz | 名称确定，单位按命名推断（头文件无注释） |
| 7 | **VCN（视频编解码）时钟** | 800 MHz | 名称确定；本机与 GFXCLK 同值，符合 APU 上 VCN 与 GFX 同域的常见行为（**推断**） |
| 16 | **SoC 电压** | 765 mV | 头文件名 + 消费方 ×0.001 惯例；原始值即 mV |
| 17 | **SoC 功耗** | 1 W | 与 16/18 满足 P≈U·I：0.765 V × 2 A = 1.53 W，量级一致（采样时刻不同故不完全相等） |
| 18 | **SoC 电流** | 2 A | 同上，见 17 |
| 21 | **图形核心电压** | 1235 mV | 头文件 `// Current graphic voltage in mV`；与 30/31 满足 P≈U·I（见下） |
| 23 | **ASIC 功耗** | 25 W | 头文件 `// Current ASIC power draw in Watt`；≈ 30(GFX 18 W) + 17(SoC 1 W) + 33(CPU 3 W) + 未暴露域/损耗，量级自洽。**提示这是封装级读数，不是 iGPU 单独功耗** |
| 28 | **GFX 域温度** | 52 °C | 名称确定（头文件无单位注释，按温度组类比）；是 EDGE(8) 缺失时的替代来源 |
| 29 | **SoC 域温度** | 53 °C | 同上；与 28 相差 1 °C，符合同芯片两域 |
| 30 | **GFX 域功耗** | 15–18 W | **关键交叉验证**：21(1.235 V) × 31(12–14 A) = 14.8–17.3 W，与 30 的 15–18 W **逐一对应**——三个字段互证为「同一 GFX 域的 U/I/P」 |
| 31 | **GFX 域电流** | 12–14 A | 同上 |
| 32 | **CPU 温度** | 61–62 °C | APU 专有字段；与 33(2–3 W) 结合看是空闲态典型值 |
| 33 | **CPU 功耗** | 2–3 W | 同上 |
| 34 | **CPU 时钟** | 3931–4046 MHz | 与 `Win32_Processor.MaxClockSpeed = 3801` 同量级（锐龙 boost 可高于标称基频/最大值，故偏大合理） |

**未在本机出现（即 `supported == 0`）的索引**：`8` EDGE 温度、`14` 风扇 RPM、`15` 风扇百分比、`19` GFX 活动率、`20` 显存活动率、`27` 热点温度。**这是本任务最实用的结论之一**：LHM 的 `TEMPERATURE_EDGE else TEMPERATURE_GFX` 回退链在本机是**必需**的（走 28），而 `INFO_ACTIVITY_GFX(19)` 拿不到，GPU 占用率必须另找来源。

**GPU 占用率的可行替代**（本机 19 不支持时的路径）：`ADL2_OverdriveN_PerformanceStatus_Get` → `ADLODNPerformanceStatus.iGPUActivityPercent`。
证据：结构体 `AtiAdlxx.cs:466-474`（`:474` `public int iGPUActivityPercent;`）；接口 `AtiAdlxx.cs:88` `public static extern ADLStatus ADL2_OverdriveN_PerformanceStatus_Get(IntPtr context, int adapterIndex, out ADLODNPerformanceStatus performanceStatus);`；两处独立消费：`nwinfo_amd_gpu.c:456-472`（`:460-461` `if (gpu->OdnPerf.iGPUActivityPercent > 0) info->UsagePercent = (double)gpu->OdnPerf.iGPUActivityPercent;`，同段还给出 `iVDDC * 0.001f`、`iGFXClock * 0.01f`、`iMemoryClock * 0.01f`、`iCurrentBusSpeed/Lanes`）与 `fastfetch-cli/fastfetch:src/detection/gpu/gpu_amd.c:400-415`（`:411` `*result.coreUsage = performanceStatus.iGPUActivityPercent;`，调试打印 `:406-409` `Performance Status - Activity: %d%%, CoreClock: %dMHz, MemoryClock: %dMHz`）。**注意该路径的单位与 PMLog 不同**：`iGFXClock`/`iMemoryClock` 是 10 kHz 单位，要 ×0.01 才是 MHz；`iVDDC` 是 mV，×0.001 得 V。

**证据（本表判定所用）**：名称与顺序见 §1 的枚举证据；电压换算见 §4；`ADLODNPerformanceStatus` 见上段；本机观测值由调用方提供（`ADL2_New_QueryPMLogData_Get` 单次采样）。
**置信度**：`1/2/3/7/16/17/18/21/23/28/29/30/31/32/33/34` ↔ 语义的**名称映射**：**已证实**（头文件枚举顺序 + 调用方索引 1/2/3 与头文件 GFXCLK/MEMCLK/SOCCLK 对齐，反证无错位）；**每个字段的物理量级解释**：30/31/21 三者互证为 **已证实**，`34` 为 **推断**，`2` 为 **推断（待核内存规格）**；「跨实现的独立确认」：**未找到可靠来源**——没有任何开源消费方读取 30–34（对 `TEMPERATURE_CPU|CPU_POWER|CLK_CPUCLK|GFX_CURRENT|SOC_CURRENT|CLK_VCNCLK` 的检索在 LHM / nwinfo / fastfetch 中均无命中），故这几项只有 AMD 头文件这一手证据 + 量级自洽。

---

## 11. VRAM 用量不在 PMLog 里

| 项 | 内容 |
|---|---|
| 结论 | **整个 PMLog 索引空间（0–74）中没有任何「显存用量」传感器**。显存用量必须走专用接口 `ADL2_Adapter_DedicatedVRAMUsage_Get`（返回 **MB**），容量走 `ADL2_Adapter_MemoryInfoX4_Get`（失败退 `ADL2_Adapter_MemoryInfo2_Get`）。LHM 与 nwinfo 都这么做，且两者都注明「ADL 返回的是 MB」。**不要**为显存用量发明一个 PMLog 索引。 |
| 证据 | ① 索引空间无此字段：`adl_defines_cortex.h:2019-2093` 全枚举（0–73）已逐项核对，无 memory-used/VRAM 语义项。② 专用接口与换算：`AmdGpu.cs:342-349` `ADL2_Adapter_DedicatedVRAMUsage_Get(_context, idx, out vramUsed)` 且注释 `//ADL reports VRAM usage in megabytes, convert to bytes for the sensor.` → `_memoryUsed.Value = vramUsed * 1024f * 1024f;`；`nwinfo_amd_gpu.c:175-203` `get_gpu_mem` 用 `ADL2_Adapter_MemoryInfoX4_Get` 取 `iMemorySize`（退 `ADL2_Adapter_MemoryInfo2_Get`）+ `ADL2_Adapter_DedicatedVRAMUsage_Get`，`info->UsedMemory = ((uint64_t)gpu->VRamUsed) * 1024 * 1024;` 后算 `MemoryPercent = 100ULL * UsedMemory / TotalMemory`。③ 原型：`nwinfo_adl.c` 的动态符号表含 `ADL2_ADAPTER_DEDICATEDVRAMUSAGE_GET(…, int*)`、`ADL2_ADAPTER_MEMORYINFOX4_GET(…, ADLMemoryInfoX4*)`、`ADL2_ADAPTER_MEMORYINFO2_GET(…, ADLMemoryInfo2*)`；`ADLMemoryInfo2` 字段 `long long iMemorySize; /* Memory size in bytes */`、`long long iMemoryBandwidth; /* Memory bandwidth in Mbytes/s */`（`adl_structures.h` 对应段）。 |
| 置信度 | 已证实 |
| 对设计的影响 | GPU 显存指标单列一条采集路径，与 PMLog 解耦；单位在采集层统一为字节。集显（UMA）上「显存用量」语义本就模糊（与系统内存共享），UI 上要按集显标注口径。 |

---

## 12. 运行期验证方法（可直接照做）

**前置**：本机**没有** `cl.exe` / `gcc.exe` / `clang.exe`（PATH 与 `C:\Program Files\Microsoft Visual Studio` 均无），但有 `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe`。所以临时探针用 C# 写、`csc /platform:x64` 编译最快；正式验证走仓库自己的 CMake 工具链。

**第 1 步（最重要，一次即可定案 `supported` 语义）**：把 **`supported` 原始整数**打印出来，而不是只打印非零项。若全部恰为 `1` → 纯布尔；若出现 `2`、`3`、非连续值 → 需重新评估。

```csharp
for (int i = 0; i < 256; i++)
    if (d.sensors[i].supported != 0)
        Console.WriteLine($"[{i}] supported={d.sensors[i].supported} value={d.sensors[i].value}");
```

**第 2 步**：取权威支持列表并与第 1 步对照。调 `ADL2_Adapter_PMLog_Support_Get(ctx, idx, &supportInfo)`，从 `usSensors[0]` 扫到 `usSensors[k] == 0`（`ADL_SENSOR_MAXTYPES`），得到 `k` 与具体 ID 集合。判读：列表里有但 `supported == 0` 的索引 ＝「驱动支持、本次无值」（对应寄存器不可读或该域当前未供电）；列表里没有的索引 ＝「驱动根本不支持」，UI 直接不显示。（LHM 就是拿 `k` 作为「本卡支持多少个传感器」，并据此在多条重复适配器里择优——见 `AmdGpuGroup.cs:91-105`、`:131-135`。）

**第 3 步：加载 GPU，看哪些索引动**（每 500 ms 采一次，空闲 → 满载 → 回落三段）：

| 索引 | 空闲 | 满载预期 | 判读 |
|---|---|---|---|
| 1 GFXCLK | 800 | **显著上升**（到 2000+ MHz 量级） | 动了即确认 |
| 19 ACTIVITY_GFX | 不支持 | 仍不支持 | **本机预期恒为 0**；若始终无值，改走 §10 的 `iGPUActivityPercent` |
| 21 GFX_VOLTAGE | 1235 | 上升 | 与 1 同向 |
| 30/31 GFX_POWER/CURRENT | 15–18 W / 12–14 A | 显著上升 | 与 1/21 同向，且满足 `P ≈ U/1000 × I` |
| 23 ASIC_POWER | 25 | 上升 | 若上升幅度 ≈ 30 的上升幅度 + CPU 变化，说明是封装级 |
| 8/28/29 温度 | 52/53 | 上升（有热惯性，滞后几秒） | 8 若突然变为有值 → 该卡其实支持 EDGE |
| 14/15 风扇 | 不支持 | 仍不支持 | 本机预期恒无；风扇走 EC（可另用 WMI/EC 读） |
| 34 CLK_CPUCLK | 3931 | GPU 满载时变化不大 | 作为「34 是 CPU 时钟而非 GPU 时钟」的判别：**GPU 满载时 34 不动而 1 动** |

**第 4 步：加载 CPU，反向验证**：单核/全核跑满 → `32` 温度上升、`33` 功耗上升、`34` 频率上升，而 `1`/`30`/`31` 基本不动。**这一步是区分 30（GFX 功耗）与 33（CPU 功耗）、以及 34（CPU 时钟）的唯一硬证据**，因为没有任何开源实现消费这几个 APU 字段（§10 置信度栏）。

**第 5 步：自洽性恒等式**（写进诊断，不写进 UI）：`|U_mV/1000 × I_A - P_W| / P_W < 0.2`。对 `(21,31,30)` 与 `(16,18,17)` 两组分别校验。本机空闲样本已通过（1.235×14 ≈ 17.3 vs 18 W；0.765×2 ≈ 1.53 vs 1 W）。若某次不满足，说明三者的采样时刻不同步（一次性 API 一次调用内应同步，共享内存路径是异步刷新），可据此判断所用的 API 路径。

**第 6 步：核对索引 2 的口径**：`Get-CimInstance Win32_PhysicalMemory | Select-Object Speed,ConfiguredClockSpeed,Capacity`。若为 `5600`，则 `2=2800` 恰是 DDR5-5600 的 I/O 时钟（MT/s ÷ 2），口径确认；若为 `7500`（LPDDR5X），则 `2800` 不是内存 I/O 时钟，需要重新解释（**这正是「名字已证实、口径待确认」的典型**）。

**第 7 步：版本相关的 52–74 打日志不显示**：把每个有值的 `52..74` 索引原样打印（索引 + 原始值），人工按两套命名各解释一次，看哪套更合理（例如值为 `37` 时：现代版解释为「边缘温度降频至 37%」，旧版为「GFX 温度降频至 37%」——两者都说得通，故**仅凭数值无法判定**，需与 §2 的 SDK vintage 线索合并判断）。

**第 8 步：降级链验证**：先调 `ADL2_Overdrive8_PMLog_ShareMemory_Support`（注意符号名拼写），若返回真则试 `ShareMemory_Start/Read/Stop` 并与一次性 API 的读数逐项比对；若两者一致，说明可以安全切到官方推荐路径。

---

## 13. 与任务书原文冲突的更正汇总

| # | 原文表述 | 更正 | 证据 |
|---|---|---|---|
| 1 | 「部分项目对 `supported` 做 AND 或测试特定位，需确定它是布尔还是位图」 | **是布尔**。三个独立消费方一律测非 0，无一处按位运算；AMD 官方文档未定义该字段。**真正的位图是索引 35 `ADL_PMLOG_THROTTLER_STATUS`**，位定义 `POWER=1<<0 / THERMAL=1<<1 / CURRENT=1<<2`。 | 本文 §3 |
| 2 | 「LibreHardwareMonitor 的 `Adl.cs` / `AdlSensor.cs`」 | **这两个文件在当前 LHM 中不存在**（`raw.githubusercontent.com` 直取 `LibreHardwareMonitorLib/Hardware/Gpu/Adl.cs` 返回 404）。AMD 后端是 `LibreHardwareMonitorLib/Hardware/Gpu/AmdGpu.cs` + `LibreHardwareMonitorLib/Interop/AtiAdlxx.cs`；`Adl.cs` 之名来自旧 OpenHardwareMonitor 树。 | `https://raw.githubusercontent.com/LibreHardwareMonitor/LibreHardwareMonitor/master/LibreHardwareMonitorLib/Hardware/Gpu/Adl.cs` → HTTP 404 |
| 3 | 「PMLog 可能只在独显可用」 | **集显可用**。门槛是 ASIC 家族 ≥ `FAMILY_AI`(141, Vega)，Phoenix/Rembrandt/Van Gogh 均在内；本机 780M 实测成功。但**可用传感器集合不同**（本机 8/14/15/19/27 不支持）。 | 本文 §7 |
| 4 | 「VRAM 用量是 PMLog 传感器之一」 | **不存在**该索引。显存用量走 `ADL2_Adapter_DedicatedVRAMUsage_Get`（MB），容量走 `ADL2_Adapter_MemoryInfoX4_Get`。 | 本文 §11 |
| 5 | 「`ADL2_New_QueryPMLogData_Get` 是现行推荐 API」 | **AMD 已正式弃用**，原文理由 `inefficient when getting GPU metrics`，替代为 `ADL2_Overdrive8_PMLog_ShareMemory_Read`。 | 本文 §6；<https://gpuopen-librariesandsdks.github.io/adl/group__OVERDRIVE8API.html> |
| 6 | 「温度值需要 ×0.001 换算」 | **PMLog 温度是整数 °C，因子 1.0**。×0.001 只适用于**旧** API 的 `ADLTemperature.iTemperature`（毫摄氏度）以及 `ADLPMActivity.iVddc`（mV）。 | 本文 §4 |
| 7 | 「`iVendorID = 0x03EA` 说明该适配器不是 AMD」 | **是 AMD**，`0x03EA = 1002₁₀`，即把 `0x1002` 的十六进制数字按十进制读回。本机取数成功正说明它与厂商判断无关。 | 本文 §9 |
| 8 | 「5 个适配器意味着系统里有 5 个 AMD GPU」 | **同一物理 GPU 的重复枚举**。按 `(BusNumber, DeviceNumber)` 去重，并跳过空 `UDID`。 | 本文 §9 |
| 9 | （隐含）「`supported` 可当作支持能力判断」 | 支持能力的权威来源是 `ADL2_Adapter_PMLog_Support_Get` 的 `usSensors[256]` 列表，与 `supported` 是**两个不同语义**：前者＝驱动是否有此传感器，后者＝本次采样是否有值。 | 本文 §3 |

---

## 14. 仍然只能靠探测的字段

1. **索引 3/4/5/6/7/36/37/38/39/40/41/42/43/44/45 的单位**——头文件没有单位注释。按命名与 1/2/3 类比可以假定 MHz/°C，但「假定」不得写进 schema 的 `unit` 字段；做法是：采集时保留原始整数 + 索引，UI 只显示有注释依据的字段；对 7/44 这类可能被用户关心的，用「与 1(已知 MHz) 同数量级」做运行期旁证后**人工**确认。
2. **索引 38/39 `SMART_POWERSHIFT_*` 的语义**（状态位？档位？百分比？）——**未找到可靠来源**。做法：只有在本机真的出现该索引时才记录并标注「语义未确认」，否则不采集。
3. **索引 52–74 的命名归属**（现代版 vs 旧版）——**未找到可靠来源**给出 `atiadlxx.dll` 版本 ↔ SDK vintage 对照。做法：只在诊断导出里原样打印（索引 + 值 + 两个候选名字），不进 UI。
4. **本机驱动下 `TEMPERATURE_EDGE(8)` / `HOTSPOT(27)` / `FAN_RPM(14)` / `INFO_ACTIVITY_GFX(19)` 是否可能在某些状态（外接显示器、独显模式、插电/电池）下变为可用**——**未找到可靠来源**。做法：每次启动都重新跑 `PMLog_Support_Get`，结果记入诊断；不要缓存跨启动。
5. **`ADL_ERR_NO_ADMINISTRATOR_PRIVILEGES(-23)` 的触发条件**——**未找到可靠来源**。做法：按 §8 单独报诊断；不要假设「提权后指标会更全」。
6. **索引 2 的口径**（内存 I/O 时钟？UCLK？SMU 上报的「有效频率」？）——名字已证实，口径未证实。做法：按 §12 第 6 步与 `Win32_PhysicalMemory.Speed` 对拍后再决定展示文案（「显存时钟」还是「内存 I/O 时钟」）。
7. **`ShareMemory_Start` 的 `iSampleRate` 单位**——官方文档自相矛盾（ms 与 second 两种说法同页并存）。做法：按 ms 传，并在诊断里记录实际刷新间隔（用 `ulLastUpdated` 或 `ulActiveSampleRate` 反推）。
8. **`supported` 是否可能取非 1 的非零值**——无任何来源讨论。做法：§12 第 1 步打印原始值；一旦出现非 1 值，本文件 §3 的结论需要重新评估。
9. **`ADL2_Overdrive8_PMLogSenorType_Support_Get` 在集显上的返回**——该函数正是官方给的「哪些传感器能读真实值」的权威答案，但在本机未实测。做法：作为 §12 第 8 步的一部分试调，成功则用它替代人工推断的支持列表。

---

## 附：对 C++ 采集器的字段建议

索引 0–51 与 73 可做成静态常量表（见 §1/§2），**采集层只保留原始整数 + 单位标签**，换算统一放展示层：

```cpp
// 只需一张静态表 + 一个运行期掩码，不要为每个传感器写分支
enum AdlPmLogSensor : int {
  kGfxClk = 1, kMemClk = 2, kSocClk = 3, kVcnClk = 7,
  kTempEdge = 8, kFanRpm = 14, kFanPct = 15,
  kSocVoltageMv = 16, kSocPowerW = 17, kSocCurrentA = 18,
  kActivityGfxPct = 19, kActivityMemPct = 20,
  kGfxVoltageMv = 21, kMemVoltageMv = 22, kAsicPowerW = 23,
  kTempHotspot = 27, kTempGfx = 28, kTempSoc = 29,
  kGfxPowerW = 30, kGfxCurrentA = 31,
  kTempCpu = 32, kCpuPowerW = 33, kCpuClk = 34,
  kThrottleStatus = 35, kBusSpeed = 40, kBusLanes = 41,
  kBoardPowerW = 73,
};

struct AdlPmLogSample {
  bool valid[256];       // = (sensors[i].supported != 0)  ← 布尔，不要按位与
  int  raw[256];         // 原始整数，单位见文档
  uint32_t supportedIds; // 由 PMLog_Support_Get 得到的权威支持集合（哈希集/位集）
  int  samplingPath;     // 0=oneshot(deprecated) 1=PMLogStart 2=ShareMemory
  // 诊断：一次调用返回的索引列表 + 原始 supported 值，便于日后核对本文结论
};
```

四项硬约束（均有证据，见对应章节）：**① `supported` 判非 0，不按位（§3）；② 电压原始值即 mV，不要无脑 ×0.001（§4）；③ 52–74 不解释、只记录（§2）；④ 所有 ADL 调用串行化（§8）。**
