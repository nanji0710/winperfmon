# 里程碑、工期与风险（含 AMD 支持与配置系统）

> 本文由 winperfmon-dev-plan.md §8（里程碑与验收标准）、§9（风险与对策）拆出并更新；上游冲突时以本文为准，并回头修正主方案。
> 关联：`architecture.md`（采样/内存预算决定 DoD 的量测口径）、`cpu-intel.md` 与 `gpu-fans.md`（M1 的技术前提）、`fps-metrics.md`（M2 的口径前提）、`config-schema.md` 与 `ui-settings.md`（M2.5 的交付内容）。

## 1. 里程碑总表（M0–M4 为原方案原样保留，新增行以 **＋** 标注）

| 里程碑 | 工作量 | 内容 | 验收(DoD) |
|---|---|---|---|
| **M0 骨架 + 免驱指标** | 2–3 天 | CMake 工程、Win32+Direct2D 窗口/托盘、采集调度器、PDH CPU 频率、NVML 全套 GPU 指标、JSON 配置、CSV 日志 | 双显卡机器上数值与 HWiNFO 一致（±1 °C / ±50 MHz / ±5% 功耗）；内存 ≤15 MB |
| **M1 Ring0 传感器** | 3–5 天 | PawnIO 集成、MSR 模块（APERF/MPERF、THERM_STATUS、RAPL）、AMD SMN 温度、SuperIO 探测 + 风扇 RPM、豁免降级逻辑 | Intel 12–15 代 + AMD Zen3–5 上 CPU 温度/功耗/风扇全部可用；HVCI 开启不报错 |
| **＋ M1-A AMD CPU 支持**（**在 M1 内**，不单独成里程碑） | ＋2 天 | SMN 温度（`THM_TCON_CUR_TMP(0x00059800)`、**不硬编码偏移**）、AMD RAPL（`0xC0010299` 单位 + `0xC001029B` 能量，fam ≥ 17h）、PState/倍频（`0xC0010063`、`0xC0010064–0xC001006B`，17h–19h 倍频 = fid/did×2、Zen5(1Ah) = fid×0.05）、PawnIO `AMDFamily0F/10/17.p` + `RyzenSMU.p` 接入、家族/型号表 | **Tctl/Tdie 与 HWiNFO 对拍 ±2 °C；封装功耗 ±5%；频率 ±50 MHz；HVCI 开启不报错** |
| **M2 FPS 引擎与统计** | 3–5 天 | PresentMon Service 客户端（含子进程/自建 ETW 两条备选路径探测）、帧环形缓冲、会话统计、1%/0.1% Low、stutter 计数、CSV 导出 | 与 CapFrameX 同局对比 1% Low 偏差 ≤0.5 FPS；FPS 采集额外 CPU ≤1% |
| **＋ M2.5 配置系统**（插在 M0 与 M3 之间） | ＋5–7 天 | **配置数据结构 + 读写接口 + 设置界面**：`Config`/`ConfigStore`/校验修复/迁移链（`config-schema.md` §6）、自研最小 JSON 子集（约 300 行 + 单测）、原子写 + `.bak`、热重载、设置界面 7 个页面 + 实时预览 + 拖拽排序（`ui-settings.md`） | **改配置即时生效；非法配置自动修复不崩；热重载（外部改文件能生效且解析失败不覆盖）；只读模式可运行（目录不可写时内存配置 + 提示）** |
| **M3 Overlay 与交互** | 4–7 天 | **RTSS OSD 后端（主路径）**、自绘无注入 Overlay（回退）、布局配置、热键、**PresentMode 降级自检**、深色主题、异常提示 UI | 无边框游戏中稳定显示 4 h 无闪烁/掉帧；FSE 下正确提示"需 RTSS 后端"；检测到 `Independent Flip → Composed: Flip` 时能提示并一键关闭 |
| **M4 打包/兼容/文档** | 2–4 天 | 单 EXE 打包、PawnIO 检测与引导安装、兼容矩阵测试、崩溃转储、README/口径说明 | 干净系统免安装运行；降级路径全部走通并给出准确原因 |

## 2. M0 明细（骨架 + 免驱指标）

| 交付物 | 关键点 | 依据 |
|---|---|---|
| CMake 工程 | **C++20 / MSVC / CMake**，`/MT` 静态 CRT，`/O2 /GL /LTCG /GS` | 主方案 §5 |
| Win32 + Direct2D 窗口/托盘 | 纯 Win32 + Direct2D/DirectWrite 自绘，无 Qt/MFC/Electron | 主方案 §5 |
| 采集调度器 | `CreateWaitableTimerEx` + 事件等待（**不用轮询 Sleep 循环**）；分级定时；空闲降频 | `architecture.md` §4、§6 |
| PDH CPU 频率 | 免驱模式：`\Processor Information(_Total)\% Processor Performance` × 基频，或 `\Processor Information(*)\Processor Frequency`（MHz） | `cpu-intel.md` §1 |
| NVML 全套 GPU 指标 | 动态加载 `nvml.dll`，无需管理员 | `gpu-fans.md` §1.1 |
| JSON 配置 | **自研最小 JSON 子集**（约 300 行 + 单测），不引入 nlohmann | `config-schema.md` §0 不变量 6 |
| CSV 日志 | 异步落盘、双缓冲，采集线程零阻塞 | `architecture.md` §7 |

## 3. M1 明细（Ring0 传感器）

| 交付物 | 关键点 |
|---|---|
| PawnIO 集成 | `\\.\PawnIO` 打开设备 → 加载模块 → IOCTL 执行；调用范式 `WR0_ExecPawn(h, &h->pio_intel, "ioctl_read_msr", &in, 1, &out, 1, NULL)`；驱动回退链 `--driver=CPUZ162\|NwHwIo\|PawnIO\|NONE` |
| MSR 模块 | `IntelMSR.p`、`AMDFamily0F/10/17.p`、`RyzenSMU.p`、`LpcIO.p`；**WinRing0 绝对不用**（CVE-2020-14979，Defender 报 `HackTool:Win32/Winring0` / `VulnerableDriver:WinNT/Winring0`） |
| 频率/温度/功耗 | APERF/MPERF、THERM_STATUS（`0x19C`/`0x1B1`/`0x1A2`）、RAPL（`0x606`/`0x611`/`0x639`/`0x619`/`0x64D`） |
| AMD 侧 | 见 §1 的 M1-A 行；**Tctl 是控制温度不是结温**，禁止硬编码偏移 |
| SuperIO 探测 + 风扇 RPM | Nuvoton/ITE 优先；`RPM = 1350000 / (count × 分频)`；ISA 总线互斥 |
| 豁免降级逻辑 | 无驱动 → 免驱模式；反作弊受保护进程 → GPU-only + PresentMon FPS；提供「禁用 Ring0 驱动安装」开关（LHM PR #1777 做法） |

## 4. M2 明细（FPS 引擎与统计）

| 交付物 | 关键点 |
|---|---|
| PresentMon Service 客户端 | 动态加载系统已装 `PresentMonAPI2.dll`（SDK 默认在 `C:\Program Files\Intel\PresentMon\SDK`）；`PM_STATUS` 全量检查；`PM_STATUS_SESSION_NOT_OPEN` 自动重开会话 |
| 备选路径探测 | 子进程 `PresentMon.exe … --output_stdout --v2_metrics --no_console_stats`（逐帧 CSV，**无 JSON 选项**）；自建 ETW 仅长期选项 |
| 帧环形缓冲 | 16k~64k 帧时间（128~512 KB），零动态分配；`keepBufferFrames` 默认 65536 |
| 会话统计 | 每秒排序一次（≤ 0.5 ms）；1%/0.1% Low、均值（调和平均）、stutter、直方图（0.1 ms 桶、上限 200 ms） |
| CSV 导出 | 会话维度导出；导出**口径说明**（含 CapFrameX 口径差异提示） |

## 5. M2.5 明细（配置系统，新增里程碑）

| 交付物 | 内容 | 工作量依据 |
|---|---|---|
| 数据结构与接口 | `config-schema.md` §6 全部接口：枚举/值类型/`Config`/`MetricCatalog()`/`ValidateAndRepair`/`ResolveConfigPaths`/`LoadConfig`/`SaveConfig`/`ConfigToJson`/`ConfigFromJson`/`ConfigStore`/`ApplyCommandLineOverrides` | 约 2–3 天 |
| JSON 层 | 自研最小 JSON 子集（解析 + 定序输出），纯函数、可脱离 Windows API 单测；原子写 + `.bak`；256 KB/深度 16/字符串 4 KB 上限 | 含在上面 2–3 天内 |
| 校验与迁移 | `ValidationPolicy` 四个开关；`Migrate_v1_to_v2` 迁移链；指标别名表；前向兼容（未知字段忽略、未知指标丢弃并记诊断） | 含在上面 2–3 天内 |
| 设置界面 | `ui-settings.md` 的 7 个页面（布局/指标/外观/窗口与置顶/采样/帧率/关于）+ 底部固定条 + 实时预览 + 拖拽排序 + 键盘可达 + 黄条诊断 | 约 3–4 天 |
| 即时生效矩阵 | `ui-settings.md` §9：`layout/metrics/colors/text` 直接重绘；`window.topmostPolicy` 等重建窗口样式；`update.*` 重设定时器；`fps.*` 重开会话；`text.fontFamily` 重建文本格式 | 含在设置界面工期内 |

**M2.5 的四条验收**：①改配置即时生效（< 1 帧内可见）；②非法配置自动修复不崩（夹紧/回退 + 诊断，绝不覆盖用户文件）；③热重载（mtime 轮询 2000 ms 生效，解析失败保留当前配置）；④只读模式可运行（目录不可写 → 内存配置 + 「设置无法保存」提示 + 退出前「另存为」）。

## 6. M3 明细（Overlay 与交互）

> **决策 ④=B 更新（+1–2 天）：RTSS OSD 为主路径。**
> - **主路径 `RtssOsdBackend`**：写 `RTSSSharedMemoryV2`（按 `dwSignature`/`dwVersion`/`dwAppEntrySize`/`dwOSDEntrySize`/`dwOSDArrOffset`/`dwOSDFrame` 协商，写 `szOSD` 后递增 `dwOSDFrame`），**不注入**；真 FSE 可见。
> - **回退 `NativeOverlayBackend`**：自绘置顶窗口，仅在 DWM 合成路径（无边框 / eFSE / 窗口化）可见。
> - **选择**：真 FSE（PresentMon `PresentMode == Hardware: Legacy Flip`）且 RTSS 共享内存可用 → RTSS；否则自绘；`window.overlay.backend` 可强制（见 `config-schema.md` §3.6）。
> - **文本组装**：`layout.orientation` 映射分隔符（横向同行拼接、纵向换行）；`showLabels`/`showUnits` 与自绘一致；颜色/字号按 RTSS 文本标签近似映射（`<C=RRGGBB>` / `<S=n>` / `<A=n>` / `<B=n>` 的支持矩阵**待核实**）。
> - **边界**：帧统计仍全部由 PresentMon 负责，RTSS 只负责显示，1% Low 口径不因引入 RTSS 而改变。
> - **DoD 增补**：真 FSE 游戏（老 DX11 关闭"全屏优化"、老 DX9）下 RTSS 后端连续显示 4 h 无异常；RTSS 未装/未运行/中途退出时自动回退且不崩；提示文案准确。

| 交付物 | 关键点 |
|---|---|
| 无注入 Overlay | `WS_EX_LAYERED \| WS_EX_TRANSPARENT \| WS_EX_TOOLWINDOW \| WS_EX_TOPMOST` + Direct2D；`SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 重新置顶 |
| 布局配置 | 横向/纵向、分组、间距、对齐、换行、`hideMissing`（`ui-settings.md` §3） |
| 热键 | `RegisterHotKey`（**不要用低级键盘钩子**）：设置 `Ctrl+Alt+F10`、显示/隐藏 `Ctrl+Alt+F11`、锁定位置 `Ctrl+Alt+F12`、开始/停止采集 `Ctrl+Alt+F9` |
| **PresentMode 降级自检** | 见 `fps-metrics.md` §3（M3 DoD 硬要求） |
| RTSS OSD 后端 | 映射 `RTSSSharedMemoryV2`，按 `dwSignature`/`dwVersion`/`dwAppEntrySize`/`dwOSDEntrySize`/`dwOSDArrOffset`/`dwOSDFrame` 协商；写 `szOSD` + 递增 `dwOSDFrame`，**无需注入** |
| 已知做不到 | 真独占全屏 `Hardware: Legacy Flip` 覆盖不了（issue #212）；置顶窗口会把 iFlip 踢成 `Composed: Flip`（issue #235/#367/#689） |

## 7. M4 明细（打包/兼容/文档）

> **决策 ③=B 更新（+1–2 天）：随包分发 PawnIO 并引导静默安装。**
> ①构建期从官方 Release 下载 PawnIO 安装包并登记 SHA-256；②首次运行"Ring0 组件"引导流程（说明用途 → 用户确认 → 静默安装 → 重新探测 → 拒绝则免驱模式）；③GPL-2.0／LGPL-2.1 合规物料随 Release 打包（许可证原文 + 源码获取说明 + 关于页清单）；④安装/卸载回归（已装则跳过、卸载默认不移除 PawnIO、拒绝安装后功能降级正确）；⑤发布物由"单 EXE"调整为"便携 ZIP"（**EXE 仍 ≤5 MB 且免安装运行**）。

| 交付物 | 关键点 |
|---|---|
| 打包（便携 ZIP） | EXE ≤ 5 MB，`/MT` 静态 CRT，无 .NET/WebView2/Python；ZIP 另含 PawnIO 官方安装包与 GPL-2.0／LGPL-2.1 合规文本（决策 ③=B） |
| PawnIO 检测与引导安装 | **只检测 + 引导 + 状态回报**，不随包分发二进制（许可与安全双重考虑） |
| 兼容矩阵测试 | Intel 12–15 代；AMD Zen3–5；NVIDIA/AMD/Intel 显卡；HVCI 开/关；有/无 PawnIO；有/无 PresentMon |
| 崩溃转储 | 本地 dump + 诊断包（不含个人信息） |
| README/口径说明 | 每项指标的读取方式、1% Low 算法、与 HWiNFO/CapFrameX 的对照测试报告 |

## 8. 工期汇总与新估算

| 项 | 原估算 | 调整 |
|---|---|---|
| M0 | 2–3 天 | 不变 |
| M1（含 AMD 支持） | 3–5 天 | **＋2 天** → 5–7 天（AMD 为增量：M1 原本已含 AMD SMN 温度） |
| M2 | 3–5 天 | 不变 |
| **M2.5 配置系统（新增）** | — | **＋5–7 天**（数据结构与接口 2–3 天 + 设置界面 3–4 天） |
| M3 | 3–5 天 | **＋1–2 天 → 4–7 天**（决策 ④=B：RTSS OSD 后端升为主路径，自绘 Overlay 降为回退；RTSS 文本标签映射与真 FSE 真机验证都要做） |
| M4 | 2–4 天 | 不变 |
| **合计** | **13–22 天 ≈ 3–5 周**（单人，含联调） | **22–35 天 ≈ 4.5–7 周**（按 5 工作日/周推算；已含决策 ③=B 的 M4 +1–2 天与决策 ④=B 的 M3 +1–2 天） |

**估算依据与明确不计入的部分**

1. AMD 增量只算「SMN 温度 + RAPL 能量 + PState/倍频 + 家族表 + PawnIO AMD 模块接入」——M1 已含 AMD SMN 温度，故为增量而非全量。
2. 配置系统的数据结构/接口按 `config-schema.md` 的接口清单估 2–3 天；设置界面按 `ui-settings.md` 的 7 个页面 + 实时预览 + 拖拽排序估 3–4 天。
3. **不计入**：主方案 §11 的「数据驱动层热更新」、`release-github.md` 的 CI/Release 流程、以及 **Super I/O 主板差异返工**（逐板卡工作，历史上不可控）→ 后者无实测数据，**待核实**。

## 9. 风险与对策（原样保留）

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

## 10. 新增风险：AMD 特有

| 风险 | 影响 | 对策 |
|---|---|---|
| **PM 表按家族/型号维护成本** | 新 CPU 发布即可能读错（17h/19h/1Ah 索引不同） | 家族表 + 型号表数据驱动；`RyzenSMU.p` 可用性探测；未知型号不猜（显示 N/A） |
| **AGESA 行为差异** | 同一型号不同 BIOS 版本读数不同（尤其 Tctl 偏移） | **禁止硬编码偏移**；按型号/AGESA 版本建表；未知组合显示 `~` 前缀 |
| **X3D 型号温度偏移** | 5800X3D/7800X3D 上 Tctl 与 CCD 差很多 → 直接照抄 Tctl 会误导 | 显式标注「Tctl（控制温度）」与「Tdie（结温）」的区别；`cpu.temp.package` 的 tooltip 说明当前读数属于哪一种 |
| AMD RAPL 计数差异 | `0xC001029B` 为 32 位，回绕处理与 Intel 相同但单位寄存器在 `0xC0010299` | 与 Intel 共用回绕与单位换算代码路径，单测覆盖两家 |
| 核级功耗缺失 | AMD 无核级 RAPL → `cpu.power.cores` 恒缺 | 缺失原因 id 用 `vendor_unsupported`（不是 `no_ring0`），避免用户以为装了驱动就有了 |

## 11. 新增风险：配置系统

| 风险 | 影响 | 对策 |
|---|---|---|
| 配置损坏（写盘中断/磁盘满/手工编辑错） | 用户设置丢失、启动失败 | 原子写（`MoveFileExW` + `MOVEFILE_WRITE_THROUGH`）+ `.bak` 一代；**解析失败绝不覆盖用户文件**；退出前提示「另存为」 |
| 跨版本迁移（字段改名/语义变化） | 老配置读不进、设置被重置 | `schemaVersion` 迁移链（逐级纯函数、可单测）+ 指标别名表（如 `"cpu.freq": "cpu.freq.avg"`）+ 前向兼容（未知字段忽略、未知指标丢弃并记诊断） |
| `schemaVersion` 过高（用户降级程序） | 新字段被旧版写回后丢失 | 只读加载、禁止自动写回、Error 诊断并提示升级 |
| 全部指标被隐藏 | 界面空白，用户以为坏了 | 目录中第一个默认可见项强制置 `true` + Warning（`config-schema.md` §3.3） |
| 热重载竞态 | UI 线程与监听线程同时改配置 | 发布 `Revision` + `shared_ptr` 原子快照；订阅回调要求 UI 侧自行 marshal；仅在实际变更时回调 |
| 对比度/可读性 | 自定义颜色导致文字不可读 | 对比度 < 3:1 时行内黄色提示（不阻断）；缩放后字号不低于 `text.minSizeDp` |

## 12. 前置条件与决策影响（阻塞项）

> **决策 ③=B / ④=B 已落定（解除两项阻塞）：** PawnIO 改为随包分发 + 引导静默安装（M4 增项）；真 FSE 可见性为硬需求 → RTSS OSD 为主路径（M3 增项）。剩余阻塞项见下表。

| # | 未决事项 | 阻塞的里程碑 | 影响 |
|---|---|---|---|
| 1 | ~~**PawnIO 分发方式**~~ ✅ **已决：随包附带官方安装包 + 引导静默安装**（决策 ③=B） | M1 / M4 | M4 增项：构建期下载并 SHA-256 锁定官方安装包、首次运行引导安装（可拒绝）、GPL-2.0／LGPL-2.1 合规物料随 Release 打包（见 `release-github.md` §3.5）；**仍不拆发驱动 `.bin`/`.sys`** |
| 2 | ~~**真独占全屏（FSE）下 Overlay 可见性**~~ ✅ **已决：硬需求 → RTSS OSD 为主路径**（决策 ④=B） | M3 | 自绘 Overlay 降为回退；M3 +1–2 天（见 §6）；RTSS 标签支持矩阵待核实 |
| 3 | ~~**GPU 三家还是先 NVIDIA**~~ ✅ **已决：三家都做** | M0/M1 | M1 需备齐 ADLX/IGCL 环境：ADLX 的 `IADLXGPUMetricsSupport` 探测、IGCL 的 `ctl*` 系列都要真机验证；**无 Intel 真机时相关代码标 `unverified`** 并在 Release Notes 说明 |
| 4 | ~~**产品名 / 仓库名**~~ ✅ **已决** | M0、M2.5 | `winperfmon`：EXE `winperfmon.exe`、`%APPDATA%\winperfmon\config.json`、仓库 `winperfmon` |
| 5 | Intel Arc / Iris Xe 真机可得性 | M0/M1 | 若无设备，IGCL 支持只能延后并在代码里标 `unverified` → **待核实**（当前环境是否有 Intel 独显/核显测试机未确认） |

## 13. DoD 速查（发布前逐条打勾）

| # | 验收项 | 目标 |
|---|---|---|
| 1 | 常驻内存 | ≤ 15 MB（纯监控）/ ≤ 25 MB（含 FPS 采集） |
| 2 | CPU 占用 | ≤ 0.3%（1 Hz 采样）/ ≤ 1.0%（游戏内 FPS 采集） |
| 3 | 冷启动 | ≤ 300 ms（`architecture.md` §8 的分段预算） |
| 4 | 体积 | 单 EXE ≤ 5 MB |
| 5 | 系统支持 | Win10 1809+ / Win11；HVCI 开启不报错 |
| 6 | 数值准确度 | HWiNFO 对拍：±1 °C / ±50 MHz / ±5% 功耗（AMD：±2 °C） |
| 7 | FPS 口径 | CapFrameX 同局 1% Low 偏差 ≤ 0.5 FPS |
| 8 | 降级路径 | 免驱模式、无 PresentMon、服务重启、反作弊受限四种场景都走通并给出准确原因 |
| 9 | Overlay 自检 | 能检测 `Independent Flip → Composed: Flip` 并一键关闭 |
| 10 | 配置系统 | 即时生效 / 非法自动修复 / 热重载 / 只读模式可运行 |
