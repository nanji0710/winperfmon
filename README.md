# winperfmon 文档集

> 产品名 **winperfmon**（已定：EXE `winperfmon.exe`、配置目录 `%APPDATA%\winperfmon\`、仓库名 `winperfmon`）。
> 目标：Windows 极致轻量化性能监控工具 —— CPU/GPU 频率、功耗、温度，风扇转速，FPS 与 1% Low FPS。
>
> **本集为 5 份设计文档 + 3 份证据台账 + 1 份构建手册**（2026-10，全部在 `docs/`）。本文件是仓库根 README，同时充当文档索引。
> 精简前的 14 份文档完整快照保存在 git 分支 `docs-archive`（提交 `6fca7fe`）里，恢复任一文件：
> `git show 6fca7fe:docs/<文件名> > docs/<文件名>`。

## 一、当前阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 开发方案（技术路线、驱动方案、里程碑） | ✅ 完成（`docs/winperfmon-dev-plan.md`） |
| P1 | 设计文档集：配置数据结构与接口、AMD 支持、UI 设置、发布合规 | ✅ 完成（`docs/` 5 份设计文档 + 3 份证据台账 + 1 份构建手册） |
| P2 | 编码实现（M0 骨架 → M1 传感器 → M2 FPS → M3 Overlay → M4 打包） | 🔵 进行中：**M0 已完成**（CPU 频率/占用 + 内存 + 配置系统 + **43 个单测全绿**）；**M1 免驱部分已完成**（AMD ADL2 PM 表 + **原生 CPU 通道** + 惰性 ACPI 热区兜底，本机 50 项里 **13 项可用**）；**M1 的 Ring0 部分已完成真机验收**（PawnIO 2.2.0、管理员运行：每核频率 16/16 核、SMN 封装温度、MSR 封装能耗并与 ADL2 互证，见 `docs/ring0-pawnio.md` §7；Intel 侧仅单测口径覆盖，未在真机实测）；**Ring0 通道必须以管理员身份运行**（PawnIO 设备对象 ACL 只授权 SYSTEM 与 Administrators），免驱默认路径不受影响、无需提权；**核心电压在本机不可用** —— SVI2 的 TFN 与两个遥测平面整轮恒为 `0x00000000`，`0` 不是 0 V、更不是 1.55 V，如实判为读不到 |

**M0 骨架已可构建运行**：`build\winperfmon.exe` 约 1.45 MB（预算 ≤ 5 MB），零第三方运行时依赖。
**M1 免驱遥测已接入**：AMD 平台无需 Ring0、无需管理员即可读到 CPU 温度/封装功耗/SoC 功耗、**CPU 平均频率（ADL2 `CLK_CPUCLK`）与 CPU 占用（原生 `NtQuerySystemInformation`）**，以及 GPU 频率、占用、温度；ACPI 热区作为**惰性**兜底（只在真要读温度时才枚举）。**默认路径完全不碰 Windows 性能计数器表（perflib）** —— 那笔 4.3 MB / 约 280–300 ms 的一次性开销来自计数器表本身（谁先碰谁付账且不归还），因此 PDH 退为末位降级，详见 `docs/winperfmon-dev-plan.md` §3.1/§6 与 `docs/amd-facts.md` §9。

**M1 免驱性能实测（本机 HP ZHAN66 14 G8a / Ryzen 7 H 255 / Radeon 780M / Win11 26300 / 非管理员 / 无 PawnIO）**：

| 维度 | 目标 | 实测 | 对照旧构建（PDH 路径） |
|---|---|---|---|
| 常驻内存 | ≤ 15 MB | **工作集 11.88 MB / 私有 2.46 MB / 峰值 12.07 MB** | 工作集 18.04 MB / 私有 6.70 MB |
| 冷启动 | ≤ 300 ms | **45.8 ms**（首帧 0.91 ms） | 435.7 ms（`stage.cpu.pdh = 381.2`） |
| 采样成本 | ≤ 0.3% | **0.92 ms/轮**（逐源：`adl2 0.76`、`cpu.native 0.13`、`memory 0.02`、`acpi-thermal 0.00`） | 23.65 ms/轮（`cpu.pdh 0.48`） |
| `--once` 可用项 | — | **13 / 50**（本机无 10 / 待接入通道 27 / 错误 0） | — |
构建与验收步骤见 `docs/build-and-run.md`（§6 免驱、§7 Ring0 提权验收）；实测证据见 `docs/amd-facts.md`。**Ring0 提权实测（PawnIO 2.2.0，管理员运行）**：整轮采样 **平均 1.66 ms/次、最大 3.34 ms/次**（25 次），工作集 **11.96 MB** / 私有 **2.47 MB** / 峰值 **12.05 MB**，`startupMs = 43.4 ms`、逐源 `cpu.ring0 0.65 ms`；`--all` 汇总 **可用 13 / 本机无 12 / 待接入通道 25 / 错误 0（合计 50）**；单测 **43 passed / 0 failed**。

```cmd
call tools\env.cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\winperfmon.exe --selftest
```

## 二、文档清单（5 份设计文档 + 3 份证据台账 + 1 份构建手册）

| 文档 | 内容 | 状态 |
|---|---|---|
| `docs/winperfmon-dev-plan.md` | **主方案**：目标指标、架构、逐项技术路线（CPU / GPU / 风扇 / FPS）、驱动方案、采样与内存预算（含 **perflib 计数器表 4.3 MB 归因与免驱实测**）、Overlay、里程碑、风险、竞品、v2 增强、决策表、参考项目与借用清单。**技术数值以本文为准。** | ✅ 完成（**470 行**） |
| `docs/config-schema.md` | **配置数据结构（字段 / 类型 / 默认值 / 校验）+ C++ 读写接口规范**：排版方式、指标可见性与排序、置顶、字号与颜色、GPU 选择。§4 目录表下新增「⑤ 口径」块（`cpu.usage` 近似 `% Processor Utility`、`gpu.vram.*` 在 ADL2 上默认不启用） | ✅ 设计稿（§6.1-2 已闭环：新增 `gpu.selectedIndex`） |
| `docs/ui-settings.md` | 设置界面的交互与行为规范（页面划分、即时预览、热键、DPI、校验反馈、首次运行向导） | ✅ 设计稿（175 行） |
| `docs/amd-cpu.md` | AMD CPU（Zen）支持设计：识别、访问通道、寄存器 / SMU 表、指标映射、降级矩阵、测试矩阵 | ✅ 设计稿 v2（186 行；8 项已闭环：6 ✅ / 2 ❌ 无一手来源并给探测方案） |
| `docs/amd-facts.md` | AMD 8 项技术事实的**证据账本**：结论 / 证据（文件:行号 或 URL）/ 置信度 / 对设计的影响，含 7 处更正与 `amd_tables.json` 字段建议；**§9 为 ADL2 免驱 PM 表实测**（含 perflib 归因、`CLK_CPUCLK` 采用判定、`Claims()` 契约） | ✅ 核实完成（**263 行**） |
| `docs/adl2-facts.md` | **ADL2 传感器索引表的原始核实记录**（`docs/amd-facts.md` §9 的底稿）：跨 SDK 版本一致性、索引 0–45 语义表、支持性判定的更正公告、缩放与结构体布局。**注意开头 §0 更正公告**：原稿把"采样值非零"当成"驱动支持" | ✅ 核实完成（373 行；§0 更正已生效） |
| `docs/release-github.md` | 发布与许可合规：MIT、第三方合规动作、随包分发 PawnIO（GPL-2.0）/ 模块（LGPL-2.1）、仓库与 CI、发布物、检查清单、隐私要点、芯片表贡献流程 | ✅ 设计稿（`wil` 已按 §6.1-1 移出依赖表） |
| `docs/laptop-telemetry-facts.md` | **笔记本电脑遥测证据综述**（证据，非设计）：ACPI 热区、EC 直连、厂商 WMI 全矩阵（HP / Dell / Lenovo / ASUS / MSI / Acer / Gigabyte / Clevo / Surface / Framework）、可借鉴开源实现、分层回退链建议。**主方案 §3.3.1 的依据**，逐条带 URL 与 `repo:path:line` | ✅ 核实完成（444 行） |
| `docs/build-and-run.md` | **构建与验收手册**：便携工具链（GCC 16.2 / CMake 4.4 / Ninja）、构建命令、CLI 用法与覆盖项、单测范围（**43 passed / 0 failed**）、本机 M0/M1 验收记录、**免驱性能验收表**（内存 11.88 MB / 冷启动 45.8 ms / 整轮 0.92 ms / 可用 13 项）与 **§7 Ring0 提权验收**（PawnIO 2.2.0：每核 16/16、SMN 温度、`--all` 13/12/25/0、整轮 1.66 ms、工作集 11.96 MB） | ✅ M0 + M1 免驱 + M1 Ring0 完成（含性能验收） |
| `docs/ring0-pawnio.md` | **Ring0 通道（PawnIO）**：为什么必须借助驱动、**设备 ACL 导致的必须提权结论**（错误 5 的根因）、GPL-2.0 例外与 LGPL-2.1 模块的合规边界、设备与 IOCTL 常量、模块函数表、AMD SMN/MSR 与 Intel MSR 常量与解码（含 **SVI2 电压在本机不可用**、**SMN 温度是 Tctl 口径**、MSR 封装能耗与 ADL2 互证）、每核频率为什么用"跨轮差分"、反伪造规则、**真机验收记录**、尚未做的部分（EC 风扇 / Super I/O / 每核温度与电压） | ✅ 真机验收通过（PawnIO 2.2.0，**需管理员**；本机核心电压读不到，如实报「不可用 + 原因」） |
| `README.md` | 本文：索引 + 决策状态 + 开放问题汇总 + 术语 | ✅ |

## 三、建议阅读顺序

1. `docs/winperfmon-dev-plan.md`（全景：目标指标、架构、驱动方案、里程碑）
2. `docs/config-schema.md`（用户自定义设置的数据契约，编码前必须冻结）
3. `docs/ui-settings.md`（同一套设置的用户可见行为）
4. `docs/amd-cpu.md` → 配套证据 `docs/amd-facts.md`（其 §9 的底稿是 `docs/adl2-facts.md`）
5. `docs/release-github.md`（发布与合规）

## 四、决策状态

| # | 决策点 | 结论 | 来源 |
|---|---|---|---|
| ① | 是否必须覆盖 AMD CPU | **是**（必须支持） | 用户指令 |
| ② | GPU 三家都做还是先 NVIDIA | **三家都做**：v1 覆盖 NVIDIA（NVML）+ AMD（ADLX，旧卡回退 ADL2）+ Intel（IGCL，备用 Level Zero Sysman） | 用户指令 |
| ③ | PawnIO 分发方式 | ✅ **已决：随发布包附带官方安装包并静默安装**（用户确认后安装，可拒绝）；须履行 GPL-2.0（驱动安装包）与 LGPL-2.1（随包模块，若分发）合规——许可证文本 + 源码获取说明 + 模块可替换 | 用户指令 |
| ④ | 真独占全屏（FSE）下的 Overlay 可见性 | ✅ **已决：硬需求 → RTSS OSD 共享内存后端为主路径**（我方不注入；无 RTSS 时回退自绘 Overlay 并提示） | 用户指令 |
| ⑤ | 是否发布到 GitHub / 对外分发 | **是** | 用户指令 |
| ⑥ | 运行时用户自定义设置（排版 / 指标 / 排序 / 置顶 / 字号颜色） | **是**，且要求先冻结数据结构与读写接口 | 用户指令 |
| ⑦ | 产品名与仓库名 | **winperfmon** | 用户指令 |
| ⑧ | 依赖策略（配置读写 / PresentMon 数据解析） | **已定**：配置读写用自研极简 JSON（零第三方依赖）；PresentMon 走逐帧 CSV（官方无 JSON 输出）；**`wil` 已决定不引入**（§六 6.1-1，零第三方依赖） | `docs/config-schema.md` §0 不变量 6 |

## 五、已并入本集的内容（2026-10 精简对照）

| 原文档 | 内容去向 |
|---|---|
| `architecture.md`（进程/线程模型、模块边界、采样与内存预算、启动时序） | 主方案 §2、§6 |
| `cpu-intel.md`（Intel MSR / RAPL / DTS 细节、降级矩阵） | 主方案 §3.1 |
| `gpu-fans.md`（NVML / ADLX / IGCL 与 Super I/O 寄存器映射、已知坑、数据驱动层） | 主方案 §3.2、§3.3 |
| `fps-metrics.md`（FPS 采集路径、PresentMode 自检、1% Low 等口径） | 主方案 §3.4（`docs/config-schema.md` 中的口径引用已改指此处） |
| `roadmap.md`（里程碑、工期、风险表） | 主方案 §8、§9、§12 |
| `privacy.md`（隐私与安全声明） | `docs/release-github.md` §9（发布前仍需独立成面向用户的 `docs/privacy.md`） |
| `contributing-sensors.md`（芯片表贡献流程 + `--probe` 格式权威定义） | `docs/release-github.md` §11（发布前仍需独立成面向用户的 `docs/contributing-sensors.md`） |

## 六、开放问题汇总（编码前需要关闭）

> 本节汇总了被删文档里的全部「待核实」项，避免信息随文件一起消失；发布/合规侧的 12 项见 `docs/release-github.md` 附录 A。

### 6.1 需要用户拍板 —— ✅ 三项均已闭环（2026-10）

| # | 问题 | 结论 |
|---|---|---|
| 1 | 是否引入 `wil`（MIT，唯一候选第三方头文件依赖） | ✅ **不引入，保持零第三方依赖**。M0 只需约 50 行自写 RAII 封装（句柄 / 注册表 / COM 智能指针）即可覆盖；「零依赖」口径与 EXE ≤ 5 MB 目标最稳，日后确有需要再单独立项（引入时须同步 `docs/release-github.md` §3.3 与 `docs/config-schema.md` §0 不变量 6） |
| 2 | 是否新增 `gpu.selectedIndex`（`-1` = 自动选渲染 GPU） | ✅ **新增**。本机实测旁证：除 `AMD Radeon(TM) Graphics`（Radeon 780M）外还枚举出 3 个虚拟显示适配器（GameViewer Virtual Display / OrayIddDriver / Honor Virtual Display），「自动选渲染 GPU」在多 GPU 与虚拟适配器环境下必有歧义 → 字段与 UI 见 `docs/config-schema.md` §3.6.1、`docs/ui-settings.md` §6.2 |
| 3 | 是否提供英文 README | ✅ **提供，发布前补 `README.en.md`**。Super I/O 芯片表贡献流程（`docs/release-github.md` §11）依赖海外用户反馈，排在 M4 打包阶段，不阻塞编码 |

### 6.2 需要实机或明确来源才能关闭

| # | 项目 | 现状 |
|---|---|---|
| 4 | Intel `cpu.voltage`（`0x198` 电压位域）与 `cpu.throttle.reason`（降频原因位） | 源文档未给位域定义；**核实前这两项在 Intel 上保持 N/A，绝不显示推算值** |
| 5 | Intel P/E 核拓扑识别（`GetLogicalProcessorInformationEx` + CPUID） | 影响「平均频率」口径（每核比值先算再平均） |
| 6 | PresentMon 接入形态：回调线程 vs 轮询消费 | 决定线程预算（当前按 UI / 采集 / FPS+统计 三线程）；以 M2 实际 API 为准 |
| 7 | 采集线程是否绑核（`SetThreadIdealProcessor`） | 影响 CPU 占用口径（≤0.3% / ≤1% 按单核占用率计）。免驱路径实测整轮采样 **0.92 ms**、逐源 `adl2 0.76`、`cpu.native 0.13`、`memory 0.02`、`acpi-thermal 0.00`，M1 收尾时定 |
| 8 | 启动耗时分段预算（≤300 ms） | ✅ **已实测并达标**：`--probe` 报 `startupMs = 45.8 ms`、`firstStepMs = 0.91 ms`；分段 `gpu-enum 24.6 / adl2 20.4 / cpu.native 0.4 / memory 0.0 / acpi-thermal 0.0 / 首帧发布 0.0`（免驱路径；PDH 路径对照 435.7 ms）。剩余：换机器/换平台的复测与每段预算的正式冻结 |
| 9 | IGCL / Level Zero 单次采样开销与采样周期 | 当前按 ADLX 同档（1 s）假设，需实测 |
| 10 | 数据驱动芯片包（`sensor-tables/*.json`）的签名 / 校验和格式 | 防止被篡改后写入 Ring0 端口；格式待定 |
| 11 | Intel Arc / Iris Xe 真机可得性 | 若无设备，IGCL 支持延后并在代码中标 `unverified` |
| 12 | Super I/O 跨主板差异返工量 | 逐板卡工作、历史上不可控，已排除在工期估算外（主方案 §9） |
| 13 | RTSS OSD 文本标签（颜色 / 字号 / 透明度 / 粗体）支持矩阵 | 决策 ④=B 的字号/颜色映射依赖它（`docs/release-github.md` 附录 A-12） |
| 14 | AMD 家族 → SMU 邮箱三元组映射、各 SKU 的 Tjmax | 无一手来源；`docs/amd-cpu.md` §9 已改为运行期探测 + 随包 SKU 表方案，证据见 `docs/amd-facts.md` 第 5、7 项 |

## 七、术语

| 术语 | 含义 |
|---|---|
| 免驱模式 | 不安装任何 Ring0 驱动时的降级工作模式。**AMD 平台已能做到免驱全遥测**：CPU 频率走 ADL2 PMLog `CLK_CPUCLK`、CPU 占用走原生 `NtQuerySystemInformation`、CPU/GPU 温度与功耗走 ADL2 PM 表、GPU 频率/占用走 ADL2，ACPI 热区仅作**惰性**兜底；**默认路径完全不碰 perflib 性能计数器表**，PDH 只是拿不到 ADL2 时的末位降级。Intel/NVIDIA 平台仍为：CPU 频率走 PDH，GPU 全指标走 NVML/ADLX/IGCL，CPU 温度/功耗/风扇显示 N/A 并说明原因 |
| Ring0 通道 | 经 PawnIO 通用驱动读 MSR / 端口 I/O / SMN 的提权采集路径 |
| Tctl / Tdie | AMD 温度：Tctl 为带偏移的控制温度，Tdie 为实际结温（Tdie = Tctl − offset） |
| RAPL | Intel/AMD 的能量计数器机制，用「能量差分 ÷ 时间」算功耗 |
| PM 表 | AMD SMU 暴露的遥测结构化数据区（PPT/TDC/EDC、SVI2 电压、每 CCD 温度等） |
| 1% Low | 帧时间降序取最差 1%（N = ceil(0.01 × 帧数)）帧的平均帧时间 T₁，1% Low = 1000 / T₁ |
| DIP / Dp | 与设备无关的像素（1/96 英寸）；字号、间距、圆角一律以 Dp 表示，再按 DPI 缩放 |
