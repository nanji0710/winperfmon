# M0 骨架 + M1 免驱遥测 + M1 Ring0：构建 / 运行 / 验收

本文件记录 **M0 骨架、M1 免驱遥测与 M1 Ring0 通道**的**可复现构建与验收步骤**，供本机复核与 CI 复用。
设计依据：`docs/winperfmon-dev-plan.md` §8 里程碑 M0/M1、§3.3.1 温度降级链（T0/T1/T4）、§4 Ring0 访问与驱动方案（**必须提权**）、§5 技术栈选型；
实测证据：`docs/amd-facts.md` §9（ADL2 免驱 PM 表 + perflib 归因）、§1–§8；Ring0 侧证据：`docs/ring0-pawnio.md` §3.1、§5.3–§5.5、§7；性能分段与内存阶梯见 `docs/winperfmon-dev-plan.md` §1、§6。

## 1. 工具链

M0 使用**便携工具链**（免管理员，不依赖 Visual Studio）：

| 组件 | 版本 | 路径（默认） |
|---|---|---|
| MinGW-w64 GCC | 16.2.0（UCRT / posix / SEH，WinLibs） | `<WPM_TC>\mingw\mingw64\bin` |
| CMake | 4.4.x | `<WPM_TC>\cmake\bin` |
| Ninja | 1.13.x | `<WPM_TC>\ninja` |

`WPM_TC` 默认是 `D:\GitHub\winperfmon-toolchain`，可用环境变量覆盖。

> 设计目标是 MSVC + `/MT` 静态 CRT（`docs/winperfmon-dev-plan.md` §5）。
> M0 期间本机没有 MSVC（装 VS Build Tools 需要管理员），因此先用 GCC 跑通链路；
> `CMakeLists.txt` 里 MSVC 分支（`/W4 /permissive- /utf-8 /EHsc /GS`）已经就位，
> 换到 MSVC 时无需改源码。

## 2. 构建

```cmd
call tools\env.cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物：

| 文件 | 说明 |
|---|---|
| `build\winperfmon.exe` | 主程序（M0 是控制台探针） |
| `build\wpm_tests.exe` | 单元测试（零第三方框架） |

体积守卫（`docs/release-github.md` §6）要求 EXE ≤ 5 MB；当前约 1.35 MB。

## 3. 运行

```cmd
build\winperfmon.exe --once       :: 采集一次，打印全部 50 项指标目录
build\winperfmon.exe --all        :: 含默认隐藏项（M0 的 --once 已等价于此）
build\winperfmon.exe --watch 10   :: 连续采样 10 秒
build\winperfmon.exe --selftest   :: 自检，返回码 = 成败（0 = PASS）
build\winperfmon.exe --probe      :: 传感器探针，格式见 docs/release-github.md §11
build\winperfmon.exe --version
```

命令行覆盖（对应 `docs/config-schema.md` §8）：`--interval <ms>`、`--vertical`、
`--horizontal`、`--gpu <n>`。

> 输出重定向（`> out.txt` / 管道）正常工作：程序**优先沿用调用方已接好的标准句柄**，
> 只有在没有有效 stdout（双击运行）时才 `AttachConsole`/`AllocConsole`。

## 4. 单测

```cmd
build\wpm_tests.exe
:: 或
ctest --test-dir build --output-on-failure
```

当前 **71 passed / 0 failed**（`test_cpu_native.cpp` 5 个纯函数用例、`test_gpu_enum.cpp` 8 个、`test_config.cpp` 16 个、`test_json.cpp` 6 个、Ring0 解码口径 8 个、**帧统计 15 个与 CSV 解析 13 个（M2）**），覆盖：

- `json_*`：标量 / 对象 / 非法输入（尾随逗号、注释、未闭合 = 空串）/ 往返 / 深度与字节上限 / `\uXXXX` 转义
- `config_*`：默认值稳定（13 项默认可见）、目录全展开、越界夹紧、未知丢弃 + 缺失追加、
  别名与大小写规范、去重留首、强制至少一项可见、字段夹紧、颜色解析、JSON 往返、
  坏 JSON 不抛、未知键忽略、类型不符回退、枚举字符串稳定性
- `catalog_*`：大小写不敏感查找、id 唯一且全小写
- `cpu_native_*`：`BusyPercentFromDelta(idle0, busy0, idle1, busy1)` 的半忙=50%、全忙=100%/全闲=0%、**拒绝无效增量**（计数倒退、零增量、时钟未前进均返回 `-1`）、取值恒在 0–100
- `gpu_resolve_*`：`gpu.selectedIndex` 解析契约 —— `auto` 选第一个有后端的适配器（跳过无后端者）、显式下标如实采用、**越界不得改选其它 GPU**、显式指向无后端适配器 = 关闭而非回退、无可用后端/空调配器列表的返回、**不可用时也必须写出原因原文**
- **Ring0 解码口径（8 个用例）**：SMN 温度往返与无效判定、SVI 电压解码与越界、每核频率比值与坏差分、Intel 封装温度、能量单位
- **帧统计（15 个用例，M2）**：调和平均 vs 算术平均（99×10 ms + 1×25 ms → 98.52 而不是 99.4）、均匀帧时两条 1% Low 口径相等、最差 k 帧口径（990×10 + 10×33.333 → 30.0）、**并列帧时间时两条口径分歧**（100×10 + 1×50 → 33.33 vs 96.19）、0.1% Low 至少取 1 帧、卡顿率与均值/中位两种基线、异常帧剔除（`{10,0,-5,1500,8}` → 2 帧有效 / 3 帧剔除）、中位数奇偶、环形绕圈保留最后 N 帧、`rejected` 与 `dropped` 分开计数、可选列逐帧缺省、PresentMode 取众数、空环 `!ok()`、环形与直接算两条路径必须一致、Reset
- **PresentMon CSV 解析（13 个用例，M2）**：列按名字映射与乱序、`NA` → 缺值而非 0、Dropped 与带引号模式、**逐字节分片喂入与整块喂入结果完全一致**、表头前横幅跳过并计数、坏行计数不致命、表头缺 `MsBetweenPresents` 时 300 行后致命、CRLF+BOM、末行无换行由 `Finish()` 兜住、`SplitCsvLine` 引号与空字段、`ParseCsvDouble` 拒绝 `NA`/空/`12abc`、端到端（夹具 → 帧 → 指标）

## 5. M0 验收记录（本机，2026-10）

- 机器：HP 战 66 14 G8a，AMD Ryzen 7 H 255（Family 25 = 0x19 / Model 117 = 0x75，Hawk Point），
  16 逻辑核，Windows 11 家庭版 26300.9711。
- 非管理员账户，`Performance Log Users` 组成员。
- `--selftest` → `SELFTEST PASS`（**可用 13 项 / 合计 50 项，错误 0 项**；M0 阶段曾是"可用 6 项"，现已含 M1 免驱项）。
- 免驱链路实测：
  - `cpu.freq.avg` = ADL2 PMLog `CLK_CPUCLK(34)`（**空闲 3775 MHz → 满载 4625 MHz**，随负载真实跟踪）；
    仅在 ADL2 不可用时才回退 PDH `% Processor Performance` × 基频（该回退路径沿用 `3793 MHz`；注册表 `~MHz` 报 `3801 MHz`，两者差一个刻度，属已知小差，不影响判读）。
  - `cpu.usage` 走原生 `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`：
    核心时间占比 × 当前频率/基频（基频 `3801 MHz`，取注册表 `CentralProcessor\0` 的 `~MHz`），
    **近似**任务管理器的 `% Processor Utility`。因为它只用一个"全核平均频率"因子，
    而 Utility 是逐核按各自频率加权，**部分核心满载时本值偏低几个百分点**
    （本机 8/16 线程满载：本值 56–57%，PDH Utility 59–62%）；频率不可信时退回未加权核心时间占比并如实标注。
    口径原文写进 `Reading::detail`。此路径**不触发性能计数器表加载**（见 §6 的性能小节）。
  - `mem.usage` / `mem.used` 走 `GlobalMemoryStatusEx`
- 未接线项**不显示 0**：按 `Availability` 标注为「需要 Ring0 通道（PawnIO），M1 接入」
  「需要厂商 GPU 库（NVML/ADLX/IGCL），M1 接入」「需要 PresentMon，M2 接入」。
  - 其中 `cpu.temp.package` / `cpu.power.package` / `cpu.power.soc` 的标注是编译期通道值
    `Availability::AvVendorOrRing0`（**不是** `AvRing0`）：AMD 上由 ADL2 免驱 PM 表提供，
    Intel 上才需要 Ring0 的 MSR DTS / RAPL。M1 免驱部分的验收见 §6。
- `--probe` 输出的头部字段（`admin=false`、`pawnio=absent`、`hvci=on`、
  `osBuild=26300.9711`）在本机读通，可直接作为 issue 附件。

## 6. M1 免驱遥测与性能验收（AMD ADL2 + 原生 CPU + 惰性 ACPI 兜底）

链路与判据见 `docs/winperfmon-dev-plan.md` §3.3.1 的 T0/T4，实测数据见 `docs/amd-facts.md` §9。
**全程不需要管理员、不需要 PawnIO（本机 `pawnio=absent`），HVCI 保持开启。**

| # | 步骤 | 预期 |
|---|---|---|
| 1 | `call tools\env.cmd` → `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` → `cmake --build build` | 构建成功，零第三方运行时依赖；`build\winperfmon.exe` 仍远低于 5 MB 体积守卫（当前约 1.35 MB） |
| 2 | `build\wpm_tests.exe`（或 `ctest --test-dir build --output-on-failure`） | 全部通过、`0 failed` |
| 3 | `build\winperfmon.exe --selftest` | 返回码 `0`（PASS）、错误 0 项 |
| 4 | `build\winperfmon.exe --once` | 50 项指标：**可用 13 项 / 本机无 10 项 / 待接入通道 27 项 / 错误 0** |
| 5 | `build\winperfmon.exe --probe` | 头部 `admin=false`、`pawnio=absent`、`hvci=on`、`osBuild=26300.9711`；**`startupMs=45.8`、`firstStepMs=0.91`**；AMD 免驱可用时 `gpu.metricsSource=ADL2`；`acpi.zones=` 列出 9 个热区；`[gpu.N]` 段含下表新增键 |
| 6 | `build\winperfmon.exe --watch 5` | 打印逐源采样耗时；整轮**平均 0.92 ms/次、最大 1.54 ms/次**，工作集 **11.88 MB** / 私有 **2.46 MB** / 峰值 **12.07 MB**（预算见 `docs/winperfmon-dev-plan.md` §1、§6） |
| 7 | 同上，观察 `--probe` 的 `stage.*` 分段 | `gpu-enum 24.6` / `adl2 20.4` / `cpu.native 0.4` / `memory 0.0` / `acpi-thermal 0.0` / `首帧发布 0.0`（ms）；首帧**不再有 +4.6 MB 跳变**（本机首帧仅 +0.06 MB 工作集） |

`--once` 本轮的可用项（数值为本机实测）：

| 指标 | 实测值 | 来源 |
|---|---|---|
| `cpu.usage` | 6 % | 原生 `NtQuerySystemInformation`（近似 `% Processor Utility`，口径见 §5） |
| `cpu.freq.avg` | 3775 MHz（空闲）→ 4625 MHz（满载） | ADL2 `CLK_CPUCLK`(34)（免驱默认） |
| `cpu.temp.package` | 46 °C | ADL2 `TEMPERATURE_CPU`（免驱主路径，ACPI 仅兜底） |
| `cpu.power.package` | 13.0 W | ADL2 `ASIC_POWER`（核显上即整片功耗） |
| `cpu.power.soc` | — | ADL2 `SOC_POWER` |
| `mem.usage` | 69 % | `GlobalMemoryStatusEx` |
| `gpu.usage` | 2 % | ADL2 `INFO_ACTIVITY_GFX` |
| `gpu.freq.core` | 800 MHz | ADL2 `CLK_GFXCLK` |
| `gpu.freq.mem` | 2800 MHz | ADL2 `CLK_MEMCLK` |
| `gpu.temp.core` | 46 °C | ADL2 `TEMPERATURE_GFX` |

> `gpu.power` **不在**可用项里：核显上按反伪造规则返回「不可用 + 理由」（`GFX_POWER` 在 GPU 空载、CPU 满载时仍报 24–34 W），独显上才采用。
> `gpu.vram.used` / `gpu.vram.usage` **本轮起也不在可用项里**：ADL2 显存用量导出首次调用即让 perflib 计数器表常驻（+4.3 MB），且实测 GPU 满载时数值恒定、疑非实时用量 → 默认不启用（`defaultVisible=false`，默认视图不受影响）；M4 再评估显式 opt-in。
> 这就是"可用 15 → 13"的来源（其余项与计数口径见 `docs/amd-facts.md` §9）。

**性能验收（同样无管理员、无 PawnIO）**：

| 项 | 预算 | 本机实测 | 对照：PDH 路径旧构建 |
|---|---|---|---|
| 常驻内存（`--watch 5`） | ≤ 15 MB | **工作集 11.88 MB / 私有 2.46 MB / 峰值 12.07 MB** | 工作集 18.04 MB / 私有 6.70 MB |
| 冷启动（`--probe`） | ≤ 300 ms | **`startupMs = 45.8 ms`**、`firstStepMs = 0.91 ms` | `startupMs = 435.7 ms`（`stage.cpu.pdh = 381.2`） |
| `--version` 进程墙钟（5 次） | — | **23.3 / 11.2 / 11.5 / 10.6 / 10.9 ms** | — |
| 整轮采样（11 次，`--watch 5`） | — | **平均 0.92 ms、最大 1.54 ms** | 平均 23.65 ms、最大 295 ms |

> 采样/启动类数字**逐轮抖动**（复测一轮：整轮平均 `1.09 ms / 最大 3.28 ms`，逐源 `adl2 0.88`、`cpu.native 0.18`、`memory 0.01`，`startupMs 44.1`）；内存口径稳定（工作集 `11.88 MB`、峰值 `12.07 MB` 复测完全一致）。**判据看量级，不看末位。**

逐源平均耗时（ms）：`acpi-thermal 0.00`、`adl2 0.76`、`cpu.native 0.13`、`memory 0.02`（旧构建 `cpu.pdh 0.48`）。
冷启动分段（ms）：`stage.gpu-enum 24.6`、`stage.memory 0.0`、`stage.adl2 20.4`、`stage.cpu.native 0.4`、`stage.acpi-thermal 0.0`、`stage.首帧发布 0.0`（启动类数字逐次抖动几毫秒：复测一轮为 `startupMs 44.1 / firstStepMs 1.03`、`gpu-enum 22.2 / adl2 21.3 / cpu.native 0.2`；**判据看量级，不看末位**）。
内存阶梯（工作集 / 私有 MB）：入口 5.96/1.22 → 配置前 6.14/1.27 → 配置后 6.19/1.27 → DXGI 后 8.73/1.80 → `provider:memory` 8.77/1.80 → `provider:adl2` 11.75/2.46 → `provider:cpu.native` 11.76/2.46 → `provider:acpi-thermal` 11.77/2.46 → 初始化后 11.77/2.46 → **首帧后 11.83/2.46**（**首帧不再有 +4.6 MB 跳变**）。

> **口径提醒（旧文档已更正）**：那笔 4.3 MB / 约 280–300 ms 的一次性开销来自 **Windows 性能计数器表（perflib）本身**，不是某个 API 的"特性"。`PdhAddEnglishCounterW` 首次 270.38 ms；`PdhOpenQueryW` = 0.00 ms；`PdhExpandWildCardPathW` 在表已加载时只要 0.05–0.08 ms —— **此前把 4.3 MB 归因于 `PdhExpandWildCardPathW` 是误判**。同一进程里先调 ADL2 显存用量导出后，`PdhAddEnglishCounterW` 从 270 ms 降到 0.05 ms（共用同一张表）；表一旦加载**在同一进程内不归还**。机制、证据与设计后果见 `docs/winperfmon-dev-plan.md` §3.1/§6 与 `docs/amd-facts.md` §9。

`--probe` 相对既有输出的新增键：

| 键 | 含义 |
|---|---|
| `startupMs` | 从进程入口到首个可用快照的总耗时（本机 `45.8`） |
| `firstStepMs` | 首次采样步耗时（本机 `0.91`） |
| `stage.<名>` | 冷启动分段打点（`gpu-enum` / `memory` / `adl2` / `cpu.native` / `acpi-thermal` / `首帧发布`） |
| `acpi.zones` | 本机热区清单：`\_TZ.BAGZ(other) \_TZ.BATZ(other) \_TZ.CHGZ(other) \_TZ.CPUZ(cpu) \_TZ.EXTZ(other) \_TZ.GFXZ(gpu) \_TZ.LOCZ(other) \_TZ.MSHZ(other) \_TZ.PGTZ(other)`（按类型标 cpu/gpu/other）。**只有 `--probe` 会触发这次枚举**，常驻路径是惰性的——枚举本身会让 perflib 计数器表常驻，见上"口径提醒" |
| `gpu.selectedIndexConfig` | 配置里写的 `gpu.selectedIndex`（配置值本身，不等同于实际生效下标） |
| `gpu.metricsSource` | `ADL2` = AMD 免驱 PM 表可用；`none` = 没有可用的 GPU 遥测通道 |
| `gpu.acpiFallbackZone` | ACPI 兜底实际采用的热区实例名（无则 `none`） |
| `[gpu.N] sharedSystemMemoryMB` | 该适配器的共享系统内存（MB）——核显显存占用率分母的一部分 |
| `[gpu.N] dxgiIndex` | DXGI 枚举下标（折叠重复项之前的原始下标；本机 WARP 适配器为 `4`） |
| `[gpu.N] luid` | 适配器 LUID（本机 4 条重复的 AMD 核显记录同为 `0x137D8`） |
| `[gpu.N] duplicates` | 被折叠的同型号重复记录数（本机 `4`） |

判据（与 §5 一致）：**读不到就说明原因，不显示 0**；设置页的 `gpu.selectedIndex`
选项基于**折叠后**的适配器列表（本机 2 项：AMD 核显 + WARP）。

## 7. M1 Ring0 真机验收（PawnIO 2.2.0 + 官方 AMD 模块，**需管理员**）

前置：安装 PawnIO 2.2.0（服务 `PawnIO` = Running / 启动类型 Manual；驱动镜像落在 DriverStore，
`DisplayVersion = 2.2.0.0`），并把官方模块 `.bin` 放到 `<exe 目录>\modules\`（本机 `build\modules\AMDFamily17.bin`）
—— **PawnIO 安装器本身不带模块**。协议的判据与解码口径见 `docs/ring0-pawnio.md` §5。

| # | 步骤 | 预期 |
|---|---|---|
| 1 | 以**管理员身份**打开终端 → `build\winperfmon.exe --probe` | 头部 `admin=true`、`pawnio=present`；出现 `[ring0]` 段，且位于 `[superio]` 之后、`[gpu]` 之前 |
| 2 | 看 `[ring0]` 身份字段 | `ring0.vendor=AMD`、`ring0.device=\\?\GLOBALROOT\Device\PawnIO`、`ring0.driverVersion=2.2.0.0`、`ring0.module=loaded`、`ring0.perCore=16/16` |
| 3 | 看 SMN 温度 | `ring0.smn.tempRaw=0x774B0000` → `ring0.smn.tempC=70.2`（`raw >> 21 / 8 − 49`，**Tctl 口径**） |
| 4 | 看电压（本机） | `ring0.smn.coreVoltageV=n/a`、`ring0.smn.voltageRaw=0x00000000`、`ring0.smn.tfn=0x00000000`，并带 `ring0.smn.voltageNote` 原因原文 —— **读不到就说明原因，不显示 0** |
| 5 | 看封装功耗 | `ring0.pwrUnitRaw=0x000A1003`（ESU = 16 → 能量单位 `2^-16 J` = 15.2588 µJ）、`ring0.pkgPowerW=25.8` |
| 6 | `build\winperfmon.exe --all` | 50 项汇总：**可用 13 / 本机无 12 / 待接入通道 25 / 错误 0**（与 §6 免驱的 13/10/27 **口径不同，不得混用**） |
| 7 | `build\winperfmon.exe --watch 12` | 采样 25 次：**平均 1.66 ms/次、最大 3.34 ms/次**；工作集 **11.96 MB** / 私有 **2.47 MB** / 峰值 **12.05 MB**；逐源 `cpu.ring0 0.65 ms`；`--probe` 的 `startupMs = 43.4`、`stepAvgMs = 1.66` |
| 8 | `build\wpm_tests.exe` | **71 passed / 0 failed**（含 8 个 Ring0 解码口径用例与 28 个 M2 帧统计/CSV 用例，见 §4） |

`--probe` 的 `[ring0]` 段逐字（本机 HP ZHAN66 14 G8a / Ryzen 7 H 255 / Radeon 780M / Win11 build 26300）：

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

**非管理员运行会怎样**：`--probe` 里 `pawnio=present`、`ring0Backend=PawnIO`，但
`ring0.unavailableReason=打不开 PawnIO 设备（错误 5（拒绝访问。））` —— PawnIO 驱动 INF 给设备对象设的 ACL 是
`D:P(A;;GA;;;SY)(A;;GA;;;BA)`，**只授权 SYSTEM 与 Administrators**，交互式用户没有任何权限位。
因此 **Ring0 通道必须在提权进程里跑**；免驱路径（ADL2 + 原生 CPU 采样 + 惰性 ACPI）不受影响、也不需要提权
（实测根因与 INF 原文见 `docs/ring0-pawnio.md` §3.1）。

三条必须记住的口径（详见 `docs/ring0-pawnio.md` §5.3–§5.5、§7）：

- **SVI2 电压在本机不可用**：`SVI0_TFN`(0x5A008) 与两个遥测平面在一次 20 s 提权采样（含 16 线程 CPU 满载）中整轮恒为 `0x00000000`
  → `cpu.voltage` 如实判「不可用」。**`0` 不是 0 V、更不是 1.55 V** —— 把原始值 0 解码成 1.550 V 是假读数。
- **SMN 温度是 Tctl 口径，不可与 ADL2 直接比较**：同一次 20 s 并发实验里 SMN 从 66.75 °C 升到峰值 **88.1 °C**（t ≈ 9 s）、
  再衰减回 72.75 °C，而同窗口 ADL2 `TEMPERATURE_CPU` 只有 57–66 °C —— **相差 10–20 °C**（ACPI 热区更低且滞后约 20 s）。
  故 `cpu.temp.package` 的 detail 必须写明这是 Tctl。
- **MSR 封装能耗与 ADL2 互证**：`MSR_PKG_ENERGY_STAT`(0xC001029B) 差分（乘 `2^-16 J`）得空闲 **18–25 W** / 满载 **33–43 W**，
  ADL2 `ASIC_POWER` 为空闲 21–24 W / 满载 32–42 W —— 两条独立路径一致。

`cpu.freq.percore` 首帧按设计不可用，文案为：「APERF/MPERF 尚无两个采样点（第一轮只建基线）；每轮钉 4 核轮换，0.5 s 一轮」。

## 8. M2 验收（PresentMon 帧统计与采集链路）

M2 的**解析与统计部分**已实现并验收；**真机 PresentMon + 真游戏**那一步还没做（本机没装 PresentMon、也没装游戏）。
口径与诚实边界见 `docs/fps-presentmon.md`。

| # | 步骤 | 预期 |
|---|---|---|
| 1 | `build\wpm_tests.exe` | **71 passed / 0 failed**，其中帧统计 15 个、CSV 解析 13 个（见 §4） |
| 2 | `build\winperfmon.exe --once` | 汇总口径变为 **可用 13 / 本机无 20 / 待接入通道 17 / 错误 0**（50 项）—— 10 个 `fps.*` 由 FPS provider 认领；本机没有 PresentMon，故判「本机无」并列出**找过的每一个路径** |
| 3 | `build\winperfmon.exe --probe` 的 `[fps]` 段 | `fps.presentmon=<路径 或 未找到>`、`fps.running=false`（**`--probe` 不启动子进程**）、`fps.note=诊断模式（--probe）：只报状态，不启动 PresentMon 子进程`；段序位于 `[gpu.N]` 之后、`[config]` 之前 |
| 4 | 检查进程没有被凭空拉起 | `--probe` 的 `firstStepMs` 应 < 1 ms（未加诊断模式时会接近 10 ms —— 首帧把 PresentMon 拉起来了） |
| 5 | 装上真 PresentMon 后 | `fps.running=true`、`fps.target` 为前台进程名或配置的目标；`--watch` 行尾出现 `FPS n（1% Low m）` |

**夹具端到端（已验证，可复现）**：把 `D:\GitHub\winperfmon-dsh-scratch\fake_presentmon.cpp` 编译成
`PresentMon.exe` 放进 `build\`（**该夹具不进仓库，验证完必须删掉**），会打印与 PresentMon 同格式的逐帧 CSV：

- `--probe` → `fps.csvFrames` 与 `fps.ringFrames` 一致增长，`fps.header=MsBetweenPresents,MsCPUBusy,MsGPUBusy,Dropped,PresentMode`
- `--watch 6` → 每行形如 `[   2.1s] CPU 17%  频率 3775 MHz  内存 78%  FPS 59（1% Low 35）`；13 次采样**平均 1.90 ms/次、最大 7.93 ms/次**，逐源 `fps 0.77 ms`，工作集 **13.10 MB** / 私有 **3.84 MB**

**尚未验证**（需要真机真游戏，见 `docs/fps-presentmon.md` §5）：与 CapFrameX 同局对比 1% Low 偏差 ≤0.5 FPS、
FPS 采集额外 CPU ≤1%、`--v2_metrics` 自动重试路径（只在装着 PresentMon 1.x 的机器上才会被触发）。

## 9. 下一步（Ring0 之外仍未做的部分）

M1 的**免驱部分与 Ring0 部分都已在本机验收通过**（免驱见 §6；Ring0 见 §7）。按 `docs/winperfmon-dev-plan.md` §8，
仍未开始的是：**每核**温度与核心电压（当前 SMN 只给封装温度、SVI2 在本机读不到）、RAPL 每核功耗、
Super I/O 探测 + 风扇 RPM、笔记本的 EC / 厂商 WMI 降级链（§3.3.1），以及 Intel 平台的电压口径（未在 Intel 机器上实测）。
权威的「尚未做」清单见 `docs/ring0-pawnio.md` §8。
