# 笔记本 CPU/GPU 温度与风扇转速获取路径 — 证据综述

> 调研日期：2026-10（与项目文档同期）。所有结论标注证据等级：
> **✅VERIFIED** = 已读一手源码/文档 ｜ **⚠️INFERRED** = 由间接证据推断 ｜ **❌NOT FOUND** = 本机检索范围内不存在证据（不是"不存在"，是"没找到"）。
> 每条结论后附 URL 或 `repo:path:line`。

---

## 0. 结论速览

| 路径 | 免驱动 | 免管理员 | 笔记本温度 | 笔记本风扇转速 |
|---|---|---|---|---|
| ACPI 热区 `MSAcpi_ThermalZoneTemperature` | ✅ | ❌ 需提权 | ⚠️ 常为假值/唯一值 | ❌ 无 |
| ACPI 热区 PDH `Thermal Zone Information` | ✅ | ✅ 通常可读 | ⚠️ 同源，常有缺失 | ❌ 无 |
| EC 直接访问（0x62/0x66） | ❌ 需 Ring0（PawnIO） | ❌ | ✅ 需机型寄存器表 | ✅ 需机型寄存器表 |
| 厂商 WMI（HP/Dell/Lenovo/Acer/Gigabyte） | ✅ | ❌ 需提权 | ✅ | ✅（厂商接口各异） |
| 厂商 WMI（ASUS） | ✅ | ⚠️ 未验证 | ✅ | ⚠️ 仅百分比，非 RPM |
| 厂商 WMI（MSI） | ✅ | ⚠️ | ✅ 固件有 | ⚠️ 固件有，Windows 无验证实现 |
| Surface | ❌ | — | ❌ Windows 无接口 | ❌ Windows 无接口 |

**两条最重要的结论：**
1. **Windows 上不存在用户态可打开的 EC 设备对象。** `\Device\ACPIEC` 是内核设备对象；`IOCTL_ACPI_*` 明确标注 "NT Kernel Model Driver only"。EC 端口只能经 Ring0 端口 I/O 访问（PawnIO `LpcACPIEC.p`）。✅VERIFIED
2. **PawnIO 已是 2025 年后的业界标准 Ring0 通路**：LiberateHardwareMonitor v0.9.5 起（PR #1857，作者 namazso 本人，commit `eb5e1a20be996d4865170b13bab97af43d97f341`）用 PawnIO 替换 WinRing0；FanControl V238+ 亦同。WinRing0 已被 Defender 列为 `VulnerableDriver:WinNT/Winring0`（CVE-2020-14979），且 Microsoft 从 2026-10 起默认开启 Memory Integrity。✅VERIFIED

---

## 1. ACPI 热区

### 1.1 类与语义

- 命名空间/类：`root\WMI` → `MSAcpi_ThermalZoneTemperature`。
  GUID 来自两份独立 DDK 头文件：`{a1bc18c0-a7c8-11d1-bf3c-00a0-c90629100000}`
  （`ziglang/zig` `lib/libc/include/any-windows-any/ddk/wmidata.h:27`；`reactos/reactos` `sdk/include/ddk/wmidata.h:27`，同处定义 `MSAcpi_ThermalZoneTemperature_ThermalStamp_SIZE sizeof(ULONG)`）。✅
- `CurrentTemperature` 单位 = **十分之一开尔文**，换算 `°C = CurrentTemperature / 10 - 273.15`。✅ 多源一致，且 Activestate 配方显式编码 `15 °C → 15*10+2732 = 2882`，反证 `2732` = 0.0 °C 的物理含义（不是错误码）。
  实现样例：`Lifailon/rudocs` `PowerShell/Get-Temperature.psm1:9-10`；`devhut.net`（2026-08-27 更新）；`phpsysinfo/includes/mb/class.thermalzone.inc.php:36`（读 `InstanceName, CriticalTripPoint, CurrentTemperature`）；`oshi/.../MSAcpiThermalZoneTemperature.java:23`；`shirou/gopsutil` `sensors/sensors_windows.go:14`；`mickem/nscp` `modules/CheckSystem/check_temperature.hpp`（注释明写 "tenths of Kelvin"）。
- **❌NOT FOUND：微软官方文档。** 三个 Learn URL 均 404；未找到任何 Microsoft 页面描述该类的权限检查机制。仅有一份非微软镜像 `wutils.com/wmi/root/wmi/ms_409/msacpi_thermalzonetemperature/`（来源不可信，仅用于确认类名 + `ms_409` 限定符集）。

### 1.2 权限：需管理员（经验证，机制未知）

需求为管理员是**经验事实**，多个独立来源：
- glenn.delahoy.com 论坛（2021-06）："I decided to try MSAcpi_ThermalZoneTemperature instead, but found that it **requires admin permission**, so I still get an error due to '**Access Denied**'."
- `sebhildebrandt/systeminformation` issue #179："when running PowerShell **as admin**, I get output from wmic … **As a non-admin** … **access denied**"。
- `GuillaumeGomez/sysinfo` issue #596："On windows, you need to run sysinfo with Administrator priviledges"。
- `shm11C3/HardwareVisualizer` PR #1633："**Commonly requires administrator rights, or is not implemented by the firmware**"。
- ⚠️ **该类执行的权限检查具体是什么，❌NOT FOUND**。检索只返回远程 DCOM/命名空间 ACL 的通用文章，无源码佐证。

### 1.3 热区实例代表什么

**✅ 实际 InstanceName 格式为 `ACPI\ThermalZone\<ZONE>`**，证据：
- reddit r/PowerShell（2020-10-14）过滤条件 `InstanceName -eq "ACPI\ThermalZone\CPUZ_0"`；
- SO 72275347 用 `typeperf "\Thermal Zone Information*()\Temperature"`，实例形如 `TZ.EXTZ`（45.85 °C）；
- ASUS ROG 论坛："ACPI thermal zone **ACPI\ThermalZone\TZ00 TZ01**"；
- HP 论坛 8067486（2021-05-19）："In the MSAcpi_ThermalZoneTemperature, I can see 8 instance names"。

**`CPUZ` 与 `EXTZ` 已确认**；`_0` 后缀变体与 `TZ00` 数字变体并存。
**❌ `GFXZ` / `CHGZ` / `LOCZ` / `BATZ` 作为 Windows 实例名字面量：NOT FOUND** — 不要硬编码，只做前缀匹配 + 运行时枚举。
另有两条命名视角：`mickem/nscp` 注释说明 WMI 的 InstanceName 源自 ACPI 命名空间路径（形如 `\_TZ.THM`）；`fastfetch-cli/fastfetch` `src/detection/cpu/cpu_windows.c` 默认取 `\_TZ.CPUZ`。

### 1.4 可靠性现实核查（这是必须内建的判据）

| 现象 | 证据 |
|---|---|
| 返回 `2732` = 0.0 °C 常量 | Activestate 配方反推换算；devhut 同式。固件返回 0 即得 2732 |
| 全部热区同一个值 | SO 64045403："I'm getting **3 objects, only 1 of which is for the CPU**" |
| 类直接不支持 | MS Learn 存档帖 a56ecf6f：用户得到 "**Not Supported**" |
| 本机完全没有热区 | `Kinmanr-ops/DeskMon` README：`MSAcpi_ThermalZoneTemperature` → "Returns 'Not supported' on this board; **no ACPI thermal zones exist at all**" |
| PDH 计数器集整体缺失 | `HotCakeX/Harden-Windows-Security` issue #1173（AMD Ryzen 桌面）：`PdhCollectQueryData warm-up failed: 0x800007D5`；`prometheus-community/windows_exporter` issue #816 / #2391 同 |
| Citrix 官方说明 | uberAgent 文档："is stored in one of these WMI objects, or both: 1. MSAcpi_ThermalZoneTemperature 2. Win32_PerfFormattedData_Counters_ThermalZoneInformation" |

### 1.5 同数据的第二条路径（免驱动的关键）

- **`Win32_PerfFormattedData_Counters_ThermalZoneInformation`**（`root\CIMV2`），属性 **`Temperature`（单位：开尔文，不是十分之一开尔文）**、`PercentPassiveLimit`、`ThrottleReasons`；限定符 `AutoCook_RawClass='Win32_PerfRawData_Counters_ThermalZoneInformation'`、`RegistryKey='{383487a6-3676-4870-a4e7-d45b30c35629}'`。✅
  `HardwareVisualizer` PR #1633 的排序即为此：①`MSAcpi_ThermalZoneTemperature`（常需管理员或固件不实现）→ ② `Win32_PerfFormattedData_Counters_ThermalZoneInformation`（"usually readable"）。
- **PDH 直读**（免 COM）：计数器集 GUID `{52bc5412-dac2-449c-8bc2-96443888fe6b}`，注册表路径 `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib\_V2Providers\{383487a6-3676-4870-a4e7-d45b30c35629}\{52bc5412-...}`。
  流程：`PerfEnumerateCounterSetInstances` → `PerfOpenQueryHandle(nullptr, ...)` → `PerfAddCounters`（`.Size = sizeof(querySpec)`，`.CounterId = PERF_WILDCARD_COUNTER`，`.InstanceId = PERF_WILDCARD_COUNTER`）→ `PerfQueryCounterData` → `PERF_DATA_HEADER`/`PERF_COUNTER_HEADER`/`PERF_MULTI_COUNTERS`/`PERF_COUNTER_DATA`。
  **解码（两个 counter id 单位不同！）**：`case 0 /*Temperature*/ → value - 273`；`case 3 /*High Precision Temperature*/ → value / 10.0 - 273`。
  参考实现：`fastfetch-cli/fastfetch` `src/detection/cpu/cpu_windows.c`；DataDog PR #52090 亦"优先 High Precision Temperature，回退 Temperature"。✅
  ✅ 值得抄的诚实处理：fastfetch 在 value==0 时返回错误串 **"Temperature data is zero"**，而不是伪造 −273 °C。

### 1.6 是否存在用户态 API / 免管理员路径

- **无文档化的用户态 API。** `CallNtPowerInformation(GetSystemPowerInformation)` 不暴露温度。`IOCTL_ACPI_*` 头文件（`tpn/winsdk-10` `Include/10.0.16299.0/shared/acpiioct.h`）标注 `Environment: NT Kernel Model Driver only`。✅
- ❌ **NOT FOUND：任何免管理员读取 `MSAcpi_ThermalZoneTemperature` 的办法。**
- 免管理员的实际替代是 §1.5 的 PDH/`CIMV2` 路径 —— 但它是**另一个数据源**，可能整体不存在。

---

## 2. 嵌入式控制器（EC）直连

### 2.1 硬件与协议

- ACPI EC 设备 `ACPI\PNP0C09`，端口 **0x62（数据）/ 0x66（命令）**。✅
- 命令集（LHM `WindowsEmbeddedControllerIO.cs` 与 NBFC `EmbeddedControllerBase.cs` 两份独立实现完全一致，后者注释标注 "See ACPI specs ch.12.2 / ch.12.3"）：
  `Read=0x80 (RD_EC)`、`Write=0x81 (WR_EC)`、`BurstEnable=0x82 (BE_EC)`、`BurstDisable=0x83 (BD_EC)`、`Query=0x84 (QR_EC)`。
- 状态位：`OutputBufferFull=0x01`、`InputBufferFull=0x02`、`Command/Data=0x08`、`BurstMode=0x10`、`SciEventPending=0x20`、`SmiEventPending=0x40`。
- 一次字节读：等 IBF 清零 → `out 0x66,0x80` → 等 IBF 清零 → `out 0x62,reg` → 等 IBF 清零且 OBF 置位 → `in 0x62`。
- 字读 = 两次字节读（标准 EC RAM 窗口内为小端，reg+1 为高字节）—— **但厂商寄存器对可能大端**（见 §3 Gigabyte 的字节序陷阱）。
- 超时常量（可照抄）：LHM `FailuresBeforeSkip=20, MaxRetries=5, WaitSpins=50`（`Thread.Sleep(1)` 轮询）；NBFC `RWTimeout=500` 次自旋、`FailuresBeforeSkip=20`、`MaxRetries=5`。
  ⚠️ **NBFC 的 `ReadByte/ReadWord` 在彻底失败时返回 0，无错误信号** —— 这是 winperfmon 绝不能复制的反面模式（0 会被当成 0 RPM / 0 °C 显示）。
- LHM 的 ASUS 特例：`WaitRead()` 若 OBF 始终不置位，则改等 IBF 清零（"Testing on Z170 Pro Gaming shows IBF clears in 1-3ms when data is ready for it"）。✅
- **分 bank**：寄存器地址 = `(bank << 8) | reg`，切 bank = 先读后写寄存器 `0xFF`。LHM 按 bank 排序寄存器以减少切换，并在 `prevBank != 0` 时警告 "Concurrent access to the ACPI EC detected. Race condition possible."。✅

### 2.2 Windows 上有没有用户态 EC 设备？

**结论：没有。✅VERIFIED（阴性结论，多路交叉）**

- `\Device\ACPIEC` 是**内核设备对象**（Acpi.sys / ACPIEC.sys）：Siemens 官方文章分析事件日志 `\Device\ACPIEC: The hardware of the Embedded Controller (EC) has not responded within the time limit` 即为证。
- grep.app 全库检索 `ACPIEC` 只返回 macOS `AppleACPIEC` kext、FreeRTOS MCHP、OpenCore 噪声 —— **索引代码中不存在任何 Windows 上打开 `\\.\ACPIEC` 的先例**。
- `IOCTL_ACPI_*`（含 EC 读写/求值）在 DDK 头中标注仅内核模式可用。
- `Soberia/EmbeddedController`、`shubhampaul/EC-Access-Tool` 都是"实现 ACPI EC 协议"，即自己在端口 I/O 层之上重写协议，而非打开设备。
- `OpenDevicePartnership/ec-test-app`（"User mode CLI to call and evaluate ACPI functions to test EC interfaces"）目标是 ACPI 方法求值，⚠️ 不是裸端口 I/O。
- 反面佐证：`dzxpert/AsusAIO-Vulnrable` DeepWiki 指出 AsusSAIO64.sys "provides primitives for interacting with the PCI configuration space and the Embedded Controller (EC). These interfaces lack granular access control, allowing any process with SYSTEM privileges to enumerate…" —— 即厂商是靠自己的 SYSTEM 级驱动 + 设备对象做的，而这正是 winperfmon 要避免的。

### 2.3 PawnIO 的 EC 相关模块（全部 LGPL-2.1-or-later，同一许可证头）

模块仓库 `namazso/PawnIO.Modules` 顶层完整清单（✅ 已逐一列出）：
`AMDFamily0F.p` `AMDFamily10.p` `AMDFamily17.p` `AMDReset.p` `ARMMSR.p` `DellSMM.p` `Echo.p` `IntelMCHBAR.p` `IntelMSR.p` `IntelOOBMSM.p` `IntelPCHThermal.p` `IsaBridgeEC.p` `LedsValve.p` `LpcACPIEC.p` `LpcCrOSEC.p` `LpcIO.p` `Nvidia.p` `PcSpeaker.p` `RyzenSMU.p` `SmbusI801.p` `SmbusIntelSkylakeIMC.p` `SmbusNCT6793.p` `SmbusPIIX4.p` `ZhaoxinMSR.p`。

**`LpcACPIEC.p`**（笔记本 EC 的正解，全源码已读）：
- 导出 **`ioctl_pio_read`（1 入 / 1 出）**、**`ioctl_pio_write`（2 入 / 0 出）**。
- `is_port_allowed(port) { return port == 0x62 || port == 0x66; }` —— **只放行这两个端口**，其他返回 `STATUS_ACCESS_DENIED`。
- 文档注释强制要求：`@warning You should acquire the "\\BaseNamedObjects\\Access_EC" mutant before calling this`。
- 模块内 TODO 明写：寄存器级事务（IBF/OBF 轮询、RDEC/WREC、burst）**不在模块内实现，必须由用户态完成**。
- `main()` **不做架构检查**（DeepWiki 页 6 确认 `LpcACPIEC.p` 与 `LpcCrOSEC.p` 为全架构通用；而 `LpcIO.p`/`IsaBridgeEC.p`/`DellSMM.p` 仅 x64）。

**`LpcIO.p`**：`ioctl_select_slot`(1/0)、`ioctl_find_bars`(0/0)、`ioctl_pio_inb`(1/1)、`ioctl_pio_outb`(2/0)、`ioctl_superio_inb/inw/outb`；互斥体 `\BaseNamedObjects\Access_ISABUS.HTP.Method`。
**内含对笔记本的关键放行（原文）**：
```
// Allow fixed ports 0x25C and 0x25D for ASUS EC
// Please keep in mind that the official AsusSAIO driver holds a kernel MUTEX, which usermode can't possibly
// acquire, so it is recommended that you make sure that the official driver is unloaded when poking these.
if (port == 0x25C || port == 0x25D) return true;
```
→ **0x25C/0x25D = ASUS EC 的端口级通道**；且模块自己警告必须先卸载官方 AsusSAIO 驱动（其内核互斥体用户态拿不到）。

**`DellSMM.p`**：导出 **`ioctl_query_smm`（6 入 / 6 出，eax/ecx/edx/ebx/esi/edi 顺序）**。`dell_smm_call()` 内部 `cpu_set_affinity(0,…)` → `query_dell_smm` → 恢复亲和性；文档写明"接口只在 CPU 0 上定义，其他 CPU 上可能挂死或返回不可用结果"。`main()` 只有在固件对签名请求 `0xfea3` 或 `0xffa3` 回 `eax='DIAG' edx='DELL'` 时才加载（否则怕落到无关的 SMM 处理程序）。✅

**`IntelPCHThermal.p`**（2026，M-Control Contributors）：导出 `ioctl_read_raw`(0/2) 与 `ioctl_identity`(0/5)；两条路线 = 传统 PCI BAR0（偏移 0x00，**0.5 °C 分辨率 + −50 °C 偏移**）与固定 PWRM（`0xFE000000` + `0x1560`）；自检要求 PCI 命令寄存器 MEMORY 位（0x04 bit1）置位，否则 "would map fine and then return bus fill values as successful samples" —— **典型的伪造值陷阱，值得抄进自检逻辑**。

**`AMDFamily17.p`**：`ioctl_read_msr`(1/1)、`ioctl_write_msr`(2/0)、`ioctl_read_smn`(1/1)；SMN 需先拿 `\BaseNamedObjects\Access_PCI` 互斥体；要求 PCI `00:00.0` 厂商 ID == `0x1022` 且 CPU family 0x17–0x1A。允许读的 MSR 白名单见源码（`MSR_PWR_UNIT 0xC0010299`、`MSR_PKG_ENERGY_STAT 0xC001029B`、`MSR_K10_COFVID_STATUS 0xC0010071`、PSTATE `0xC0010061–0xC001006B`、SMCA bank `0xC0002000–0xC00023FF` 等）。

**客户端集成（PawnIO wiki "Using PawnIO Modules"，✅）**：
1. 注册表读 `InstallLocation`（`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO`），回退 `%ProgramFiles%\PawnIO`；
2. `LoadLibrary(InstallLocation + "PawnIOLib")`；
3. `pawnio_open(&h)` → `pawnio_load(h, blob, size)` → `pawnio_execute(h, "ioctl_xxx", in, inSize, out, outSize, &retSize)`。
   **`.p` 里 `DEFINE_IOCTL_SIZED(<name>, in, out)` 的函数名就是 `pawnio_execute` 的字符串**。
**许可证（对 winperfmon 极重要）**："Official and unrestricted binary editions: **Proprietary, however redistribution of installer is allowed**. Open source edition: **GPL 2 with exception (PawnIO), LGPL 2.1 (PawnIOLib)**… It is recommended that users are simply redirected to pawnio.eu… as the builds are interface-compatible."

### 2.4 安全协议（照做即可）

1. **`Global\Access_EC` 互斥体**。
   - 模块文档写 `\BaseNamedObjects\Access_EC`，LHM `Hardware/Mutexes.cs` 用 `Global\Access_EC` —— 同一对象的 NT 与 Win32 两种视图。
   - OmenMon `config.html`：`EcMutexTimeout` 默认 200 ms，"Only one application can be accessing the Embedded Controller at any given time, otherwise the results might be unpredictable."
   - LHM 超时 `WaitEc(10)`（10 ms）。
   - **ACL 配方（抄 SIV / Argus Monitor 页面里的 C 示例）**：`AllocateAndInitializeSid(SECURITY_WORLD_SID_AUTHORITY, 1, SECURITY_WORLD_RID, …)` → `AddAccessAllowedAce(acl, ACL_REVISION, MUTANT_ALL_ACCESS, sid)` → `CreateMutex(&sa, FALSE, name)`，失败则 `OpenMutex(READ_CONTROL|MUTANT_QUERY_STATE|SYNCHRONIZE, FALSE, name)`。LHM 亦为 WorldSid FullControl + `AbandonedMutexException` 视为成功、mutex 为 null 时 fail-open。
   - ⚠️ **陷阱：非提权进程可能静默拿到"会话本地"的 Access_EC 而非 Global 对象**（`bigelephantz-bot/WinMonitor2` commit `abbbfbc`：「The EC fell back to a session-local Access_EC when the Global name could not be [created]」）。必须校验对象确实在 `\BaseNamedObjects`。
2. **只用精确字节读**。`h4fnp/tpnsfand` `EC.md` 实测：一次 EC 字节事务 ≈ **1.5–2 ms**；多字节轮询在 0.5 s 周期下造成每 tick ≈500 次额外中断、uncore 被钉在 1200 而非 900 MHz、C-state 降级、**额外 ≈1 W**。该文还揭穿了一个流传的谬误："读一个 EC 寄存器要阻塞 230 ms" 实为 `od -jN -N1` 无法 seek 导致读了 0..N−1 全部字节（`cat`/`hexdump` 256 字节 ≈ 360 ms）。
3. **预期与 ACPI 驱动竞争**。`phaax/FanControl.AcerPredatorPH315` README："**Fan speed reads occasionally fail.** The ACPI driver and this plugin both arbitrate for the EC; when ACPI wins, the read times out."（其缓解：保留上次好值而不是掉到 0。）
4. **固件层地雷**：ThinkPad `TPACPI_FAN_NS` —— **对 0x93 的每一次写入都会把 0x94 复位为 255（风扇停转）**，包括重复写入相同值；`0x93` 回读恒为 `(written & 0x10) | 0x04`，只有 `0x04`/`0x14` 两种状态；`0x93=0x04` 时 EC 自行维护 0x94，主机写入不生效；S3 恢复后 EC 收回控制权。
   MSI 上游警告："Randomly poking the embedded controller interface can potentially cause damage to the machine"。
   PH315 插件警告："Writing the wrong byte to the wrong EC register can brick fans or melt your laptop."
5. **温度变化速率是控制回路硬约束**：tpnsfand 实测**包温在 1 秒内可上升 >40 °C**（X13 Yoga Gen 1：50→97 °C；L13 Yoga Gen 2：59→89 °C）。

### 2.5 已公开、可直接落地的机型寄存器表

| 平台 | 寄存器 | 来源 |
|---|---|---|
| HP Omen | `CPUT 0x57` CPU 温度、`GPTM 0xB7` GPU 温度、**`RPM1/RPM2 0xB0/0xB1` = 左风扇 RPM（双字节）、`RPM3/RPM4 0xB2/0xB3` = 右风扇 RPM**、`XGS1/XGS2 0x2E/0x2F` 风扇百分比回读、`HPCM 0x95` 性能模式、`OMCC 0x62` 手动风扇控制、`FTHM 0xF9` 阈值位 | OmenMon `Hardware\EcData.cs`（GPL-3.0），`cli.html` ✅ |
| ThinkPad（标准） | ACPI DSDT 寄存器 `HFSP`，EC 偏移 **0x2F**，8 bit，等级 0–7（+`0xFF` 脱开） | thinkwiki.org/wiki/How_to_control_fan_speed ✅ |
| ThinkPad（`TPACPI_FAN_NS` 非标准） | `0x93` 控制字节、`0x94` 转速命令、`0x95` 转速表；**`RPM ≈ 491520 / value`**（0x94、0x95 同式）；tacho=255 是饱和地板（= 1927 RPM，表示风扇未转） | `h4fnp/tpnsfand` `EC.md`（GPL-2.0-only）✅ |
| Acer Predator Helios 300 PH315-53 | CPU 风扇 = 寄存器 19 / `0x13`（字）、GPU 风扇 = 21 / `0x15`（字）、CPU 控制 `0x37`、GPU 控制 `0x3A` | `phaax/FanControl.AcerPredatorPH315` ✅（原始映射来自 NBFC `Acer Predator PH315-53.xml`） |
| Clevo | CPU/GPU1/GPU2 风扇 = `0xCE`/`0xD0`/`0xD1`；端口 `EC_SC 0x66` / `EC_DATA 0x62` / `IBF 1` | HWiNFO 论坛主题 6072；`SkyLandTW/ClevoECView` `fanctrl.c`；`tuxedocomputers/tuxedo-fan-control` ⚠️ |
| Gigabyte | 主板温度 = EC 寄存器 `0x62`；风扇模式位 = `0xD` bit7 | `tangalbert919/gigabyte-laptop-wmi` `aorus-laptop.c` ✅ |
| Framework | `EC_CMD_PWM_SET_FAN_TARGET_RPM = 0x21` | community.frame.work 主题 21930；PawnIO `LpcCrOSEC.p` ✅ |

⚠️ **Acer PH315 的诚实声明必须照抄**：CPU/GPU 风扇读数是 EC 原始字（0–6122），**不是真正的转速表读数**，"treat it as a 0..6122 scalar that tracks the real fan well enough to drive a graph (idle ≈ 0, full tilt ≈ 5000–6000). **FanControl labels it as RPM in the UI regardless.**"

---

## 3. 厂商 WMI 矩阵（核心章节）

格式：**厂商 / 命名空间+类 / 风扇 / 温度 / 权限 / 证据**。每行末尾标注 ✅已知可用 / ⚠️未验证 / ❌不存在。

### HP — ✅风扇 RPM 与温度**都有**

| 项 | 内容 |
|---|---|
| 命名空间+类 | `root\hp\InstrumentedBIOS` → **`HPBIOS_BIOSNumericSensor`** |
| 风扇 | ✅ **RPM**。`BaseUnits` ValueMap 含 **19 = RPM**；`SensorType` ValueMap 含 **5 = Tachometer**、**12 = Air Flow**；驱动 `scale_numeric_sensor()` 注释 "**Fan readings are in RPM units; others are in milliunits**" |
| 温度 | ✅ 名称模式：`"CPU Thermal Index"` / `"CPU Temperature"` / `"Chassis Thermal Index"` / `"System Ambient Temperature"`；驱动说明 "Some systems have sensors named 'X Thermal Index' in 'Other' units… readings were reliably −6 °C [off]" |
| 权限 | ❌需管理员（经验证据） |
| 证据 | `torvalds/linux` `drivers/hwmon/hp-wmi-sensors.c`（含 MOF：`[read] string Name/Description; uint32 SensorType; string OtherSensorType; uint32 OperationalStatus; uint32 Size; string PossibleStates[]; string CurrentState; uint32 BaseUnits; sint32 UnitModifier; uint32 CurrentReading; uint32 RateUnits`）；类 GUID `8F1F6435-9F42-42C8-BADC-0E9424F20C9A`；SO 45736193 答复 "My HP laptop has HP specific WMI objects that contains temperature in Celcius units and **fan RPM speed objects. Running this WMIC command in administrator mode** will retrieve them: `wmic /NAMESPACE:\\root\HP\InstrumentedBIOS path HPBIOS_BIOSNumericSensor get Name,CurrentReading`"；`rostman.eu`（2023-04-30）在 EliteDesk 800 G5 SFF 实测 |

补充：
- **两条独立通路**：除 `HPBIOS_BIOSNumericSensor` 外，HP 笔记本驱动 `drivers/platform/x86/hp/hp-wmi.c` 用方法号查询风扇：`HPWMI_FAN_COUNT_GET_QUERY 0x10`、**`HPWMI_FAN_SPEED_GET_QUERY 0x11`**、`HPWMI_FAN_SPEED_MAX_GET_QUERY 0x26`、`HPWMI_VICTUS_S_FAN_SPEED_GET_QUERY 0x2D`（命令类型 `HPWMI_GM = 0x20008`，magic `signature = 0x55434553`）。
  `hp_wmi_get_fan_speed(fan)`：输入 `char fan_data[4] = {fan,0,0,0}`，`fsh = fan_data[2]; fsl = fan_data[3]; return (fsh << 8) | fsl;` → **真 RPM，输出偏移 2..3 的大端 uint16**；`CPU_FAN 0` / `GPU_FAN 1`。
  ⚠️ **单位陷阱**：`hp_wmi_get_fan_speed_victus_s()` 返回 `fan_data[fan] * 100` —— Victus-S 系列返回的是"百转"，**不是 RPM**。
- **两代类名并存**：老机型用 **`HP_BIOSSensor`**（`root\hp\instrumentedbios`，deploymentbunny.com 2016-03-16）→ 必须同时探测两个名字。命名空间大小写不敏感。
- `HPBIOS_BIOSSettingInterface` 确实存在（`[abstract, singleton]`，`SetBIOSSetting([out] uint32 Return, [in] string Name, [in] string Value, [in, optional] string Password)`，`Return` ValueMap `0..6 = {Success, Not Supported, Unspecified Error, Timeout, Failed, Invalid Parameter, Access Denied}`），但**是 BIOS 设置不是传感器**。HP forum 8030384 观测到错误码 6（Access Denied）。
- HP 自家文档：HP CMI 白皮书 "sensors … including both numerical sensors such as **fan speed (rpm)**"、"surfaced as an enumeration in WMI"；HP Installation Support Pack PDF "The namespace for HP CMI is root\hp\instrumentedbios"；BCU 文档 "most commercial HP desktops, **notebooks**, and workstations"。
- 传感器缺失的判定：`OperationalStatus` 为 `NO_CONTACT(12)` / `LOST_COMMUNICATION(13)` = 传感器不存在。

### Dell — ✅真路径是 SMM（LegacyDiags），不是 `root\dcim\sysman`

| 项 | 内容 |
|---|---|
| 免驱动路径 | `root\dcim\sysman\diagnostics` → 类 **`LegacyDiags`** → 方法 **`Execute`** |
| 参数名（逐字） | `EaxLen`/`EaxVal`/`EbxLen`/`EbxVal`/`EcxLen`/`EcxVal`/`EdxLen`/`EdxVal`（uint32 长度 + `byte[]` 值） |
| 风扇 | ✅ **RPM**：`eax = 0x02a3`（GET_SPEED，`ebx` = 风扇号）→ `(eax & 0xffff) * fan_mult` |
| 温度 | ✅ `eax = 0x10a3`（GET_TEMP，`ebx` = 传感器号）→ `eax & 0xff` \(\degree\)C；10 个传感器，标签 `{"CPU","GPU","SODIMM","Other","Ambient","Other"}` |
| 权限 | ❌需管理员（FanControl 需以管理员运行） |
| 证据 | `MoCodesHeavy/FanControl.DellWmiPlugin`（MIT）README：插件之所以存在，是因为内置 Dell 插件走"老式 I/O 端口 SMM 接口"，而 **"Many recent Dell machines no longer answer on that interface and only accept the same SMM commands over ACPI-WMI (the `LegacyDiags` class in `root\dcim\sysman\diagnostics`)"**；自检命令 `Get-CimClass -Namespace root\dcim\sysman\diagnostics -ClassName LegacyDiags | Select CimClassName, CimClassMethods`（类须暴露 `Execute`）；实测 **Latitude 5540**（Off / ~2400 / ~4000 RPM）；"**No kernel driver is installed.** Everything goes through Dell's own WMI interface"；`Rem0o/FanControl.Releases` discussion #4198 中维护者 Rem0o（2026-10-01）确认 "Tested on a **Dell Pro Max 16**, works great!" ✅ |

SMM 寄存器协议（`torvalds/linux` `drivers/hwmon/dell-smm-hwmon.c`，✅ 全源码）：
- `I8K_SMM_GET_FAN 0x00a3`、**`I8K_SMM_GET_SPEED 0x02a3`**、`I8K_SMM_GET_FAN_TYPE 0x03a3`、`I8K_SMM_GET_NOM_SPEED 0x04a3`、**`I8K_SMM_GET_TEMP 0x10a3`**、`I8K_SMM_GET_TEMP_TYPE 0x11a3`、`I8K_SMM_SET_FAN 0x01a3`、`I8K_SMM_GET_DELL_SIG1 0xfea3`、`I8K_SMM_GET_DELL_SIG2 0xffa3`。
- 传统端口路径的汇编逐字为 `out %%al,$0xb2` 然后 `out %%al,$0x84`，**进位标志 = 错误**；经 `smp_call_on_cpu(0, …, true)` 执行 → **只能 CPU 0**。
- 风扇倍率：默认 30，若标称转速读数 > `I8K_FAN_RPM_THRESHOLD`(1000) 则自动降为 1（在遍历标称转速表时一次性完成）。
- **必须复刻的假值判定**：温度 `0x99`(153) 为瞬时假值 → `msleep(100)` 后重试一次；**`0xc1`(193) = GPU 关闭时的返回值** → 判为无数据；`temp > I8K_MAX_TEMP(127)` → `-ENODATA`；`dell_smm_call()` 判定"无应答"的规则是 `(regs->eax & 0xffff) == 0xffff || regs->eax == 输入 eax`。
- **两条路都要实现**：`i8k_init` 先试 DMI 白名单 + 传统签名，失败则注册 WMI 驱动；注释 "On modern machines, SMM communication happens over WMI, meaning the SMM handler might not react to legacy SMM calls."
- **WMI 后备**：GUID **`F1DDEE52-063C-4784-A11E-8A06684B9B01`**，方法 id **`0x1`**（`DELL_SMM_LEGACY_EXECUTE`）；载荷 `u32 wmi_payload[] = {sizeof(eax),eax,sizeof(ebx),ebx,sizeof(ecx),ecx,sizeof(edx),edx}`（32 字节，**只用 4 个寄存器，无 esi/edi**）；响应为 le32 长度前缀列表，每个条目 4 字节对齐（`offset = ALIGN(offset+ret,4)`）；`wmi_parse_register()` 拒绝 `reg_size == 0 || reg_size > 4`。
- **黑名单（必须照抄）**：`i8k_blacklist_fan_support_dmi_table`（Inspiron 7720、Vostro 3360、XPS13 9333、XPS 15 L502X —— "all fan related SMM functions … freeze kernel for about 500ms"）；`i8k_blacklist_fan_type_dmi_table`（Studio XPS 8000/8100、Inspiron 580、Inspiron 3505 —— 调用 GET_FAN_TYPE 后 "CPU fan speed randomly going up and down"）。
- 风扇控制（仅白名单机型）：两对命令 `{manual 0x30a3, auto 0x31a3}` / `{manual 0x34a3, auto 0x35a3}`。
- Windows 侧 PawnIO 等价值：**`DellSMM.p` `ioctl_query_smm`**（6 入 / 6 出，CPU 0 亲和性钉住，签名自守）。

`root\dcim\sysman` 与 `DCIM_Fan`：
- `DCIM_Fan` ✅ 确实存在：`windows-admins/ConfigMgr` `Hardware Inventory/Dell_OMCI_SMS_DEF.mof:396-397` = `Namespace("\\.\root\dcim\sysman") class DCIM_Fan : SMS_Class_Template`；`dell/omsdk` `omdrivers/iDRAC/MOF/DCIM_FanView.mof:8` = `class DCIM_FanView : DCIM_View`。
- **但它属于 Dell Command | Monitor（可安装软件）**，Dell FAQ kbdoc 000177297："Dell Command | Monitor uses \"root/dcim/sysman\" namespace"，支持类别含 "Cooling Devices(Fan)" 与 "Temperature"。**原厂笔记本上该命名空间不存在。**
- **`DCIM_TemperatureProbe`：❌ NOT FOUND**（grep.app 多次 429；Dell 手册页被 Akamai 拦截）。DCM 文档中可见的是 `DCIM_NumericSensor`、`DCIM_ThermalInformation`、`DCIM_Sensor`，无 `DCIM_TemperatureProbe` 的原文佐证。
- 其他需要驱动的 Dell 工具（**反面教材**）：`AaronKelley/DellFanCmd`（README 自己说 "superseded by DellFanManagement"）、`AaronKelley/DellFanManagement`（issue #14："Fan control cannot be manipulated on Dell laptop models released in 2021 or later"）、`michyprima/FanCtrl`（Windows 服务 + SMM I/O，"making use of the driver from Aaron Kelley"）、`xytoki/node-dell-fan`（"Signed driver file by AaronKelley/DellFanCmd"）。

### Lenovo — 三套体系，Legion 与非 Legion 都有 WMI，ThinkPad 没有

| 子系列 | 命名空间+类 | 风扇 | 温度 | 权限 | 证据 |
|---|---|---|---|---|---|
| Legion / 游戏本 | `root\WMI` → **`LENOVO_GAMEZONE_DATA`** | 无风扇字段（用下一行） | ✅ `GetCPUTemp`=方法 18、`GetGPUTemp`=19 | ❌需管理员 | `torvalds/linux` `Documentation/wmi/devices/lenovo-wmi-gamezone.rst`（GUID `{887B54E3-DDDC-4B2C-8B88-68A26A8835D0}`）✅ |
| Legion / 游戏本 | `root\WMI` → **`LENOVO_FAN_METHOD`** | ✅ **RPM 直出**：`Fan_GetCurrentFanSpeed(FanID)`，FanID 0=CPU 1=GPU；另有 `Fan_GetCurrentSensorTemperature`、`Fan_Get_FullSpeed`、`Fan_Set_Table` | （见 `Fan_GetCurrentSensorTemperature`） | ❌需管理员 | GUID `{92549549-4bde-4f06-ac04-ce8bf898dbaa}`；调用点 `LenovoLegionToolkit-Team/LenovoLegionToolkit` `LenovoLegionToolkit.Lib/System/Management/WMI.LenovoFanMethod.cs:14-27`；`johnfanv2/LenovoLegionLinux` `kernel_module/legion-laptop.c:2682,3823,3851`；`TIANLI0/THRM` `internal/laptopfan/laptopfan_windows_lenovo.go:8-20` ✅ |
| 非 Legion（"Other Mode"） | `root\WMI` → **`LENOVO_OTHER_METHOD`** | ✅ 经数据块 **`LENOVO_CAPABILITY_DATA_00`** 的 `fanX_input`（当前 RPM）/`fanX_target`（目标 RPM）/`fanX_div` | ⚠️ `LENOVO_CAPABILITY_DATA_01` 里的 `cpu_temp`/`gpu_temp` 是**功耗/热上限**，**不是温度读数** | ⚠️ | `Documentation/wmi/devices/lenovo-wmi-other.rst`；`LENOVO_OTHER_METHOD` GUID `{dc2a8805-3a8c-41ba-a6f7-092e0089cd3b}`（WmiMethodId 17 GetFeatureValue / 18 SetFeatureValue / 19 GetDataByCommand / 99 GetDataByPackage）；`LENOVO_CAPABILITY_DATA_00` GUID `{362a3afe-3d96-4665-8530-96dad5bb300e}`；`LENOVO_CAPABILITY_DATA_01` `{7a8f5407-cb67-4d6e-b547-39b3be018154}`；`LENOVO_CAPABILITY_DATA_02` `{bbf1f790-6c2f-422b-bc8c-4e7369c7f6ab}`；`LENOVO_FAN_TEST_DATA` `{b642801b-3d21-45de-90ae-6e86f164fb21}`（`NumOfFans`/`FanId[]`/`FanMaxSpeed[]`/`FanMinSpeed[]`）；LWN `https://lwn.net/Articles/1044308` ✅ 文档级 |
| ThinkPad | — | ❌ **NOT FOUND** | ❌ NOT FOUND | — | Lenovo 自己的 WMI 文档（docs.lenovocdrt.com / `download.lenovo.com/pccbbs/...`）只记录 `root\WMI` 下基类 **`Lenovo_BIOSElement`**（BIOS 设置）。`LENOVO_THERMAL_*`、`Lenovo_FanSpeed` **全库检索不存在** |

**Lenovo Other Mode 的成熟度**：LWN（2025-10-31）与 LKML patch series v8（2026-01-13）/ v10（2026-01-17）——**截至 2026-01 尚未进入 mainline**，驱动为 `lenovo-wmi-capdata` + `lenovo-wmi-other`。→ 标记为"文档已确认，实战样本少"。

**ThinkPad 的实际路径只能是 EC**：`aungkokomm/ThinkPad-Toolbox`（.NET 8 + "WinRing0/PawnIO"，"live CPU temperature, fan RPM"）、`stanso/TPFanWin`（Windows 服务 "directly interacts with the Embedded Controller (EC) via Port I/O"）、`ThinkPad-Forum/TPFanControl`（`fancontrol/portio.cpp` 包含 `TVicPort.h` → **第三方内核驱动**）。
⚠️ **型号家族坑**：thinkwiki.de/TPFanControl 明确列出**两套互不兼容的风扇等级编码** —— 一套是 "fan 0 to 7 (zero to high)"，另一套是 "fan **255 to 0** (zero to high)"（针对 Edge E520）。不可硬编码。

**权限**：`pjt222/fancontrol` 的 `tools/LenovoWmi.psm1:117-124` 注释："Checks if the current process belongs to the `Administrator` role … prerequisite for accessing the `root\WMI` namespace; otherwise, WMI queries return 'Access Denied'"。

### ASUS — ✅可用，但风扇只有百分比

| 项 | 内容 |
|---|---|
| 设备路径 | **`\\.\ATKACPI`**（`CreateFile(GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, OPEN_EXISTING)`） |
| IOCTL | **`0x0022240C`** |
| 方法帧 | 8 字节头 = `uint32 MethodID` + `uint32 argsLen`，后接参数；固定 16 字节输出缓冲 |
| 方法 ID | `DSTS = 0x53545344`（get）、`DEVS = 0x53564544`（set）、`INIT = 0x54494E49`、`WDOG = 0x474F4457` |
| 风扇设备 ID | `CPU_Fan = 0x00110013`、`GPU_Fan = 0x00110014`、`Mid_Fan = 0x00110031` |
| 风扇语义 | ⚠️ **百分比 0..100，不是 RPM**：`raw = DeviceGet(endpoint); fan = raw & 0xFFFF; if (fan > 120 || (fan == 0 && raw < 0)) fan = -1;` |
| 温度设备 ID | `Temp_CPU = 0x00120094`、`Temp_GPU = 0x00120097` |
| WMI 等价 | `root\wmi` → **`AsusAtkWmi_WMNB`**，方法 `DEVS`，参数 `Device_ID` / `Control_status`；事件类 `AsusAtkWmiEvent` |
| 端口级后备 | PawnIO `LpcIO.p` 固定端口 **0x25C/0x25D**（含 AsusSAIO 内核互斥体警告，见 §2.3） |
| 证据 | `seerge/g-helper` `app/AsusACPI.cs` ✅；`irusanov/ZenTimings` `Common/AsusWMI.cs`；`torvalds/linux` `drivers/platform/x86/asus-wmi.c`（`ASUS_CPU_FAN_DESC "cpu_fan"` 等） |
| 风扇曲线 | `DevsCPUFan = 0x00110022`、`DevsGPUFan = 0x00110023`、`DevsCPUFanCurve = 0x00110024`、`DevsGPUFanCurve = 0x00110025`、`DevsMidFanCurve = 0x00110032`、`FanHysteresis = 0x00110034` |

⚠️ g-helper 是否需要管理员：**未验证**（README 直接抓取 ECONNRESET，FAQ 页只讲热键与 ASUS Optimization Service）。
⚠️ ASUS **主板**的 WMI 传感器是完全独立的另一套（`drivers/hwmon/asus_wmi_sensors.c`，X370/X470/B450/X399），不要混用。

### MSI — 固件接口已确认，Windows 侧无 RPM 消费者

| 项 | 内容 |
|---|---|
| 命名空间+类 | `root\WMI` → **`MSI_ACPI`**，GUID **`{ABBC0F6E-8EA1-11d1-00A0-C90629100000}`** |
| 风扇 | ⚠️ **固件已确认有，Windows 无验证实现**：`Get_Fan` = WmiMethodId **17**，子功能 `0x00`，返回 "up to four 16-bit fan speed readings in big-endian format"，不支持的槽位硬编码 `0x0000`；**`RPM = 480000 / reading`**；读数为 0 即 RPM 为 0 |
| 温度 | ✅ `Get_Temperature` = 方法 13/14；`Get_Thermal` = 15/16 |
| 其他方法 | 1/2 Get/SetPackage、3/4 Get/Set_EC、5/6 BIOS、7/8 SMBUS、9/10 MasterBattery、11/12 SlaveBattery、19/20 Device、21/22 Power、23/24 Debug、25/26 AP、27/28 Data、29 Get_WMI |
| 强制约束 | **所有方法都需要 32 字节输入缓冲**（Windows `CreateByteField()` 的怪癖）；`in[0]` = 子功能，`out[0]` = 成功标志（`0x00` = 失败） |
| 线程安全 | ⚠️ 上游警告："The ACPI control method responsible for handling the WMI method calls is **not thread-safe**. This is a firmware bug that needs to be handled inside the driver itself." |
| **类可能不存在** | `kykc/MsiFanControl`（MIT）自带 `MsiWmiAcpiMof.dll` + `MsiWmiAcpiMof.reg`，README 写 "Using built-in **wmiacpi.sys** Windows driver with **MSI custom-made MOF** definitions"，且需一次性 `msifancontrol install` → **厂商 MOF 未注册时 `MSI_ACPI` 类根本不存在** |
| 证据 | `torvalds/linux` `Documentation/wmi/devices/msi-wmi-platform.rst` ✅（内含解码 MOF）；辅助类 GUID `{ABBC0F60-...}`（Package，16B）、`{ABBC0F63-...}`（Package_32，32B）；上游消费者 `BeardOverflow/msi-ec`；`BeardOverflow/msi-ec` issue #240 交叉确认："MsiFanControl project is a replacement app for 'WMI1' devices… made for Windows and utilizes **ACPI-WMI subsystem to Read/Write data to EC**" |
| **`micommon`** | ❌ **NOT FOUND**（不存在该类名） |

⚠️ **本机 `MsiFanControl status` 只设模式/曲线，不输出 RPM** —— 即 "Windows 上有人读出 MSI 风扇 RPM" 这一环**尚无公开实现**。winperfmon 若实现将是首批。

### Acer — ✅游戏本有 WMI 风扇/温度

| 项 | 内容 |
|---|---|
| 命名空间+类 | `root\WMI` → **`AcerGamingFunction`** |
| 风扇 | ✅ 已知可用（Nitro/Predator） |
| 温度 | ✅ |
| 证据 | `Paulrod20/p-helper`：`PredatorHelper/Core/Services/PerformanceModeService.cs:16-24` 与 `PredatorHelper/Core/Hardware/Class1.cs:7-15` = `new ManagementObjectSearcher(@"root\WMI", "SELECT * FROM AcerGamingFunction")`；`daeora/apge-control`："talks directly to the `AcerGamingFunction` WMI provider — the same firmware interface PredatorSense's backend service used"；`jsserve-org/vibed-acer-fan-control`（记录了 `gmInput` 编码）；`Rushil-Ekhande/Acer-Nitro-GPU-Fan-control`："Reads CPU and GPU temperatures, **fan RPM**, fan targets, power state, and Acer platform profile state" ✅ |
| 上游进展 | platform-driver-x86 补丁系列（W. Armin，2025-09-23）："platform/x86: acer-wmi: add fan speed monitoring — Support CPU and GPU fan speed monitoring through WMI for **Predator PHN16-71**"；配套补丁加 `SetGamingFanSpeed()` / `GetGamingFanSpeed()`；**`ACER_WMID_GET_GAMING_SYS_INFO_METHODID`** 命令 `0x0000` 返回支持传感器位图，命令 `0x0001` 读值 ⚠️ 文档级 |
| EC 后备（已验证可行） | `phaax/FanControl.AcerPredatorPH315`：经 **PawnIO `LpcACPIEC`** 直读 EC（PH315-53，寄存器见 §2.5），且**明确声明"不是真正的转速表读数"** |
| 其他型号 | `vicentetob/acer-aspire-fan-control`：Acer Aspire A515-57 "ships with the Nitro/Predator gaming fan control fully functional in its firmware" ⚠️ |
| 事件 GUID | `676AA15E-6A47-4D9F-A2CC-1E6D18D14026`（`acer-wmi.c:116`、`Linuwu-Sense` `src/linuwu_sense.c:118`、`facer.c:127` 三处一致） |

### Gigabyte — ✅已确认可用，但类需要 MOF 注册

| 项 | 内容 |
|---|---|
| 命名空间+类 | `root\WMI` → **`GB_WMIACPI_Set`**，GUID **`{ABBC0F75-8EA1-11d1-00A0-C90629100000}`** |
| 风扇 | ✅ **RPM**：方法 **`getRpm1`** / **`getRpm2`**（FanControl.GigabyteWMI 插件，实测 **GIGABYTE GAMING A16 CVH**，i7-13620H + RTX 5060 Laptop，BIOS FB05，Win11 Home，2026-08） |
| 温度 | ✅ 方法 **`getCpuTemp`** / **`getGpuTemp1`** |
| 自检 | `Get-WmiObject -Namespace "root\WMI" -Class "GB_WMIACPI_Set"` → 期望 `__RELPATH : GB_WMIACPI_Set.InstanceName="ACPI\\PNP0C14\\DCK_0"` |
| **类可能不存在** | `ejempty/aero-keyboard-fan`（MIT）："Gigabyte's 'Turbo' fan mode doesn't poke the Embedded Controller directly — it calls **ACPI-WMI** methods on the `root\WMI` class **`GB_WMIACPI_Set`**"；但该类"**only appears once a MOF is registered**" —— Gigabyte 自带私有 `acpimof.dll`，该仓库用一份干净房间的 `aero_fan.mof` + `mofcomp` 注册一次后 "persists in the WMI repository across reboots" ✅ |
| 设备状态 ID 接口 | `TEMP_CPU 0xE1`、`TEMP_GPU 0xE2`、**`FAN_CPU_RPM 0xE4`、`FAN_GPU_RPM 0xE5`**、`FAN_THREE_RPM 0xE8`（2023 AORUS 17）、`FAN_FOUR_RPM 0xE9`（2023 AORUS 17X）、`FAN_PWM 0x50`、`CPU_FAN_DUTY 0x46`、`GPU_FAN_DUTY 0x47`、`FAN_CUSTOM_MODE 0x67`、`FAN_FIXED_MODE 0x6A`、`FAN_AUTO_MODE 0x70`、`FAN_GAMING_MODE 0x71`、`FAN_SILENT_MODE 0x57`、`GPU_QBOOST 0x51` |
| **字节序陷阱** | `static u16 convert_fan_rpm(int val) { u16 fan_rpm = val; return rol16(fan_rpm, 8); }`，注释 "Reverses byte order of fan RPM … the embedded controller stores the value in big-endian while x86 is little-endian"；调用处 `if (!strcmp(DMI_PRODUCT_FAMILY,"GIGABYTE GAMING") || !strcmp(...,"GIGABYTE AERO")) *val = output; else *val = convert_fan_rpm(output);` → **新 GAMING/AERO 家族小端，老机型大端，取决于 DMI product family** |
| 证据 | `tangalbert919/gigabyte-laptop-wmi` `aorus-laptop.c`（GPL-2.0-or-later，(C) 2023 Albert Tang）✅；`justin-mecham/FanControl.GigabyteWMI`（DutyTable：`{57,68,80,91,103,114,125,137,148,160,171,183,194,206,217,229}`，"Gigabyte's EC expects raw hardware duty speeds rather than linear percentages"，98–100% → 229 → **7500 RPM**）✅ |
| 台式机是另一套 | `t-8ch/linux-gigabyte-wmi-driver`：GUID `DEADBEEF-2001-0000-00A0-C90629100000`、6 个温度传感器、命令 `TEMPERATURE_QUERY 0x125`；解码 `(s8)temp * 1000`，且 **`if (temp == 0) return -ENODEV;`（0 = 传感器不存在，不是 0 °C）** —— 与 winperfmon 的"绝不伪造"原则完全一致，值得照抄 ✅ |

### Clevo / Uniwill / Tongfang — ❌无 WMI，只有 EC

- 端口常量三处一致：`#define EC_SC 0x66`、`#define EC_DATA 0x62`、`#define IBF 1`（`SkyLandTW/ClevoECView` `fanctrl.c`、`SkyLandTW/clevo-indicator` `src/clevo-indicator.c`）。✅
- 风扇寄存器 **`0xCE` / `0xD0` / `0xD1`**（CPU / GPU1 / GPU2）—— HWiNFO 论坛主题 6072（2019-12-11，Clevo P157SM），与 `tuxedocomputers/tuxedo-fan-control` 互相印证 ⚠️。
- `F-19-F/PyECClevo`："access EC in python and control clevo fan both works on **windows** and linux"。
- `vindeckyy/fan-control` README（2026-07-17）："Fan tachometer (RPM) readouts where the backend exposes them, including the **Uniwill/Tongfang EC tach on Windows**"。
- **❌ 无任何厂商 WMI 类。**

### Microsoft Surface — ❌Windows 侧无接口

- Linux 侧机制完整：Surface Aggregator Module（SSAM，即其 EC）——`SSAM_SSH_TC_TMP` 命令 `0x02`(get)/`0x03`(set)，`SSAM_SSH_TC_FAN` 命令 `0x0e` 实例 `0x01`；`drivers/platform/surface/surface_platform_profile.c`（"Surface Platform Profile / Performance Mode driver for Surface System Aggregator Module (**thermal and fan subsystem**)"）✅。
- LKML PATCH v5（2024-01-31）"hwmon: add fan speed monitoring driver for Surface devices"：**"provides read only access to the fan speed… The fan speed is always regulated by the EC and cannot be influenced directly."** 且 "newer Microsoft Surface devices (Surface Book 3 and Pro 9) have thermal sensors connected via the **Surface Aggregator Module (the embedded controller)**"。
- **Windows 侧 `SurfaceThermalZone` / `SurfacePlatform` WMI 类：❌NOT FOUND。** SSAM 是 Windows 上无对应公开用户态接口的内核内 ACPI 传输（Surface Serial Hub over UART）。
- ⚠️ 一条 `premsaig/laptop-temperature-monitor` 声称 "Microsoft Surface Laptop (4/5/6) … reads CPU core temps directly from Surface UEFI ACPI firmware via `MSAcpi_ThermalZoneTemperature`"，但该仓库 `main` 分支 README **HTTP 404** —— **不可引用**。
- → **结论：Windows 上 Surface 风扇转速/温度按 N/A 处理。**

### Framework（CrOS EC）— ✅唯一有公开命令号的笔记本 EC

- `EC_CMD_PWM_SET_FAN_TARGET_RPM = 0x21`（community.frame.work 主题 21930 `fauxectool 0x21 dFA0`）。✅
- PawnIO `LpcCrOSEC.p`（两种传输：标准 LPC 包偏移、MEC/EMI 间接访问）+ LHM `LibreHardwareMonitorLib/PawnIo/LpcCrOS.cs`。

### 通用类 — 必须探测后丢弃

- **`Win32_Fan`（`root\CIMV2`）**：Microsoft Learn 定义 `[Dynamic, Provider("CIMWin32"), UUID("{464FFAB5-946F-11d2-AAE2-006008C78BC7}"), AMENDMENT] class Win32_Fan : CIM_Fan`。powershell.one 明确："When `Get-CimInstance` returns no result, then apparently no instances of class Win32_Fan exist. **This is normal behavior.** Either the class is not implemented on your system (may be deprecated or due to missing drivers…)"。社区共识（bytes.com、BigFix 论坛 4804）："most system vendors … do not provide a device driver that exposes these properties to WMI … the classes are in the repository but the implementation is mis[sing]"。
- **`Win32_TemperatureProbe`**：同命（DeskMon："Returns nothing — **almost never implemented on consumer desktops**"）。
- → 二者只能"探测到就丢弃"，**绝不能显示**。

---

## 4. 可借鉴的开源实现

| 项目 | 许可证 | 是否需驱动 | 机制 | EC/笔记本支持 |
|---|---|---|---|---|
| **LibreHardwareMonitor** | MPL-2.0 | 需 PawnIO（v0.9.5+） | `Hardware/Motherboard/Lpc/EC/WindowsEmbeddedControllerIO.cs` → `PawnIo/LpcACPIEC.cs` → `Resources/PawnIo/LpcACPIEC.bin` | **❌无笔记本厂商 EC**。`EC/EmbeddedController.cs` 的 `_boards` 表**全是台式 ASUS 主板**（ROG STRIX/CROSSHAIR/PRIME/TUF，Amd400/500/600/800、Intel100/300/370/400/600/700/800）+ Chromebook `FRANBMCP06` 类；非 Windows 直接 `return null` |
| **nwinfo** (a1ive/nwinfo) | Unlicense | 需 PawnIO | C 语言直读，不用 WMI | **❌完全无 EC**：`libnw/lpc/` 只有 `lpc.c/lpc_fintek.c/lpc_ite.c/lpc_nuvoton.c/lpc_winbond.c`（纯桌面 Super I/O）。README 自述 "Gathers information directly without relying on WMI"，致谢 PawnIO |
| **NBFC** (hirschmann/nbfc) | ⚠️未核实 | **需 WinRing0（已进 Defender 黑名单）** | `StagWare.Plugins.ECWindows/ECWindows.cs` → `HardwareMonitor.Instance` → `WaitIsaBusMutex` + `ReadIoPort`/`WriteIoPort`；协议在 `StagWare.Hardware.LPC/EmbeddedControllerBase.cs` | ✅**有**：653 份机型 XML 配置。**附带 `ec-probe.exe`**（wiki "EC-probing-tool"） |
| **FanControl** (Rem0o) | 闭源 | V238+ 内置 PawnIO 版 LHM | 插件体系 | ✅生态最全（Dell/Gigabyte/Acer 插件见下） |
| **g-helper** (seerge) | ⚠️未核实 | ❌免驱动 | `\\.\ATKACPI` + IOCTL `0x0022240C` | ✅ ASUS（风扇=百分比） |
| **TPFanControl** | ⚠️未核实 | **需 TVicPort 内核驱动** | `fancontrol/portio.cpp` 包含 `TVicPort.h` | ✅ ThinkPad（等级 0–7） |
| **DeskMon** (Kinmanr-ops) | MIT | ❌免驱动 | 直接传感器 → Afterburner `MAHMSharedMemory` → HWiNFO `Global\HWiNFO_SENS_SM2` → CoreTemp `CoreTempMappingObject` → `n/a` | 桌面为主，但**是本项目约束下最好的"实测现实"文档** |
| **OmenMon** | GPL-3.0 | **需 WinRing0** | HP EC 直读 + WMI BIOS | ✅ HP Omen，**寄存器表最全** |
| **TIANLI0/THRM** | ⚠️未核实 | ❌免驱动 | Go，跨厂商 Windows 笔记本风扇读取 | ✅ Lenovo Legion (+ ASUS) |
| **phaax/FanControl.AcerPredatorPH315** | MIT 类 | 需 PawnIO | EC 0x62/0x66 via `LpcACPIEC` | ✅ Acer PH315-53，**"笔记本 EC 遥测只用 PawnIO、无自定义驱动"的参考实现** |
| **kulvind3r/ECReader** | ⚠️ | 需 PawnIO | README 标题即 "EC Register Reader **based on PawnIO driver**" | ✅ 直接先例 |
| **aungkokomm/ThinkPad-Toolbox** | ⚠️ | WinRing0 / PawnIO | .NET 8 | ✅ ThinkPad |
| **stanso/TPFanWin** | ⚠️ | 端口 I/O | Windows 服务 `TPFanWinService` | ✅ ThinkPad |
| **AaronKelley/DellFanCmd / DellFanManagement** | ⚠️ | 需签名驱动 | SMM | ✅ Dell（2021 后机型受限，issue #14） |
| **michyprima/FanCtrl / xytoki/node-dell-fan** | ⚠️ | 需驱动 | SMM I/O | ✅ Dell |
| **Steve-Tech/CrOS_EC_Python** | ⚠️ | 3 种驱动可选 | "Loading a driver will likely require administrator permissions" | ✅ Framework |
| **Soberia/EmbeddedController**、**shubhampaul/EC-Access-Tool** | ⚠️ | 需端口 I/O 层 | 自行实现 ACPI EC 协议 | 通用 |

**NBFC 653 份机型的 OEM 覆盖统计（= 各厂商 EC 寄存器表被公开解出的程度）**：
HP **59**、ASUS ~47、Acer ~40、Lenovo **30**、ThinkPad **12**、Sony 6、Dell 5、Gigabyte 3、Toshiba 2、Xiaomi 1；
**Clevo 0、MSI 0、Surface 0、Huawei 0** —— 与"这些厂商走不同/未公开的 EC 协议"完全吻合。

**DeskMon 的实测结论（对 winperfmon 的约束最有价值的一段）**：
- "**Do not 'update' to LibreHardwareMonitorLib 0.9.4 or older.** Those versions embed **WinRing0** (CVE-2020-14979) — Microsoft Defender quarantines it, Vanguard and Easy Anti-Cheat block it. **0.9.5+ dropped it.**"
- "**Do not add `requireAdministrator` to `app.manifest`. It does not work.** Measured on AMD hardware: running elevated, Ryzen Tctl/Tdie still reads exactly `0`. Elevation buys nothing and costs the no-UAC property…" ⚠️ **注意：这条针对桌面 AMD 的 CPU 温度；对本项目的笔记本厂商 WMI 路径而言提权仍然是必需的（HP/Dell/Lenovo 均有明确证据）。**
- "**A sensor reading of exactly `0` is missing data, not a measurement.** Tctl and package power both report `0` when there is no ring-0 path, and Afterburner publishes `0` for sources it lists but cannot read. Rendering that as `0°` looks completely plausible and is completely wrong."
- 共享内存通道的细节：`MAHMSharedMemory`（32 字节头 `dwSignature 'MAHM'`、`dwNumEntries` × 1324 字节条目、`float data` 在 +1300，**按名字而非索引**定位条目，用头部 UTC 时间戳判活、丢弃 >15 s、拒绝 `FLT_MAX` 哨兵）；`Global\HWiNFO_SENS_SM2`（需要 **Pro** 版共享内存支持）；`CoreTempMappingObject`（免费）。以及 `NamedSection.Detach()` 规则："A Windows named section lives as long as any handle to it, so holding a dead one pins the name and a restarting publisher asking for a larger section gets handed the stale smaller one."
- PawnIO 的保留意见（记为他们的判断，非事实）："PawnIO's compatibility with **Memory Integrity**（本机已开启）and its standing with anti-cheat **are not publicly documented**, and installing it is still a kernel driver install."

---

## 5. 对 winperfmon 的设计建议

### 5.1 温度 — 分层回退链（可信度降序）

| # | 来源 | 前置条件 | 输出 | 失败判定 |
|---|---|---|---|---|
| T1 | **MSR/SMN via PawnIO**（`IntelMSR.p` / `AMDFamily17.p`）—— Intel `IA32_THERM_STATUS 0x19C` / `IA32_PACKAGE_THERM_STATUS 0x1B1`，TjMax 取 `MSR_TEMPERATURE_TARGET 0x1A2` bits[23:16]，`TjMax − DTS`；AMD SMN `THM_TCON_CUR_TMP 0x00059800` bits[31:21] × 0.125 | 用户已安装 PawnIO | CPU 温度（最可信） | 读数 0 / 未变更 → 丢弃（不要显示 0） |
| T2 | **GPU 温度**：NVML / ADLX / IGCL 用户态 DLL | 无 | ✅免驱动 | 库缺失 → 跳过 |
| T3 | **厂商 WMI**：HP `HPBIOS_BIOSNumericSensor`（`BaseUnits==2` 或名含 "Thermal Index"）、Dell `LegacyDiags` `eax=0x10a3`、Lenovo `LENOVO_GAMEZONE_DATA` 18/19、Gigabyte `getCpuTemp`/`getGpuTemp1`、Acer `AcerGamingFunction`、ASUS `DSTS 0x00120094/0x00120097` | v1 已提权 | 温度 + **来源标签** | 传感器缺失（HP `OperationalStatus` 12/13）、类不存在（MOF 未注册）→ 降级 |
| T4 | **ACPI 热区两层**：① `root\WMI` `MSAcpi_ThermalZoneTemperature`（**十分之一开尔文**，需提权）→ ② `root\CIMV2` `Win32_PerfFormattedData_Counters_ThermalZoneInformation`（**开尔文**，通常可读）/ PDH 计数器集 `{52bc5412-dac2-449c-8bc2-96443888fe6b}`，**counter id 3 优先于 0** | 无 | 温度（兜底） | 见 5.4 判据 |
| T5 | **外部监视器共享内存**：Afterburner `MAHMSharedMemory` → HWiNFO `Global\HWiNFO_SENS_SM2` → CoreTemp | 对方在跑 | 温度，**必须标注 `来源=外部程序`** | 时间戳 >15 s、`FLT_MAX`、名字不匹配 → 丢弃 |

### 5.2 风扇转速 — 分层回退链

| # | 来源 | 输出语义 | 前置条件 |
|---|---|---|---|
| F1 | **PawnIO EC**：`LpcACPIEC.p`（0x62/0x66）+ 机型寄存器表 | 真 RPM（若该表给出的是原始标量，**必须按标量呈现，不得标 RPM**） | 用户已装 PawnIO **且** 机型在表中 |
| F2 | **厂商 WMI 且确为 RPM**：HP 方法 `0x11`（大端 uint16，输出偏移 2..3）/ Victus-S `0x2D`（×100，**单位不同**）；Dell `LegacyDiags` `eax=0x02a3` × fan_mult（30 或 1）；Lenovo `LENOVO_FAN_METHOD.Fan_GetCurrentFanSpeed` / `CAPABILITY_DATA_00.fanX_input`；Gigabyte `getRpm1`/`getRpm2`（**字节序按 DMI product family 决定**） | 真 RPM | v1 提权 |
| F3 | **厂商 WMI 但只有百分比**：ASUS `DSTS 0x00110013/14/31` | **百分比，非 RPM** | 不做换算；UI 标为「转速百分比（厂商固件）」 |
| F4 | **GPU 风扇**：NVML percent / ADLX / IGCL | 百分比或 RPM | 免驱动 |
| F5 | **N/A** | — | 见 5.3 |

**EC 路径的硬性实现要求**（缺一不可）：
1. 先拿 `Global\Access_EC`（World ACL 创建，失败则 `OpenMutex`），超时 200 ms 级别；**校验对象在 `\BaseNamedObjects`**，绝不接受会话本地回退。
2. 只做精确字节读（1.5–2 ms/字节），轮询周期 ≥1 s，避免 0.5 s 级别的高频多字节扫描。
3. 读失败**保留上次好值并标记 stale**，不要归零。
4. 只读、不写（v1 完全不要碰 `0x93`/`0x94` 这类"写入即改变状态"的寄存器）。
5. 机型寄存器表走 `sensor-tables/<vendor>-<model>.json` + `[issues]` 收集流程（与 `docs/release-github.md:372` 的既有约定一致），字段至少含：EC 寄存器、宽度、字节序、公式、以及**"这是否是真转速表"**的显式标记。

### 5.3 诚实答案 = "本机不可用" 的情形（必须在 UI 明说原因）

| 情形 | 提示文案要点 |
|---|---|
| 未安装 PawnIO + 厂商无 WMI 类（多数 ThinkPad / Clevo / 白牌） | 「本机风扇转速需 PawnIO 驱动读取 EC，当前不可用」 |
| 笔记本温度：AMD 且未装 PawnIO，且无厂商 WMI | 「本机 CPU 温度需要内核态读取（PawnIO），当前不可用」——**不要显示 0** |
| 厂商类未注册（MSI `MSI_ACPI`、Gigabyte `GB_WMIACPI_Set`） | 「厂商 WMI 类未注册（需厂商软件运行一次）」 |
| ACPI 热区不存在 / 所有热区同值 / 恒为 2732 | 「本机固件未提供可用的 ACPI 热区」 |
| Surface 全部机型 | 「Windows 无 Surface 风扇/温度公开接口（SAM/EC 仅内核内部）」，永久 N/A |
| ASUS 风扇只有百分比 | 显示百分比并标注「厂商固件百分比，非转速」 |
| Acer PH315 这类原始标量 | 显示 0..6122 标量并标注「EC 原始值，非转速表读数」 |

### 5.4 跨切面规则

1. **探测-再信任**：任何来源在**显示前**必须通过三项检查 —— ① 值非 0（0 = 无数据）；② 同机多传感器不同值（或至少不与 `2732`/`273.15 K` 这种哨兵相等）；③ 加载后数值会变化。ACPI 热区尤其要先判定"是不是所有热区同一个常量"。
2. **三态 UI**（与 `docs/ui-settings.md:46` 对齐）：可用 / **本机不可用（附原因）** / 需要驱动（PawnIO）。
3. **互斥体卫生**：`Global\Access_EC`、`Global\Access_ISABUS.HTP.Method`、`Global\Access_PCI` 三把都要按 SIV 的 World-ACL 配方创建并 fail-closed；第三方合规参考（Argus Monitor 表）可作为"哪些软件会踩我们"的预期清单。
4. **Ring0 只走 PawnIO**：签名、未被黑名单、安装包允许再分发、且 2025-09 起已是 LHM/FanControl 的事实标准。建议在现有 `LpcIO.p`/`IntelMSR.p`/`AMDFamily17.p`/`RyzenSMU.p` 之外**补入 `LpcACPIEC.p`、`DellSMM.p`、`IntelPCHThermal.p`**（笔记本 EC、Dell SMM、Intel PCH 温度）。
5. **v2 服务拆分**：SYSTEM 服务持有 PawnIO 句柄与全部互斥体、负责 EC/SMM 访问；普通权限 UI 经 IPC 取数。**绝不允许 UI 进程静默退化为会话本地互斥体**（WinMonitor2 commit `abbbfbc` 的真实现成教训）。
6. **SMM 调用必须钉在 CPU 0**（Dell 接口只在 CPU 0 定义）；`DellSMM.p` 已在模块内做亲和性处理，若自行实现需复刻。
7. **MSI 的 ACPI 方法线程不安全** → 对 `MSI_ACPI` 的所有调用串行化（单线程 + 队列）。
8. **不要把"外部程序共享内存"与"本机传感器"混在一个数值列**；来源必须可见可排序。

### 5.5 v1 / v2 分工建议（最小可交付）

- **v1（提权 EXE，无 PawnIO）**：T2 + T3 + T4 + F2 + F3 + F4。可覆盖 HP、Dell（2021+ 全部走 `LegacyDiags`）、Lenovo Legion、Acer 游戏本、Gigabyte（若 MOF 已在）。**ThinkPad / Clevo / MSI 风扇在此版本大概率是"本机不可用"** —— 这是诚实结论，不是缺陷。
- **v1 + opt-in PawnIO**：解锁 T1（Intel/AMD 温度）、F1（EC 风扇，需机型表）、Dell SMM 直连，以及桌面 Super I/O（既有设计）。
- **v2（SYSTEM 服务 + 普通权限 UI）**：把 5.4 第 5 条落地；同时借服务身份解决厂商 WMI 的提权问题。

---

## 附：本简报的证据缺口（明确标注，勿当作结论）

- `MSAcpi_ThermalZoneTemperature` 的**权限检查机制**：❌NOT FOUND（只有经验事实）。
- 该类**任何 Microsoft 官方文档页**：❌NOT FOUND（三个 Learn URL 404）。
- `GFXZ` / `CHGZ` / `LOCZ` / `BATZ` 作为 Windows 实例名字面量：❌NOT FOUND（只确认 `CPUZ`、`EXTZ`、`TZ00/TZ01`、`CPUZ_0`）。
- `DCIM_TemperatureProbe`：❌NOT FOUND。
- Microsoft Surface 的 Windows WMI 类（`SurfaceThermalZone` 等）：❌NOT FOUND。
- 任何 Windows 程序从 MSI `MSI_ACPI` 读风扇 **RPM** 的实例：❌NOT FOUND（只有 Linux 侧公式）。
- g-helper 是否需要管理员：⚠️未验证。
- NBFC / g-helper / TPFanControl / OmenMon 的**许可证字段**：本轮未逐一核实（OmenMon 已确认为 GPL-3.0）。
- PawnIO 与 **Memory Integrity (HVCI)** 的兼容性：⚠️作者未公开说明，仅 DeskMon 的保留意见。
- NBFC `StagWare.Hardware/HardwareMonitor.cs` 的 WinRing0 包装内部实现：抓取多次超时，未读。
