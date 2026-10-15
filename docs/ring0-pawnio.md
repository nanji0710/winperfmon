# Ring0 通道（PawnIO）：为什么、怎么用、边界在哪

本文记录 winperfmon 唯一的 Ring0 通路。**结论先写**：CPU 的 MSR、AMD 的 SMN、主板 EC 端口
在 Windows 用户态都读不到，而项目铁律禁止自研内核驱动、注入与挂钩，所以这些指标只有两条路
——要么不做，要么借助一个**用户态可打开、可审计的通用 IO 驱动**。本项目选 PawnIO。

> **结论二（本机实测）**：Ring0 通道**必须以管理员身份运行** —— PawnIO 的设备对象 ACL 只授权
> SYSTEM 与 Administrators，交互式用户打开设备直接失败（错误 5 拒绝访问，根因见 §3.1）。
> 默认免驱路径（ADL2 + 原生 CPU 采样 + 惰性 ACPI）不受影响、无需提权。

> 适用范围：`docs/winperfmon-dev-plan.md` §3.3.1 的 T1/F1 两层，以及 `docs/config-schema.md`
> §4 里标注 `AvRing0` 的指标（每核频率、核心电压、每核温度、风扇转速、Super I/O 传感器）。

---

## 1. 为什么不能不要驱动

| 想读的东西 | 用户态可行？ | 依据 |
|---|---|---|
| MSR（APERF/MPERF、RAPL、温度目标） | ❌ | `RDMSR` 是特权指令，用户态执行即 `#GP` |
| AMD SMN（THM/TCON、SVI2 遥测） | ❌ | 经 PCI 索引/数据对间接访问，需 Ring0 |
| 主板 EC（风扇转速、部分温度） | ❌ | 端口 I/O 是特权操作；`\Device\ACPIEC` 是内核对象，`IOCTL_ACPI_*` 标 `Environment: NT Kernel Model Driver only`，全库检索没有用户态先例 |
| ACPI 热区温度 | ✅ | 走性能计数器（见 §3.3.1 的 T4），本项目已实现，免管理员 |

历史方案 **WinRing0** 被明确排除：CVE-2020-14979，且 Microsoft Defender 将其判为
`HackTool:Win32/Winring0`。

## 2. PawnIO 的定位与许可

- PawnIO 是 namazso 开发的通用 Ring0 执行框架：驱动只提供「按名调用已签名模块里的函数」这一
  单一动作，模块（`*.bin`）由驱动校验签名后载入。LibreHardwareMonitor 自 v0.9.5 起也从
  WinRing0 切换到它，属于 2025 年后的业界标准通路。
- **驱动本体是 GPL-2.0，但许可带明确例外**：允许与「仅通过设备 IOCTL 接口与 PawnIO 通信的
  独立模块」组合。winperfmon 只调用 `DeviceIoControl`，不链接 `PawnIOLib`、不含任何驱动代码，
  因此**本仓库整体仍是 MIT**。
- 模块（`AMDFamily17.bin` / `IntelMSR.bin` / `LpcACPIEC.bin` …）是 **LGPL-2.1**，因此以独立文件
  形式随包分发（`modules\` 目录，附 `COPYING`），不编入 `winperfmon.exe`。
- 安装包：官方 `PawnIO_setup_2.2.0.exe`（3,410,960 B，取自
  `github.com/namazso/PawnIO.Setup/releases`）。按 `README.md` §6.1 的决策③：**随包附带、由用户
  确认后调用官方安装器安装、允许拒绝**；winperfmon 绝不静默装驱动，也不会绕过 UAC。
- **安装形态（本机实测，PawnIO 2.2.0）**：服务 `PawnIO` = Running、启动类型 Manual；卸载注册表键
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO` 的 `DisplayVersion = 2.2.0.0`；
  驱动镜像落在 **DriverStore**
  （`\SystemRoot\System32\DriverStore\FileRepository\pawnio.inf_amd64_a72a2f969b8b7496\PawnIO.sys`），
  **不在** `C:\Windows\System32\drivers\` 下 —— 判定「是否已安装」不能只看后者。
- **安装器不带模块**：`C:\Program Files\PawnIO\` 只有 `PawnIOLib.dll`、`PawnIOLib.h`、
  `PawnIOUtil.exe`、`uninstall.exe`。官方模块 `.bin` **必须随我们的程序分发**，放在
  `<exe 目录>\modules\`（本机：`AMDFamily17.bin` 10652 B、`LpcACPIEC.bin` 2612 B、`COPYING` 27032 B）。

## 3. 用户态接口（`src/sensors/pawnio.{h,cpp}` 的全部依据）

设备与 IOCTL 常量取自官方头文件 `pawnio_um.h`：

```
k_device_type = 41394 (0xA1B2)；三个 IOCTL 都是 METHOD_BUFFERED + FILE_ANY_ACCESS
  ⇒ CTL_CODE 退化为 (type << 16) | (function << 2)
  LOAD_BINARY  fn 0x821 → 0xA1B22084      「载入模块字节」
  EXECUTE_FN   fn 0x841 → 0xA1B22104      「调用模块函数」
  VERSION      fn 0x861 → 0xA1B22184      「取驱动版本」
```

- 设备路径：先试 `\\?\GLOBALROOT\Device\PawnIO`，失败再试 `\\.\PawnIO`
  （`CreateFileW(GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, OPEN_EXISTING)`）。
- 调用格式：输入缓冲 = **32 字节 NUL 填充的 ASCII 函数名 + `uint64` 参数数组**，
  输出缓冲 = `uint64` 数组，`DeviceIoControl` 回填的字节数 ÷ 8 即输出个数。
- 驱动版本：注册表 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO` 的
  `DisplayVersion`（比调用 VERSION 更省事）。同键也是判定「是否已安装」的依据：本机驱动镜像只在
  DriverStore、没有 `System32\drivers\` 副本（见 §2），**不要拿 `System32\drivers\PawnIO.sys`
  是否存在当判据**。
- 模块文件查找：`<exe目录>\modules\<名字>` → `<exe目录>\<名字>`；**只读不改写**，用官方发行版
  的原字节（签名因此保持有效）。

### 3.1 必须提权：设备 ACL 的实测根因

以**非管理员**身份运行 `--probe`：头部是 `pawnio=present`、`ring0Backend=PawnIO`，但通道打不开 ——

```ini
ring0.unavailableReason=打不开 PawnIO 设备（错误 5（拒绝访问。））
```

根因在驱动的 INF。`C:\Windows\System32\DriverStore\FileRepository\pawnio.inf_amd64_a72a2f969b8b7496\pawnio.inf`
里有：

```ini
AddReg = Custom_Security
[Custom_Security]
HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)"
```

`D:P(A;;GA;;;SY)(A;;GA;;;BA)` 只给 **SYSTEM** 与 **Administrators** 完全访问（`GA`）——
设备对象 ACL 里**没有授予交互式用户的任何权限位**，所以用户态 `CreateFileW` 必然失败，与我们的
代码无关，也无法在用户态绕过（这不是"提示一下就过去"的权限，是设备对象的强制访问检查）。

因此：

- **Ring0 通道必须以管理员身份运行**；这与 `docs/winperfmon-dev-plan.md` §2 的 v1 形态
  「单进程 + `requireAdministrator`（Ring0 与 ETW 都需要）」一致。
- **默认免驱路径不受影响**：ADL2 PM 表 + 原生 CPU 采样 + 惰性 ACPI 热区都不碰 Ring0，
  无需提权（见 §6 与 `docs/build-and-run.md` §6）。

## 4. 用到的模块函数

| 模块 | 函数 | 入参 | 出参 |
|---|---|---|---|
| `AMDFamily17.bin` / `IntelMSR.bin` | `ioctl_read_msr` | MSR 索引 | MSR 值（64 位） |
| `AMDFamily17.bin` | `ioctl_read_smn` | SMN 偏移 | 32 位读数 |
| `LpcACPIEC.bin` | `ioctl_pio_read` | 端口 | 该端口读到的字节 |
| `LpcACPIEC.bin` | `ioctl_pio_write` | 端口、值 | 无 |
| `LpcIO.bin` | `ioctl_pio_read` / 芯片探测 | 端口 | 同上 |

`LpcACPIEC` 只放行端口 `0x62` / `0x66`（EC 的命令/数据口），并要求调用者先持有
`\BaseNamedObjects\Access_EC` 互斥体——**互斥与 IBF/OBF 握手都要用户态自己实现**，模块只给端口。

## 5. AMDFamily17 的寄存器常量（`src/sensors/cpu_ring0.cpp`）

| 用途 | 地址 | 解码 |
|---|---|---|
| 封装温度 | SMN `0x00059800`（THM_TCON_CUR_TMP） | `bits[31:21] / 8 − 49`（°C）；`bits[31:21] == 0` ⇒ 传感器未就绪，判读不到（与 ACPI 的 `2732` 同类陷阱）。**口径是 Tctl，不可与 ADL2 直接比较，见 §5.4** |
| SVI2 平面有效位（TFN） | SMN `0x0005A008`（`F17H_M01H_SVI` `0x5A000` + `0x8`） | `bit0` 置位 = 平面 0（Core）读不出，`bit1` 对应平面 1（SoC） |
| 核心电压 | SMN 平面偏移随型号（表见 §5.3） | `vid = (tel >> 16) & 0xFF`、`vcc = 1.550 V − 6.25 mV × vid`（**必须 8 位掩码**，低位是电流等其它数据），只接受落在 0.30–1.60 V 的解码值；`raw == 0` ⇒ 读不到（规则见 §5.2，实测见 §5.3） |
| 每核频率 | MSR `0xC00000E8`（APERF）/ `0xC00000E7`（MPERF） | `基频 × ΔAPERF / ΔMPERF`；要求 `ΔMPERF ≠ 0` 且 `ΔAPERF ≤ 4 × ΔMPERF`（否则判为两次读数错位） |
| 封装功耗（兜底） | MSR `0xC001029B`（PKG_ENERGY_STAT）+ `0xC0010299`（PWR_UNIT） | 能量单位 `2^−ESU` 焦耳（ESU = `PWR_UNIT[12:8]`），`ΔE/Δt` 即瓦特；本机 `PWR_UNIT = 0x000A1003` ⇒ ESU = 16 ⇒ **15.2588 µJ**/单位（见 §5.5） |

Intel 侧（`IntelMSR.bin`）：封装温度 = `TEMPERATURE_TARGET[23:16] − PACKAGE_THERM_STATUS[22:16]`；
封装功耗 = `RAPL PKG_ENERGY_STATUS(0x611)` 差分，ESU 取 `RAPL_POWER_UNIT(0x606)[12:8]`。

### 5.1 每核频率为什么"跨轮差分"而不是"轮内等待"

第一版写法是「钉到目标核 → 读 → `Sleep(3ms)` → 再读」。实测不可用：Windows 默认定时器粒度
15.6 ms，`Sleep(3)` 实际睡 15.6 ms，16 个核一轮就是 250 ms 白等。现在改为**跨轮差分**：
每轮只钉 4 个核、读 APERF/MPERF 后立刻换下一个核，用**上一轮同一核的读数**做差分。
窗口天然等于采样间隔（默认 500 ms），既不睡也不烧 CPU，读数还是这段时间的平均频率。
代价是每个核约 2 s（16 核）刷新一次，因此：

- `perCoreDetail[i]` 写明该值的年龄（`本轮` / `1.2 s 前` / `未采样`）；
- `cpu.freq.percore` 的值是「有值核的平均」，detail 里写明采样了几个核、本轮新采几个、最小/最大。

**首帧必然不可用**：APERF/MPERF 要两个采样点才能做差分，第一轮只建基线，因此首轮
`cpu.freq.percore` 的文案是「APERF/MPERF 尚无两个采样点（第一轮只建基线）；每轮钉 4 核轮换，
0.5 s 一轮」（提权实测，见 §7）。这是设计行为，不是故障。

### 5.2 反伪造规则（与全项目一致）

1. 温度读数 0 ⇒ 未就绪，**不是 0 °C**；解码值不在 (0, 125] ⇒ 读不到。
2. 电压：`raw == 0` ⇒ 直接判读不到（**`0` 不是 0 V，更不是 1.55 V** —— 按公式会算出 1.550 V 的
   假读数，见 §5.3）；两个平面都解不出落在 0.30–1.60 V 的值 ⇒ 判读不到，绝不显示「最接近的那个」。
3. 功耗换算结果不在 0.3–250 W ⇒ 判读不到，并把原始 Δ、ESU、窗口写进 detail（便于定位单位口径问题）。
4. 每核频率：钉核失败（`SetThreadAffinityMask` 返回 0）就**不读**该核的 MSR——在没钉住的核上读
   APERF/MPERF 拿到的是别的核的数，那属于造假。
5. 所有读数失败一律 `Status::Unavailable` + 原因，不保留旧值冒充新值。
6. SMN 封装温度是 **Tctl** 口径，`detail` 里必须写明它是 Tctl、不可与 ADL2 的 die/skin 读数
   直接比较（见 §5.4）。

### 5.3 SVI2 电压在本机不可用（实测）

`SVI0_TFN`（`0x5A008`）与两个遥测平面（`0x5A00C` / `0x5A010`）在一次 **20 s 的提权采样**
（其间含 **16 线程 CPU 满载**）中**整轮恒为 `0x00000000`** → `cpu.voltage` 如实判为「不可用」。

**`0` 不是 0 V，更不是 1.55 V。** 按权威解码 `vcc = 1.550 − 0.00625 × ((tel >> 16) & 0xFF)`，
`raw == 0` 会算出 **1.550 V** —— 那是假读数：此前正是这样把原始值 0 解码成了 1.550 V。
本机 SMU（Family 19h model 0x75 Hawk Point）根本不通过 SVI2 暴露电压，
`ring0.smn.voltageNote` 里记的就是这句原因原文。

解码权威口径来自 LibreHardwareMonitor `Amd17Cpu`：`vid = (tel >> 16) & 0xFF`、
`vcc = 1.550 − 0.00625 × vid`。**必须用 8 位掩码** —— 低位是电流等其它数据，用更宽的掩码
（例如 `raw & 0xFFFFFF`）会把它们算进电压。

平面偏移随型号（`F17H_M01H_SVI` = `0x5A000`；平面 0 = Core，平面 1 = SoC）：

| 型号 | 平面 0 | 平面 1 |
|---|---|---|
| model 0x31 | SVI + 0x14 | SVI + 0x10 |
| model 0x21、model 0x44 | SVI + 0x10 | SVI + 0xC |
| **default（含本机 model 0x75 Hawk Point）** | **SVI + 0xC**（`0x5A00C`） | **SVI + 0x10**（`0x5A010`） |

> 参考实现 LibreHardwareMonitor 的 `Amd17Cpu` 在 `raw == 0` 上**没有保护**，所以它在这类机器上
> 也会显示假的 1.55 V —— 我们的零值保护是**相对参考实现的实际改进**，不是"多加了一条规则"。

### 5.4 SMN 温度是 Tctl 口径，不能与 ADL2 直接比较

同一次 20 s 并发实验（提权，两条通道同时读）：

| 通道 | 同一时间窗的读数 |
|---|---|
| SMN `THM_TCON_CUR_TMP`（`0x59800`） | 随负载 66.75 °C → 峰值 **88.1 °C**（t ≈ 9 s）→ 衰减回 72.75 °C |
| ADL2 `TEMPERATURE_CPU`(s32) | 只有 **57–66 °C** |
| ACPI 热区 | 读数更低，且滞后约 **20 s** |

→ **两条 Ring0/免驱路径相差 10–20 °C**：SMN 给的是 **Tctl / 控制温度**，ADL2 PM 表给的是厂商
die/skin 口径。因此：

- ring0 给出的 `cpu.temp.package` 的 `detail` 里**必须写明这是 Tctl、不可与 ADL2 直接比较**；
- ADL2 在场时仍由 ADL2 优先（Ring0 只在 ADL2 缺席时兜底，见 §6）——这对用户更安全：
  显示一个比 die 高 10–20 °C 的 Tctl，只会被当成"机器很烫"。

### 5.5 MSR 封装能耗与 ADL2 互证

读 `MSR_PKG_ENERGY_STAT`（`0xC001029B`）做差分（乘能量单位 `2^−16 J = 15.2588 µJ`）：

| 负载 | MSR `PKG_ENERGY_STAT` 差分 | ADL2 `ASIC_POWER`(s23) |
|---|---|---|
| 空闲 | **18–25 W** | **21–24 W** |
| 满载 | **33–43 W** | **32–42 W** |

两条**互相独立**的路径（Ring0 MSR 与厂商 PM 表）结论一致 → 封装功耗可互证，量级可信。
本机 `PWR_UNIT`（`0xC0010299`）实测 `0x000A1003` ⇒ ESU = `[12:8]` = 16 ⇒ 单位 15.2588 µJ。

## 6. 与其它数据源的分工（谁让位给谁）

```
ADL2（AMD 免驱 PM 表）
  ├─ cpu.temp.package / cpu.power.package / cpu.freq.avg   ← 优先，Ring0 不抢（taken() 让位）
  └─ gpu.*
Ring0（PawnIO）
  ├─ cpu.voltage        ← 独有价值（ADL2 没有这一项）
  ├─ cpu.freq.percore   ← 独有价值
  └─ cpu.temp.package / cpu.power.package  ← 仅在 ADL2 缺席（Intel、或未装 atiadlxx.dll）时兜底
ACPI 热区
  └─ 只在上面两条都拿不到时出手（惰性建立 PDH 查询）
```

两点实测补充：

- **本机 `cpu.voltage` 不可用**（SVI2 的 TFN 与两个平面整轮恒为 0，见 §5.3）——
  「独有价值」指的是 ADL2 没有这一项，不等于「本机一定有值」。
- **Ring0 兜底的封装温度是 Tctl**，比 ADL2 高 10–20 °C（见 §5.4），所以 ADL2 在场时更不该让
  Ring0 抢先。

`--probe` 的 `[ring0]` 段是这条通道的自证：驱动版本、设备是否打开、模块是否载入、SMN 温度/电压
原始值与解码值、每核扫到的核数与最小/最大频率、以及失败原因；`[init]` 段里还会写一句
`cpu.ring0: 不可用（…）`，所有 `AvRing0` 指标的降级文案都指向它，避免出现"提示需要 Ring0
却不说卡在哪一步"。

## 7. 真机验收记录（PawnIO 2.2.0，管理员运行）

环境：HP ZHAN66 14 G8a / AMD Ryzen 7 H 255（Family 19h model 0x75 Hawk Point）/ Radeon 780M /
Windows 11 build 26300。

**`--probe` 的 `[ring0]` 段（关键行，逐字）**：

```ini
ring0.vendor=AMD
ring0.device=\\?\GLOBALROOT\Device\PawnIO
ring0.driverVersion=2.2.0.0
ring0.module=loaded
ring0.modulePath=D:\GitHub\winperfmon\build\modules\AMDFamily17.bin
ring0.smn.tempC=70.2
ring0.smn.tempRaw=0x774B0000
ring0.smn.coreVoltageV=n/a
ring0.smn.voltageRaw=0x00000000
ring0.smn.tfn=0x00000000
ring0.smn.voltageNote=SVI2 平面与 TFN 全为 0：本机 SMU 不通过 SVI2 暴露电压（实测 Family 19h model 0x75 Hawk Point 恒为 0），0 不是 0 V、更不是 1.55 V，按读不到处理
ring0.pkgPowerW=25.8
ring0.pwrUnitRaw=0x000A1003
ring0.perCore=16/16
ring0.perCoreMHzMin=3713
ring0.perCoreMHzMax=4529
```

- 温度自证：`tempRaw = 0x774B0000` ⇒ `bits[31:21] = 954` ⇒ `954 / 8 − 49 = 70.2 °C`（§5 的公式）。
- 能量单位自证：`pwrUnitRaw = 0x000A1003` ⇒ ESU = `[12:8]` = 16 ⇒ `2^−16 J = 15.2588 µJ`（§5.5）。
- **电压 `n/a` 是正确结果，不是缺陷**（见 §5.3）：本机 SMU 不通过 SVI2 暴露电压。

**`--all`（提权）汇总**：**可用 13 / 本机无 12 / 待接入通道 25 / 错误 0（合计 50）**。
（`docs/build-and-run.md` §6 那张免驱验收表记的是**未提权、未装 PawnIO** 的汇总 `可用 13 /
本机无 10 / 待接入通道 27`，两次口径不同，不要混用。）

**`cpu.freq.percore` 首帧按设计不可用**，文案为：「APERF/MPERF 尚无两个采样点（第一轮只建基线）；
每轮钉 4 核轮换，0.5 s 一轮」。

**性能与内存（`--watch 12`，提权、干净进程）**：

| 项 | 实测 |
|---|---|
| 采样次数 / 平均 / 最大 | **25 次**、**平均 1.66 ms/次**、**最大 3.34 ms/次** |
| 内存 | 工作集 **11.96 MB** / 私有 **2.47 MB** / 峰值 **12.05 MB** |
| 逐源平均耗时（ms） | `acpi-thermal 0.00`、`adl2 0.88`、`cpu.native 0.07`、`cpu.ring0 0.65`、`memory 0.02` |
| `--probe` | `startupMs = 43.4`、`stepAvgMs = 1.66` |

**单测**：**43 passed / 0 failed**（新增 8 个 Ring0 解码口径用例：SMN 温度往返与无效判定、
SVI 电压解码与越界、每核频率比值与坏差分、Intel 封装温度、能量单位）。

**三条口径结论**（本轮最重要的技术结论，详见 §5.3–§5.5）：① SVI2 电压在本机不可用，
`0` 不是 0 V、更不是 1.55 V；② SMN 温度是 Tctl、不可与 ADL2 直接比较（差 10–20 °C）；
③ MSR 封装能耗与 ADL2 `ASIC_POWER` 互证一致。

## 8. 尚未做的部分（诚实清单）

以下是 Ring0 通道**尚未接入**的部分（已完成并真机验收的见 §7）：

- **风扇转速（EC 路径）读通了，但寄存器地址仍未确定**。`LpcACPIEC.bin` 已经在分发目录里，且
  **只读读通**：`D:\GitHub\winperfmon-dsh-scratch\ecscan.cpp`（scratch 探针，未进仓库）用
  `ioctl_pio_read/ioctl_pio_write` 在用户态实现 0x66/0x62 的 IBF/OBF 握手，先取
  `Global\Access_EC` 互斥体，再逐寄存器读 0x00–0xFF：整表 256 个寄存器里只有 4–8 个读失败，
  说明通道可用。但**本机（HP ZHAN66 14 G8a）的 EC 风扇寄存器地址未知**：LHM 的 EC 映射表只有
  Dell / MSI / ITE 通用几种，没有 HP ZHAN / ProBook G8 代；HP 厂商 WMI 这条路也走不通 ——
  `root/hp/instrumentedBIOS` 命名空间存在，但 `HPBIOS_BIOSNumericSensor` 在管理员下查询返回
  **「不支持」**（该机型没有实现传感器类），所以 §3.3.1 的 F2 层在本机不可用。
  下一步的取证手段是读本机 DSDT 的 EC `OperationRegion` 字段名（例如 `FANT`/`FAN1`/`RPM1`），
  拿到候选偏移后再用「探测-再信任」四条判据验证。
- **EC 读取在满载下会变脆**（实测补充）：16 线程 CPU 满载时，同一批寄存器的等待超时明显增多
  （累计读失败从每轮 4–8 个涨到几十个）。所以真要接入 EC 时，等待预算必须给足，并且采样线程
  要提优先级——否则游戏满载时风扇值会比实际更早变成「读不到」。
- **Super I/O 传感器未接**。本机是笔记本，通常没有独立的 Super I/O 芯片；`LpcIO.bin` 留给台式机。
- **Intel 电压未接**：各代 SVID 表口径不同，不猜。
- 写操作（EC 写、PWM 调速）**一律不做**：winperfmon 是监控工具，只读。
