# winperfmon 文档集

> 产品名 **winperfmon**（已定：EXE `winperfmon.exe`、配置目录 `%APPDATA%\winperfmon\`、仓库名 `winperfmon`）。
> 目标：Windows 极致轻量化性能监控工具 —— CPU/GPU 频率、功耗、温度，风扇转速，FPS 与 1% Low FPS。

## 一、当前阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 开发方案（技术路线、驱动方案、里程碑） | ✅ 完成（已并入本目录：`winperfmon-dev-plan.md`） |
| P1 | 设计文档集（本目录）：配置数据结构与接口、AMD 支持、UI 设置、发布合规 | ✅ 完成（14 份文档；AMD 一手事实核实已闭环：8 项中 6 项关闭、2 项无一手来源并给出探测方案） |
| P2 | 编码实现（M0 骨架 → M1 传感器 → M2 FPS → M3 Overlay → M4 打包） | ⛔ 未开始（用户要求：先整理文档） |

**当前仓库内没有可运行代码，只有设计文档。** 因此「增加 AMD CPU 支持」在 P1 阶段的含义是：把 AMD 支持写进设计（见 `amd-cpu.md`），编码阶段再落地实现。

## 二、文档清单

| 文档 | 内容 | 状态 |
|---|---|---|
| `config-schema.md` | **配置项数据结构（字段/类型/默认值/校验）+ 读写接口规范**：排版方式、指标可见性与排序、置顶、字号与颜色 | ✅ 设计稿 v1（待确认） |
| `ui-settings.md` | 设置界面的交互与行为规范（页面划分、即时预览、热键、DPI、校验反馈、首次运行向导） | ✅ 设计稿 v1（待确认） |
| `amd-cpu.md` | AMD CPU（Zen 架构）支持设计：识别、访问通道、寄存器/SMU 表、指标映射、降级矩阵、测试矩阵 | ✅ 设计稿 v2（8 项待核实已闭环：6 项 ✅、2 项 ❌ 无一手来源并给探测方案） |
| `amd-facts.md` | AMD 8 项技术事实的**证据账本**：逐条「结论 / 证据（文件:行号 或 URL）/ 置信度 / 对设计的影响」，含与 `amd-cpu.md` 原文冲突的 7 处更正与 `amd_tables.json` 字段建议 | ✅ 核实完成（已证实 4、已证实含推断 2、未找到 2） |
| `release-github.md` | 发布到 GitHub 的准备：许可证、第三方合规、随包分发 PawnIO（GPL-2.0）与模块（LGPL-2.1）的合规动作、CI、发布物、检查清单 | ✅ 设计稿 v2（12 项待核实；含决策 ③=B 新增项） |
| `privacy.md` | 面向用户的隐私与安全声明（不联网/不采集/不注入/Ring0 可选） | ✅ 设计稿 v1 |
| `architecture.md` | 架构（进程模型、模块边界、线程与采样/内存预算、启动时序、配置系统位置） | ✅ 拆自 §2/§6（4 项待核实） |
| `cpu-intel.md` | Intel CPU 路线（MSR / RAPL / DTS 细节与降级矩阵） | ✅ 拆自 §3.1（7 项待核实） |
| `gpu-fans.md` | GPU（NVML/ADLX/IGCL）与 Super I/O 风扇路线（寄存器映射要点 + 已知坑） | ✅ 拆自 §3.2/§3.3（4 项待核实） |
| `fps-metrics.md` | FPS 采集路径、PresentMode 自检、1% Low 等指标口径定义 | ✅ 拆自 §3.4（0 项待核实） |
| `roadmap.md` | 里程碑与风险（含 AMD 与配置系统对工期的影响） | ✅ 拆自 §8/§9（4 项待核实） |
| `contributing-sensors.md` | 传感器（Super I/O / 风扇 / 笔记本 EC）支持贡献流程，并**权威定义 `wpm.exe --probe` 的输出格式**（`probeVersion=1`） | ✅ 设计稿 v1 |
| `winperfmon-dev-plan.md` | **主方案**（技术路线、驱动方案、里程碑；已并入本目录） | ✅ 完成 |

`winperfmon-dev-plan.md`（开发方案，已从工作区根目录移入本目录）是**主方案**，上述文档是它的细化与增补；两者冲突时以本目录文档为准，并回头修正主方案。

## 三、建议阅读顺序

1. `winperfmon-dev-plan.md`（先看全景：目标指标、架构、驱动方案、里程碑）
2. `config-schema.md`（用户自定义设置的数据契约，编码前必须冻结）
3. `ui-settings.md`（同一套设置的用户可见行为）
4. `amd-cpu.md`（AMD 支持的实现设计；配套证据与更正见 `amd-facts.md`）
5. `release-github.md`（发布与合规）

## 四、决策状态

| # | 决策点 | 结论 | 来源 |
|---|---|---|---|
| ① | 是否必须覆盖 AMD CPU | **是**（必须支持） | 用户指令 |
| ② | GPU 三家都做还是先 NVIDIA | **三家都做**：v1 覆盖 NVIDIA（NVML）+ AMD（ADLX，旧卡回退 ADL2）+ Intel（IGCL，备用 Level Zero Sysman） | 用户指令 |
| ③ | PawnIO 分发方式 | ✅ **已决：随发布包附带官方安装包并静默安装**；须履行 GPL-2.0（驱动安装包）与 LGPL-2.1（随包模块，若分发）合规（许可证文本 + 源码获取说明 + 模块可替换），用户拒绝时落免驱模式 | 用户指令 |
| ④ | 真独占全屏（FSE）下的 Overlay 可见性 | ✅ **已决：硬需求 → RTSS OSD 共享内存后端为主路径**（我方不注入；无 RTSS 时回退自绘 Overlay 并提示） | 用户指令 |
| ⑤ | 是否发布到 GitHub / 对外分发 | **是** | 用户指令 |
| ⑥ | 运行时用户自定义设置（排版/指标/排序/置顶/字号颜色） | **是**，且要求先冻结数据结构与读写接口 | 用户指令 |
| ⑦ | 产品名与仓库名 | **winperfmon**（EXE `winperfmon.exe`、`%APPDATA%\winperfmon\`、仓库 `winperfmon`） | 用户指令 |
| ⑧ | 依赖策略（配置读写 / PresentMon 数据解析） | **已定**：配置读写用自研极简 JSON（零第三方依赖）；PresentMon 走逐帧 CSV（官方无 JSON 输出）；`wil` 是否引入留待编码前拍板 | `config-schema.md` 不变量 6、`architecture.md` §12 |

## 五、与发布文档的命名对齐

`release-github.md` §5 给出的仓库目录树里出现几个面向用户的文件名，与本文档集的对应关系如下（避免两处漂移）：

| 发布文档中的文件名 | 本文档集的对应文件 | 状态 |
|---|---|---|
| `docs/metrics.md`（指标口径） | `fps-metrics.md` + `config-schema.md` §4 | ✅ 内部文档就绪；发布前需合并出一份面向用户的《指标口径》 |
| `docs/hardware-support.md`（支持矩阵） | `cpu-intel.md` + `amd-cpu.md` + `gpu-fans.md` | ✅ 内部文档就绪；发布前需另生成一份面向用户的「已支持硬件」清单 |
| `docs/privacy.md`（隐私声明） | `privacy.md` | ✅ 已独立成文件 |
| `docs/contributing-sensors.md`（芯片表贡献流程） | `contributing-sensors.md` | ✅ 已独立成文件（该文件即 `--probe` 输出格式的权威定义） |

## 六、术语

| 术语 | 含义 |
|---|---|
| 免驱模式 | 不安装任何 Ring0 驱动时的降级工作模式：CPU 频率走 PDH，GPU 全指标走 NVML/ADLX/IGCL，CPU 温度/功耗/风扇显示 N/A 并说明原因 |
| Ring0 通道 | 经 PawnIO 通用驱动读 MSR / 端口 I/O / SMN 的提权采集路径 |
| Tctl / Tdie | AMD 温度：Tctl 为带偏移的控制温度，Tdie 为实际结温（Tdie = Tctl − offset） |
| RAPL | Intel/AMD 的能量计数器机制，用「能量差分 ÷ 时间」算功耗 |
| PM 表 | AMD SMU 暴露的遥测结构化数据区（PPT/TDC/EDC、SVI2 电压、每 CCD 温度等） |
| 1% Low | 帧时间降序取最差 1%（N = ceil(0.01 × 帧数)）帧的平均帧时间 T₁，1% Low = 1000 / T₁ |
| DIP / Dp | 与设备无关的像素（1/96 英寸）；本项目的字号、间距、圆角等一律以 Dp 表示，再按 DPI 缩放 |
