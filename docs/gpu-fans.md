# GPU 与风扇采集设计：NVML/ADLX/IGCL 与 Super I/O

> 本文由 winperfmon-dev-plan.md §3.2（GPU）与 §3.3（主板风扇转速）拆出并细化；上游冲突时以本文为准，并回头修正主方案。
> 关联：`architecture.md`（线程/采样预算）、`config-schema.md` §4（指标 ID 目录）、`cpu-intel.md`（同一套 provider 契约）。
> 术语：Ring0 通道 / 免驱模式 见 `README.md` §五。**芯片型号、寄存器、端口号、许可名必须与本文字面一致。**

## 0. 指标 ID 与命名归属

| 命名归属 | 指标 ID | 说明 |
|---|---|---|
| GPU 风扇归 `gpu.fan.*` | `gpu.fan.percent`、`gpu.fan.rpm` | **不要**写成 `fan.gpu.*` |
| 主板/CPU/机箱/水泵归 `fan.<槽位>.rpm` | `fan.cpu.rpm`、`fan.chassis1..6.rpm`、`fan.pump.rpm` | 槽位名固定为 `cpu` / `chassis1..6` / `pump` |

| 指标 ID | 默认名称 | 单位 | 精度 | 默认可见 | 来源（首选 → 降级） |
|---|---|---|---|---|---|
| `gpu.usage` | GPU | % | 0 | ✅ | NVML / ADLX / IGCL |
| `gpu.freq.core` | GPU 频率 | MHz | 0 | ✅ | 同上 |
| `gpu.freq.mem` | 显存频率 | MHz | 0 | ✅ | 同上 |
| `gpu.temp.core` | GPU 温度 | °C | 0 | ✅ | 同上 |
| `gpu.temp.hotspot` | 热点温度 | °C | 0 | ❌ | NVML / ADLX |
| `gpu.temp.mem` | 显存温度 | °C | 0 | ❌ | NVML `NVML_FI_DEV_MEMORY_TEMP`；ADLX |
| `gpu.power` | GPU 功耗 | W | 1 | ✅ | NVML `GetPowerUsage`(mW)；ADLX；IGCL |
| `gpu.fan.percent` | 显卡风扇 | % | 0 | ❌ | NVML（多为百分比）；ADLX/IGCL |
| `gpu.fan.rpm` | 显卡风扇 | RPM | 0 | ❌ | ADLX / IGCL（**NVIDIA 通常不提供 RPM**） |
| `gpu.vram.usage` | 显存占用 | % | 0 | ❌ | NVML / ADLX |
| `gpu.vram.used` | 显存已用 | GB | 1 | ❌ | 同上 |
| `gpu.throttle.reason` | 显卡降频原因 | — | — | ❌ | NVML `GetCurrentClocksThrottleReasons`；ADLX |
| `gpu.enc.usage` | 编码器 | % | 0 | ❌ | NVML / ADLX |
| `gpu.dec.usage` | 解码器 | % | 0 | ❌ | NVML / ADLX |
| `fan.cpu.rpm` | CPU 风扇 | RPM | 0 | ❌ | Super I/O（Nuvoton/ITE）→ 笔记本 EC |
| `fan.chassis1..6.rpm` | 机箱风扇 N | RPM | 0 | ❌ | 同上 |
| `fan.pump.rpm` | 水泵 | RPM | 0 | ❌ | 同上 |
| `mb.temp.vrm` | 供电温度 | °C | 0 | ❌ | Super I/O |
| `mb.temp.board` | 主板温度 | °C | 0 | ❌ | Super I/O |

## 1. GPU 路线（三家）

### 1.1 NVIDIA —— NVML

| 项 | 内容 |
|---|---|
| 库 | `nvml.dll` **随驱动安装，无需管理员** |
| 动态加载 | 运行期 `LoadLibrary` + 取符号（Rust 侧参考 `nvml-wrapper` 的 `libloading` 做法） |
| 频率 | `nvmlDeviceGetClockInfo(SM/MEM)` → `gpu.freq.core` / `gpu.freq.mem` |
| 功耗 | `nvmlDeviceGetPowerUsage`（**单位 mW**，显示前换算 W）、`nvmlDeviceGetPowerManagementLimit` |
| 温度 | `nvmlDeviceGetTemperature(GPU)`；`nvmlDeviceGetFieldValues(NVML_FI_DEV_MEMORY_TEMP …)` 取 hotspot / 显存 |
| 风扇 | `nvmlDeviceGetFanSpeed`（**%**，注意：NVIDIA 一般不提供 RPM，`gpu.fan.rpm` 在 NVIDIA 上应缺失而非换算） |
| 降频原因 | `nvmlDeviceGetCurrentClocksThrottleReasons`（功耗/热/PM 限制位） |
| 显存 | 显存占用（→ `gpu.vram.usage` / `gpu.vram.used`） |
| 采样 | 500 ms，单次 **5–50 µs**（`architecture.md` §6.1） |

### 1.2 AMD —— ADLX（旧卡降级 ADL2）

| 项 | 内容 |
|---|---|
| 接口 | **ADLX**（随驱动安装），核心接口 `IADLXGPUMetrics` |
| 能力探测 | **先用 `IADLXGPUMetricsSupport` 探测支持项**（不是每个型号/驱动都支持全部指标） |
| 可给指标 | GPU 使用率、GFX/显存频率、温度（**edge / hotspot / VRAM**）、风扇转速、功耗、电压、显存占用 |
| 手册 | `gpuopen.com/manuals/adlx` |
| 旧卡降级 | **ADL2** `ADL2_New_QueryPMLogData_Get`（`ADL_PMLOG_*`） |
| 采样 | 1 s，单次 **0.1–1 ms**；**初始化重** → 必须缓存 `IADLXPerformanceMonitoringServices` 实例，禁止每拍重建 |

### 1.3 Intel（Arc / Iris Xe）—— IGCL

| 项 | 内容 |
|---|---|
| 接口 | **IGCL**：`ctlPowerTelemetryGet`（功耗/频率/温度）、`ctlEnumTemperatureSensors`、`ctlFanGetSpeed` / `ctlFanGetConfig`、`ctlFrequencyGetState` |
| 可给指标 | 功耗、频率、温度、**风扇 RPM**、显存 |
| 备用 | **Level Zero Sysman**（`zesPowerGetEnergyCounter` 等） |
| 采样 | 与 ADLX 同档（1 s）；具体单次开销 **待核实**（源文档未给 IGCL 成本） |

## 2. 能力探测与缺失读数规则

| 步骤 | 做法 |
|---|---|
| 1. 探测 | 逐指标调用厂商的"支持查询"（ADLX 用 `IADLXGPUMetricsSupport`；NVML 用返回值判定；IGCL 用 `ctlEnum*`/`ctl*GetConfig`）。**探测结果缓存**，不每次采样重探 |
| 2. 缺失 | 硬件不暴露的项 → 按 `layout.hideMissing` 处理：`true` 整项隐藏，`false` 显示 `N/A` 且颜色降为 `colors.unit` |
| 3. 绝不 | **绝不填假值**（不填 0、不沿用上一拍的值、不按型号名猜）——沿用主方案「缺失读数规则」 |
| 4. 原因 | provider 只回缺失原因 id（`no_driver` / `vendor_unsupported` / `sensor_absent` / `value_invalid`），文案由 UI 映射（`cpu-intel.md` §8 同一套 id 体系） |
| 5. 异常值校验 | 温度 −40…125 °C、功耗 0…1000 W 之外 → 打标 `N/A(异常)` 并记一次诊断（主方案 §9） |

## 3. 多 GPU / 混合输出

| 情形 | 处理 |
|---|---|
| 枚举 | 枚举**全部** GPU 设备并标注 **iGPU / dGPU** |
| 默认选中 | 「**当前正在渲染的 GPU**」——优先用 **PresentMon 报告的 GPU**，其次用**使用率最高者** |
| 手动切换 | 允许用户手动切换目标 GPU（配置项见 `config-schema.md` §9 开放问题：当前 `Config` 无 GPU 选择字段，**待核实**是否需要新增 `gpu.selectedIndex`） |
| 热插拔 | 设备增减（外接显卡坞、切换独显直连）→ 重新枚举 + 状态通知（`architecture.md` §10） |
| 混合输出注意 | 笔记本内屏可能由 iGPU 呈现而 dGPU 在渲染：此时"渲染 GPU"的读数才是用户关心的，但**帧率来自 PresentMon**，两者可能指向不同 GPU —— 在 UI 上并列显示两台设备的标识，不合并 |

## 4. 风扇：通道与优先级

| 项 | 内容 |
|---|---|
| 原理 | Super I/O 芯片经 **LPC 总线**，端口 **`0x2E/0x2F` 或 `0x4E/0x4F`** 的 index/data 对访问 |
| 前提 | **必须 Ring0 端口 I/O**（经 PawnIO 的 `LpcIO.p` / `PortIO`；`WR0_ExecPawn` 范式见 `cpu-intel.md` §7） |
| 优先芯片（Nuvoton） | **NCT6776F / NCT6779D / NCT6798D / NCT6687D / NCT6799D**（chip ID 探测、bank select、FANIN 计数） |
| 优先芯片（ITE） | **IT8688E / IT8689E / IT8792E / IT8625E / IT8665E** |
| RPM 公式 | **RPM = 1350000 / (count × 分频)** |
| 笔记本 | 多数机型风扇走 **ACPI EC（端口 `0x62/0x66`）**，寄存器映射因 OEM 而异 → **优先级最低**，先只支持常见游戏本映射 |
| 不可读 | 显示 `N/A`，**不要伪造** |

## 5. ITE 寄存器映射要点（LHM `IT87XX.cs`，原样保留）

| 项 | 值 |
|---|---|
| 地址寄存器 | `base + 0x05` |
| 数据寄存器 | `base + 0x06` |
| VENDOR_ID | 寄存器 `0x58`，取值 **0x90 / 0x7F** |
| 温度基址 | `0x29`（**sbyte**） |
| 电压寄存器 | `{0x20..0x28, 0x2f, 0x2c, 0x2d, 0x2e}` × 增益（**0.012 / 0.011 / 0.0109 / 默认 0.016**） |
| 风扇计数低位 | `{0x0d, 0x0e, 0x0f, 0x80, 0x82, 0x4c}` |
| 风扇计数高位 | `{0x18, 0x19, 0x1a, 0x81, 0x83, 0x4d}` |
| 16 位使能 | `0x0C`（**bit4/5/2**） |
| 8 位分频 | `0x0B` |
| 16 位模式 | **RPM = 1.35e6 / (count × 2)** |
| 8 位模式 | **divisor = 1 << ((0x0B >> (3×i)) & 7)** |
| PWM 控制 | `0x15 / 0x16 / 0x17 / 0x7f / 0xa7 / 0xaf`（**bit7 = 自动模式，不可读**） |

## 6. Nuvoton 寄存器映射要点（LHM `Nct677X.cs`，约 71 KB 常数表）

| 项 | 值 |
|---|---|
| 进入序列 | `outb(0x2E, 0x87)` **连续两次** |
| 退出序列 | `outb(0x2E, 0xAA)` |
| 解锁寄存器 | `0x28` **清 bit4** |
| Chip-ID → 型号 | **`0xC3/0x30` = NCT6776F、`0xC5/0x60` = NCT6779D、`0xD4/0x2B` = NCT6798D、`0xD5/0x92` = NCT6687D、`0xD8/0x02` = NCT6799D…** |
| 访问流程 | **两个端口 `0x2E` 与 `0x4E` 都要探测**、**写后回读校验**、**写后 dummy read 冲刷总线**、**ISA 总线全局互斥**（`Mutexes.WaitIsaBus(10)`） |
| 共存 | 多进程同时访问 Super I/O 会互相破坏状态 —— **和 HWiNFO/AIDA 共存时必须处理**（互斥 + 失败重试，不要硬抢） |

## 7. 已知坑（原样保留，逐条进代码注释）

| # | 坑 |
|---|---|
| 1 | IT87xx 只能在 **`0x2E`** 进入、IT8792 在 **`0x4E`**；若 `0x4E` 回读不是 **`0xFFFF`**（说明已被别家进入），强行进入会让 **IT8792 谎报 chip ID `0x8883`** |
| 2 | **IT8688E** 写完地址寄存器后回读不匹配 —— **不能用回读校验，必须特判** |
| 3 | **IT8665E / IT8655E** 每次 update 前必须 **`SelectBank(0)`**，否则温度与转速乱跳；bank 选择在**休眠/恢复后失效** |
| 4 | 型号识别必须按主板特判：X870E Nova WiFi 上 "NCT6796D" 实为 **NCT5585D**、"NCT6799D" 实为 **NCT6796DS**；MSI B840/B850/B860/X870/Z890 上 "NCT6687D" 实为 **NCT6687DR** |
| 5 | **只认 chip ID 会读错** → 必须做「**chip ID + 主板特判表**」的数据驱动层 |
| 6 | 端口互斥缺失 → 与 HWiNFO/AIDA 同时运行时读数抖动或双方失效（见 §6 访问流程） |
| 7 | PWM 的 bit7 表示自动模式，此位**不可读**（不要把它当成转速/占空比数据） |

## 8. 常数来源与许可（红线）

| 来源 | 许可 | 可用方式 |
|---|---|---|
| **nwinfo**（`lpc_nuvoton.c` 等） | **Unlicense＝公共领域** | **可直接搬** |
| **LibreHardwareMonitor** `LibreHardwareMonitorLib/Hardware/Motherboard/Lpc/*.cs` | **MPL-2.0** | **文件级 copyleft**：移植的文件须保留 MPL 声明并保证源码可获取；**纯常数可自由使用，但别照抄文件结构** |
| **siomon**（Rust） | MIT | 交叉校验 NCT67xx / ITE87xx 常数 |
| **VirtualSMC / SMCSuperIO**（C++） | — | 再一份交叉校验表 |
| PawnIO（驱动／官方安装包） | **GPL-2.0** | 运行期 IOCTL 调用，不链接源码；随包分发官方安装包须履行 GPL-2.0 义务（`release-github.md` §3.5） |
| PawnIO.Modules（`.bin` 模块） | **LGPL-2.1**（GitHub REST API `license.spdx_id = LGPL-2.1`；仓库 `COPYING` 记 LGPL-2.1-or-later） | 若随包提供模块二进制：随附许可证与源码获取说明、保留声明、允许用户替换模块（同上 §3.5） |
| 关于页 | — | 统一列出所有第三方项目的名称、许可证与源码链接（主方案 §13.3 第 6 条） |

## 9. 数据驱动层（把维护成本从"发版"降为"换数据包"）

主方案 §11 指定：把「**PawnIO 模块清单 + Super I/O 芯片特判表 + AMD SMU 偏移表**」做成**可热更新的数据包**，不重新编译就能支持新主板/新型号。

| 数据包内容 | 形式（设计） | 加载约束 |
|---|---|---|
| 芯片特判表 | 主板标识（`Manufacturer + Product`）→ 实际芯片型号 + 寄存器基址覆盖 | 数据包版本号 + 校验；不合法则回退内置表 |
| 风扇槽位映射 | 芯片 → 槽位（`fan.cpu.rpm` / `chassis1..6` / `pump`）的编号映射 | 槽位名必须与 `config-schema.md` §4 一致 |
| 温度/电压换算 | 增益与偏移（ITE 电压增益等） | 与内置表同构，避免两套代码路径 |
| 数据包签名 | 需签名/校验和，防止被篡改后写入 Ring0 端口 | 具体格式 **待核实**（源文档未定义） |

## 10. 采样与预算

| 采样项 | 周期 | 单次成本预算 | 备注 |
|---|---|---|---|
| NVML | 500 ms | 5–50 µs/次 | `architecture.md` §6.1 |
| ADLX | 1 s | 0.1–1 ms | 初始化重，缓存 `IADLXPerformanceMonitoringServices` |
| IGCL / Level Zero | 1 s | **待核实** | 与 ADLX 同档假设，需实测 |
| SuperIO | 1 s | 10–100 µs | Ring0 端口 I/O，含互斥等待 |
| GPU 风扇 RPM/% | 跟随所属路线 | — | NVIDIA 只有 % |
| 采样周期与配置 | `update.sensorIntervalMs`（200–5000 ms，默认 500） | — | ADLX/SuperIO「1 s」= 默认档的两倍（`architecture.md` §6.2） |

## 11. 降级矩阵

| 能力 | ① 有 Ring0 | ② 免驱模式 | ③ 反作弊受限（GPU-only + FPS） |
|---|---|---|---|
| GPU 全部指标 | NVML/ADLX/IGCL ✅ | NVML/ADLX/IGCL ✅（**不需要 Ring0**） | ✅ |
| `gpu.fan.percent` | 厂商 API ✅ | ✅ | ✅ |
| `gpu.fan.rpm` | 厂商 API（NVIDIA 通常缺失） | 同左 | 同左 |
| `fan.*.rpm`（主板风扇） | Super I/O ✅ | **N/A**（需要 Ring0） | **N/A** |
| `mb.temp.*` | Super I/O ✅ | **N/A** | **N/A** |
| PresentMon 遥测备用 | 服务同时提供自己聚合的 GPU 温度/功耗/占用（走 NVAPI 等） | 同左 | 同左 |

## 12. 验收与自测要点

| 项 | 标准 | 方法 |
|---|---|---|
| GPU 数值一致性（M0） | 与 HWiNFO 对拍 **±1 °C / ±50 MHz / ±5% 功耗** | 双显卡机器（M0 DoD） |
| 三家覆盖 | NVIDIA（NVML）+ AMD（ADLX）+ Intel（IGCL） | 每种卡至少一台机器；无卡环境用能力探测的 mock |
| 缺失规则 | 不出现 0 冒充读数 | 断驱动/无传感器场景回归：只允许隐藏或 `N/A` |
| 多 GPU | 正确标注 iGPU/dGPU 且默认选中渲染 GPU | 笔记本混合输出 + 台式双卡 |
| 风扇 RPM | 与主板 BIOS 读数一致（**RPM = 1350000 / (count × 分频)**） | 已知芯片主板（NCT6779D / IT8688E）+ 与 HWiNFO 对拍 |
| 共存 | 与 HWiNFO/AIDA 同时运行不互相破坏 | 两者同时开启，观察 10 min 无抖动（§6 互斥） |

## 13. 现实提醒（本项目最大的工程风险）

- **Windows 上不存在独立的 Super I/O 库**：只有 Linux 的 `it87` / `nct6775` 与 macOS 实现；**LHM 也没有官方 C++/Rust 移植**，现存项目（LHM-CppExport、lhm-service）全是"包一层 .NET"的桥。
- 因此 Super I/O 这块**没有整套现成代码可抄，必须自建并随新主板持续维护** —— 这是工期与返工的主要来源（`roadmap.md` 风险表）。
- 缓解措施共三条：①优先覆盖 Nuvoton/ITE 主流芯片；②未识别时显示 `N/A` 并**上报芯片 ID** 供后续支持（主方案 §9）；③§9 的数据驱动层把新增主板支持变成"换数据包"。
