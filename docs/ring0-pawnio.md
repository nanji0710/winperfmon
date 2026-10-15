# Ring0 通道（PawnIO）：为什么、怎么用、边界在哪

本文记录 winperfmon 唯一的 Ring0 通路。**结论先写**：CPU 的 MSR、AMD 的 SMN、主板 EC 端口
在 Windows 用户态都读不到，而项目铁律禁止自研内核驱动、注入与挂钩，所以这些指标只有两条路
——要么不做，要么借助一个**用户态可打开、可审计的通用 IO 驱动**。本项目选 PawnIO。

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
  `DisplayVersion`（比调用 VERSION 更省事）。
- 模块文件查找：`<exe目录>\modules\<名字>` → `<exe目录>\<名字>`；**只读不改写**，用官方发行版
  的原字节（签名因此保持有效）。

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
| 封装温度 | SMN `0x00059800`（THM_TCON_CUR_TMP） | `bits[31:21] / 8 − 49`（°C）；`bits[31:21] == 0` ⇒ 传感器未就绪，判读不到（与 ACPI 的 `2732` 同类陷阱） |
| SVI2 遥测有效平面 | SMN `0x0005A008`（TFN） | `bit0` 指示平面 0 落在 `+0xC` 还是 `+0x10`（不同代 SMU 相反），故两个偏移都试 |
| 核心电压 | SMN `0x0005A00C` / `0x0005A010` | `1.55 V − 6.25 mV × (raw & 0xFFFFFF)`，只接受落在 0.30–1.60 V 的解码值 |
| 每核频率 | MSR `0xC00000E8`（APERF）/ `0xC00000E7`（MPERF） | `基频 × ΔAPERF / ΔMPERF`；要求 `ΔMPERF ≠ 0` 且 `ΔAPERF ≤ 4 × ΔMPERF`（否则判为两次读数错位） |
| 封装功耗（兜底） | MSR `0xC001029B`（PKG_ENERGY_STAT）+ `0xC0010299`（PWR_UNIT） | 能量单位 `2^−ESU` 焦耳（ESU = `PWR_UNIT[12:8]`），`ΔE/Δt` 即瓦特 |

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

### 5.2 反伪造规则（与全项目一致）

1. 温度读数 0 ⇒ 未就绪，**不是 0 °C**；解码值不在 (0, 125] ⇒ 读不到。
2. 电压两个平面都解不出合理值 ⇒ 判读不到，绝不显示「最接近的那个」。
3. 功耗换算结果不在 0.3–250 W ⇒ 判读不到，并把原始 Δ、ESU、窗口写进 detail（便于定位单位口径问题）。
4. 每核频率：钉核失败（`SetThreadAffinityMask` 返回 0）就**不读**该核的 MSR——在没钉住的核上读
   APERF/MPERF 拿到的是别的核的数，那属于造假。
5. 所有读数失败一律 `Status::Unavailable` + 原因，不保留旧值冒充新值。

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

`--probe` 的 `[ring0]` 段是这条通道的自证：驱动版本、设备是否打开、模块是否载入、SMN 温度/电压
原始值与解码值、每核扫到的核数与最小/最大频率、以及失败原因；`[init]` 段里还会写一句
`cpu.ring0: 不可用（…）`，所有 `AvRing0` 指标的降级文案都指向它，避免出现"提示需要 Ring0
却不说卡在哪一步"。

## 7. 尚未做的部分（诚实清单）

- **风扇转速（EC 路径）未接**。`LpcACPIEC.bin` 已经在分发目录里，但**本机（HP ZHAN66 14 G8a）
  的 EC 风扇寄存器地址未知**：LHM 的 EC 映射表只有 Dell / MSI / ITE 通用几种，没有 HP ZHAN /
  ProBook G8 代。下一步的取证手段是读本机 DSDT 的 EC `OperationRegion` 字段名（例如
  `FANT`/`FAN1`/`RPM1`），拿到候选偏移后再用「探测-再信任」四条判据验证。
- **Super I/O 传感器未接**。本机是笔记本，通常没有独立的 Super I/O 芯片；`LpcIO.bin` 留给台式机。
- **Intel 电压未接**：各代 SVID 表口径不同，不猜。
- 写操作（EC 写、PWM 调速）**一律不做**：winperfmon 是监控工具，只读。
