# winperfmon 文档集

> 产品名 **winperfmon**（已定：EXE `winperfmon.exe`、配置目录 `%APPDATA%\winperfmon\`、仓库名 `winperfmon`）。
> 目标：Windows 极致轻量化性能监控工具 —— CPU/GPU 频率、功耗、温度，风扇转速，FPS 与 1% Low FPS。
>
> **本集已精简为 7 份**（2026-10）。精简前的 14 份文档完整快照在 git 历史里（提交 `6fca7fe`），恢复任一文件：
> `git show 6fca7fe:docs/<文件名> > docs/<文件名>`。

## 一、当前阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 开发方案（技术路线、驱动方案、里程碑） | ✅ 完成（`winperfmon-dev-plan.md`） |
| P1 | 设计文档集：配置数据结构与接口、AMD 支持、UI 设置、发布合规 | ✅ 完成（本目录 7 份） |
| P2 | 编码实现（M0 骨架 → M1 传感器 → M2 FPS → M3 Overlay → M4 打包） | ⛔ 未开始（用户要求先整理文档；本机暂无 C++ 工具链） |

**当前仓库内没有可运行代码，只有设计文档。**

## 二、文档清单（7 份）

| 文档 | 内容 | 状态 |
|---|---|---|
| `winperfmon-dev-plan.md` | **主方案**：目标指标、架构、逐项技术路线（CPU / GPU / 风扇 / FPS）、驱动方案、采样与内存预算、Overlay、里程碑、风险、竞品、v2 增强、决策表、参考项目与借用清单。**技术数值以本文为准。** | ✅ 完成（324 行） |
| `config-schema.md` | **配置数据结构（字段 / 类型 / 默认值 / 校验）+ C++ 读写接口规范**：排版方式、指标可见性与排序、置顶、字号与颜色 | ✅ 设计稿（479 行） |
| `ui-settings.md` | 设置界面的交互与行为规范（页面划分、即时预览、热键、DPI、校验反馈、首次运行向导） | ✅ 设计稿（163 行） |
| `amd-cpu.md` | AMD CPU（Zen）支持设计：识别、访问通道、寄存器 / SMU 表、指标映射、降级矩阵、测试矩阵 | ✅ 设计稿 v2（187 行；8 项已闭环：6 ✅ / 2 ❌ 无一手来源并给探测方案） |
| `amd-facts.md` | AMD 8 项技术事实的**证据账本**：结论 / 证据（文件:行号 或 URL）/ 置信度 / 对设计的影响，含 7 处更正与 `amd_tables.json` 字段建议 | ✅ 核实完成（140 行） |
| `release-github.md` | 发布与许可合规：MIT、第三方合规动作、随包分发 PawnIO（GPL-2.0）/ 模块（LGPL-2.1）、仓库与 CI、发布物、检查清单、隐私要点、芯片表贡献流程 | ✅ 设计稿（含本次合并内容） |
| `README.md` | 本文：索引 + 决策状态 + 开放问题汇总 + 术语 | ✅ |

## 三、建议阅读顺序

1. `winperfmon-dev-plan.md`（全景：目标指标、架构、驱动方案、里程碑）
2. `config-schema.md`（用户自定义设置的数据契约，编码前必须冻结）
3. `ui-settings.md`（同一套设置的用户可见行为）
4. `amd-cpu.md` → 配套证据 `amd-facts.md`
5. `release-github.md`（发布与合规）

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
| ⑧ | 依赖策略（配置读写 / PresentMon 数据解析） | **已定**：配置读写用自研极简 JSON（零第三方依赖）；PresentMon 走逐帧 CSV（官方无 JSON 输出）；`wil` 是否引入见 §六「需要拍板」 | `config-schema.md` §0 不变量 6 |

## 五、已并入本集的内容（2026-10 精简对照）

| 原文档 | 内容去向 |
|---|---|
| `architecture.md`（进程/线程模型、模块边界、采样与内存预算、启动时序） | 主方案 §2、§6 |
| `cpu-intel.md`（Intel MSR / RAPL / DTS 细节、降级矩阵） | 主方案 §3.1 |
| `gpu-fans.md`（NVML / ADLX / IGCL 与 Super I/O 寄存器映射、已知坑、数据驱动层） | 主方案 §3.2、§3.3 |
| `fps-metrics.md`（FPS 采集路径、PresentMode 自检、1% Low 等口径） | 主方案 §3.4（`config-schema.md` 中的口径引用已改指此处） |
| `roadmap.md`（里程碑、工期、风险表） | 主方案 §8、§9、§12 |
| `privacy.md`（隐私与安全声明） | `release-github.md` §9（发布前仍需独立成面向用户的 `docs/privacy.md`） |
| `contributing-sensors.md`（芯片表贡献流程 + `--probe` 格式权威定义） | `release-github.md` §11（发布前仍需独立成面向用户的 `docs/contributing-sensors.md`） |

## 六、开放问题汇总（编码前需要关闭）

> 本节汇总了被删文档里的全部「待核实」项，避免信息随文件一起消失；发布/合规侧的 12 项见 `release-github.md` 附录 A。

### 6.1 需要用户拍板

| # | 问题 | 影响 |
|---|---|---|
| 1 | 是否引入 `wil`（MIT，唯一候选第三方头文件依赖） | 决定「零依赖」口径与产物体积（主方案 §5） |
| 2 | 是否新增 `gpu.selectedIndex`（`-1` = 自动选渲染 GPU） | 多 GPU / 混合输出机器需要指定监控对象；需新增 `gpu` 配置区块 + UI「关于/硬件」页（`config-schema.md` §9） |
| 3 | 是否提供英文 README | 影响国际用户量与 Super I/O 反馈量（`release-github.md` §10） |

### 6.2 需要实机或明确来源才能关闭

| # | 项目 | 现状 |
|---|---|---|
| 4 | Intel `cpu.voltage`（`0x198` 电压位域）与 `cpu.throttle.reason`（降频原因位） | 源文档未给位域定义；**核实前这两项在 Intel 上保持 N/A，绝不显示推算值** |
| 5 | Intel P/E 核拓扑识别（`GetLogicalProcessorInformationEx` + CPUID） | 影响「平均频率」口径（每核比值先算再平均） |
| 6 | PresentMon 接入形态：回调线程 vs 轮询消费 | 决定线程预算（当前按 UI / 采集 / FPS+统计 三线程）；以 M2 实际 API 为准 |
| 7 | 采集线程是否绑核（`SetThreadIdealProcessor`） | 影响 CPU 占用口径（≤0.3% / ≤1% 按单核占用率计），M0 实测后定 |
| 8 | 启动耗时分段预算（≤300 ms） | 设计目标，M0 用 QPC / WPA 打点校准 |
| 9 | IGCL / Level Zero 单次采样开销与采样周期 | 当前按 ADLX 同档（1 s）假设，需实测 |
| 10 | 数据驱动芯片包（`sensor-tables/*.json`）的签名 / 校验和格式 | 防止被篡改后写入 Ring0 端口；格式待定 |
| 11 | Intel Arc / Iris Xe 真机可得性 | 若无设备，IGCL 支持延后并在代码中标 `unverified` |
| 12 | Super I/O 跨主板差异返工量 | 逐板卡工作、历史上不可控，已排除在工期估算外（主方案 §9） |
| 13 | RTSS OSD 文本标签（颜色 / 字号 / 透明度 / 粗体）支持矩阵 | 决策 ④=B 的字号/颜色映射依赖它（`release-github.md` 附录 A-12） |
| 14 | AMD 家族 → SMU 邮箱三元组映射、各 SKU 的 Tjmax | 无一手来源；`amd-cpu.md` §9 已改为运行期探测 + 随包 SKU 表方案，证据见 `amd-facts.md` 第 5、7 项 |

## 七、术语

| 术语 | 含义 |
|---|---|
| 免驱模式 | 不安装任何 Ring0 驱动时的降级工作模式：CPU 频率走 PDH，GPU 全指标走 NVML/ADLX/IGCL，CPU 温度/功耗/风扇显示 N/A 并说明原因 |
| Ring0 通道 | 经 PawnIO 通用驱动读 MSR / 端口 I/O / SMN 的提权采集路径 |
| Tctl / Tdie | AMD 温度：Tctl 为带偏移的控制温度，Tdie 为实际结温（Tdie = Tctl − offset） |
| RAPL | Intel/AMD 的能量计数器机制，用「能量差分 ÷ 时间」算功耗 |
| PM 表 | AMD SMU 暴露的遥测结构化数据区（PPT/TDC/EDC、SVI2 电压、每 CCD 温度等） |
| 1% Low | 帧时间降序取最差 1%（N = ceil(0.01 × 帧数)）帧的平均帧时间 T₁，1% Low = 1000 / T₁ |
| DIP / Dp | 与设备无关的像素（1/96 英寸）；字号、间距、圆角一律以 Dp 表示，再按 DPI 缩放 |
