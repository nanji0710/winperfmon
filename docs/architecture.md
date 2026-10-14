# 架构设计：进程模型、模块边界、线程与性能预算

> 本文由 winperfmon-dev-plan.md §2 拆出并细化（并并入 §6 采样策略与性能预算）；上游冲突时以本文为准，并回头修正主方案。
> 关联：`config-schema.md`（配置数据契约）、`cpu-intel.md`、`gpu-fans.md`、`fps-metrics.md`、`roadmap.md`。
> 约定：尺寸一律用 **Dp**；术语（免驱模式 / Ring0 通道 / Tctl/Tdie / PM 表 / 1% Low）以 `README.md` §五为准。

## 1. 分层总览

```
┌────────────────────────────────────────────────────────────────┐
│ UI 层    主窗口(卡片/曲线) │ 托盘 │ Overlay(置顶分层窗口) │ RTSS OSD(可选后端) │
├────────────────────────────────────────────────────────────────┤
│ 会话与统计  会话管理 │ 帧时间环形缓冲 │ 百分位统计 │ CSV/JSON 导出 │
├────────────────────────────────────────────────────────────────┤
│ 采集调度器  分级定时器(100ms~5s) │ 空闲降频 │ 异常值校验 │ 设备热插拔 │
├────────────────────────────────────────────────────────────────┤
│ ISensorProvider 抽象                                           │
│  ├ CpuProvider  Intel MSR / AMD SMN+MSR  (+PDH 降级)           │
│  ├ GpuProvider  NVML / ADLX / IGCL(含 Level Zero 备用)          │
│  ├ FanProvider  SuperIO(Nuvoton/ITE) / EC / GPU 风扇            │
│  └ FpsProvider  PresentMon Service API │ 子进程 │ 自建 ETW      │
├────────────────────────────────────────────────────────────────┤
│ 访问层  Ring0: PawnIO 模块(MSR / PortIO / SMN)                  │
│         User: PDH / WMI / NVML.dll / ADLX.dll / IGCL.dll        │
│         ETW : PresentMon 服务(SYSTEM) 或本进程 ETW 会话          │
└────────────────────────────────────────────────────────────────┘
```

| 层 | 职责 | 允许依赖 | 禁止 |
|---|---|---|---|
| UI 层 | 绘制、输入、设置界面、热键、托盘、Overlay/RTSS 后端 | 只读快照、`ConfigStore`（只读侧） | 直接调用 provider 或访问层；在 UI 线程做设备 I/O |
| 会话与统计 | 会话生命周期、帧时间环形缓冲、百分位/均值/卡顿计算、导出 | 帧环形缓冲、`ConfigStore` 快照 | 触碰窗口句柄；每帧动态分配 |
| 采集调度器 | 分级定时器、空闲降频、异常值校验、设备热插拔 | `ISensorProvider` 接口 | 感知具体厂商 API |
| ISensorProvider | 把厂商 API 归一为**同一套指标 ID**（目录见 `config-schema.md` §4） | 访问层薄封装 | 生成用户文案（只回状态码 + 缺失原因 id） |
| 访问层 | Ring0（PawnIO IOCTL）、User（PDH/WMI/厂商 DLL）、ETW | 系统 API、`\.\PawnIO` 设备 | 含业务逻辑；反向依赖上层 |

**依赖方向铁律**：依赖只能自上而下。访问层与 provider 不得 include UI 头；`ISensorProvider` 返回**缺失原因 id**（如 `no_ring0` / `vendor_unsupported` / `sensor_absent`），文案由 UI 层按 id 映射 —— 这是免驱模式能统一解释「为什么这里显示 N/A」的前提。

## 2. 进程模型：v1 单进程 → v2 服务拆分

| 版本 | 进程 | 权限 | 说明 |
|---|---|---|---|
| **v1** | 单进程 | `requireAdministrator`（Ring0 与 ETW 都需要） | 实现最简单，先跑通全链路 |
| **v2（推荐尽早拆）** | `pmsvc`（SYSTEM 服务，独占所有 Ring0/驱动访问）+ `pmui`（普通权限 UI/Overlay） | UI 不提权 | 命名管道 + 共享内存 IPC |

**v2 的三个收益**（决定了长期结构）：
1. UI 不需要提权 —— Overlay 在游戏上更安全，反作弊面更小；
2. 驱动访问与 UI 崩溃隔离：UI 崩了不影响采集，采集崩了 UI 还能报错；
3. Ring0 通道集中在单一进程，`\.\PawnIO` 的句柄与模块生命周期只需管理一份。

**IPC 设计约束**

| 项 | 设计 | 理由 |
|---|---|---|
| 通道 | 命名管道（控制/命令）+ 共享内存（高频快照） | 快照零拷贝：UI 直接读「传感器快照」双层缓冲 |
| 消息方向 | `pmui → pmsvc`：订阅/取消/Zoom 配置变更/请求重连；`pmsvc → pmui`：快照发布通知、状态、诊断 | 采集侧单向发布，避免竞争 |
| 安全 | 管道 ACL 只允许当前用户 SID；服务不接受远程连接 | 防止本机其他用户注入配置或读遥测 |
| 重连 | `pmui` 检测管道断开 → 退避重连（1 s / 2 s / 5 s / 10 s 封顶）并显示「采集服务未运行」 | 不静默失败 |
| 降级 | 服务不可用且用户未提权 → 进入**免驱模式**（CPU 频率走 PDH，GPU 走 NVML/ADLX/IGCL，CPU 温度/功耗/风扇显示 N/A 并说明原因） | 与主方案 §4「免驱动轻量模式」一致 |

## 3. 模块划分与依赖方向

> **决策 ④=B 更新：OSD 输出层拆成两个实现，接口 `IOsdBackend`。**
> ① **主路径 `RtssOsdBackend`**：写 `RTSSSharedMemoryV2`，不注入，真 FSE 可见；② **回退 `NativeOverlayBackend`**：自绘置顶窗口（`WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_TOPMOST` + Direct2D），仅 DWM 合成路径可见。
> 选择逻辑：`window.overlay.backend = auto`（默认）时，检测到真 FSE（PresentMon `PresentMode == Hardware: Legacy Flip`）且 RTSS 共享内存可用 → ①；否则 ②；用户可强制。上层（指标组装、文本排版、刷新调度）与后端无关。

| 模块 | 关键避免的事 | 对外产出 | 归属文档 |
|---|---|---|---|
| `wpm::ui` | 不缓存"当前值"（每次从快照读） | 窗口/Overlay/RTSS 文本 | 本文 §5、`ui-settings.md` |
| `wpm::stats` | 不在采集线程排序 | `StatsSnapshot`（均值/百分位/卡顿/直方图） | `fps-metrics.md` |
| `wpm::sched` | 不用轮询 `Sleep` 循环 | 定时触发 + 降频 | 本文 §4、§6 |
| `wpm::sensors` | 不假设任何传感器一定存在 | `SensorSnapshot`（指标 ID → 值 + 状态） | `cpu-intel.md`、`gpu-fans.md` |
| `wpm::access` | 不含业务判断 | `READ_MSR` / `PORT_IO` / 厂商 DLL 调用的薄封装 | 本文章 §2、主方案 §4 |
| `wpm::config` | 热路径不加锁 | `shared_ptr<const Config>` 快照 | `config-schema.md` §6 |

## 4. 线程模型

| 线程 | 数量 | 职责 | 硬性禁止 | 与外界通信 |
|---|---|---|---|---|
| **UI 线程** | 1（创建窗口者） | 消息泵、Direct2D 绘制、设置界面、热键、托盘 | 不做设备 I/O、不 sleep 等数据、不排序帧缓冲 | 读双缓冲快照（无锁）；改配置走 `ConfigStore::Apply`/`Mutate` |
| **采集线程** | 1 | 按分级定时器轮询 MSR / NVML / ADLX / IGCL / Super I/O；异常值校验 | 不碰窗口句柄、堆分配、不写日志文件 | 写「传感器快照」双层缓冲 + `std::atomic` 序号发布 |
| **FPS 线程** | 1 | PresentMon 会话生命周期、帧事件消费/轮询、写帧环形缓冲、维护 `PresentMode` 状态 | 不做排序/百分位（耗时工作交给统计线程） | 单写者写环形缓冲；`PresentMode` 变化发状态事件 |
| **统计线程** | 1（v2 可并入 FPS 线程） | 每秒一次排序 + 百分位/均值/卡顿计算；滚动窗（最近 10 s） | 不做 I/O、不分配（复用 `[ThreadStatic]` 式预分配缓冲） | 写「统计快照」双层缓冲 |
| **服务线程**（v2 `pmsvc`） | 1 + 管道 I/O | Ring0/驱动独占访问、管道服务端、快照共享内存发布 | 不渲染 | 命名管道 + 共享内存 |

- **定时器**：`CreateWaitableTimerEx` 高精度定时器 + 事件等待（**不用轮询 `Sleep` 循环**）—— 空闲时零唤醒，这是 CPU ≤0.3% 的前提。
- **共享数据**：双缓冲快照 + `std::atomic` 序号发布，读侧无锁；写侧只写"非当前读"的那一份。
- **v1 线程预算**：3 个工作线程（UI / 采集 / FPS+统计）。主方案 §6 写的第三条线程是 **PresentMon 回调线程**；而 §3.4 又描述为**轮询消费帧数据** —— 这两种模型对线程数与唤醒频率的影响不同，**待核实**（M2 接入 SDK 时以实际 API 形态为准，若为纯轮询则不需要回调线程，FPS 线程自己按 200 ms 拉遥测）。
- **订阅回调线程语义**：`ConfigStore::Subscribe` 的回调在「发布变更的线程」上执行（可能是文件监听所在的采集线程）→ **UI 侧必须自行 marshal 到 UI 线程**，不得在回调里直接操作窗口。

## 5. 数据流与数据所有权

| 数据 | 生产者 | 消费者 | 同步方式 | 生命周期 |
|---|---|---|---|---|
| 传感器原始读数 | 采集线程 | provider → 快照 | 线程内顺序 | 单次采样 |
| 传感器快照 | 采集线程 | UI、Overlay、CSV 导出 | 双缓冲 + 原子序号 | 进程级 |
| 帧时间 | FPS 线程 | 统计线程、曲线、导出 | 环形缓冲（单写者） | 会话级 |
| 统计快照 | 统计线程 | UI、Overlay、RTSS 后端 | 双缓冲 + 原子序号 | 会话级 + 滚动窗 |
| 配置 | UI 线程（设置）或监听线程（热重载） | 所有线程（只读） | `shared_ptr<const Config>` 原子发布 | 进程级 |
| 诊断 | 各层 | UI（黄条/关于页） | 追加式环形 | 进程级 |

**关键约定**：Overlay 只读「统计快照 + 传感器快照」，不自己算数；`presentMode` 取自 FPS 线程维护的状态，用于 Overlay 降级自检（主方案 §7 对策 1）。

## 6. 采样策略与 CPU 预算

### 6.1 分级采样表（主方案 §6 原值）

| 采样项 | 周期 | 单次成本预算 | 执行线程 |
|---|---|---|---|
| MSR 读取（频率/温度/RAPL） | 500 ms（频率 200 ms，仅性能视图开启时） | 1–5 µs/次 | 采集线程 |
| NVML | 500 ms | 5–50 µs/次 | 采集线程 |
| ADLX | 1 s（初始化重，缓存 `IADLXPerformanceMonitoringServices` 实例） | 0.1–1 ms | 采集线程 |
| SuperIO | 1 s | 10–100 µs | 采集线程 |
| PresentMon 帧事件 | 事件驱动（帧级） | 回调 + 每 200 ms 拉一次遥测 | FPS 线程 |
| UI 刷新 / Overlay | 4 Hz / 2–4 Hz | 仅在可见时重绘 | UI 线程 |

### 6.2 与用户可调配置的映射（`config-schema.md` §3.7）

| 配置字段 | 取值范围 | 默认 | 与上表的关系 |
|---|---|---|---|
| `update.sensorIntervalMs` | 200–5000 | 500 | 上表 MSR/NVML「500 ms」是**默认档**；ADLX/SuperIO「1 s」实现为 `2 × sensorIntervalMs` 或独立档位（ADLX 初始化重，不宜跟随 200 ms） |
| `update.overlayRefreshFps` | 1–10 | 2 | 上表「Overlay 2–4 Hz」的下界；非注入 Overlay 建议不超过 2 Hz（主方案 §7 对策 3） |
| `update.panelRefreshFps` | 1–30 | 4 | 上表「UI 4 Hz」 |
| `update.pauseWhenHidden` | bool | true | 「界面隐藏时暂停传感器采样（仅保留 FPS 会话统计）」 |
| `update.lowPowerOnBattery` | bool | false | 电池供电时把 `sensorIntervalMs` 乘一个降频系数（默认 4×，上限仍受 5000 ms 约束） |

### 6.3 CPU 预算的上界推算（设计目标，非实测）

| 场景 | 目标 | 推算 | 结论 |
|---|---|---|---|
| 纯监控（1 Hz 采样） | ≤ 0.3% | 按 `sensorIntervalMs = 500`：MSR 2 次/s × 5 µs + NVML 2 次/s × 50 µs + ADLX 1 次/s × 1 ms + SuperIO 1 次/s × 100 µs ≈ 1.21 ms/s ≈ **0.12%**；再计上 UI 4 Hz 重绘与诊断/日志 | 有裕量 |
| 游戏内 FPS 采集 | ≤ 1.0% | 纯监控 + 帧级事件消费 + 每 200 ms 拉遥测（5 次/s）+ 统计线程每秒一次排序 65 536 帧（≤ 0.5 ms） | 有裕量，但**排序是最大单项** |
| 用户把间隔调到 200 ms | 无固定目标 | MSR/NVML 成本 ×2.5 → ≈ 0.3%，ADLX 仍 1 s | 设置界面必须显示**运行期实测**的「预估额外 CPU 占用」（`ui-settings.md` §7），而不是展示常数 |

> 代价口径：以上百分比按「单核占用率」计（与 Windows 任务管理器一致），采集线程固定绑在一个非 P-core 之外的核上还是允许迁移 —— **待核实**（需在 M0 用 `SetThreadIdealProcessor` 对照实测，再决定是否绑核）。
> ADLX 是唯一 0.1–1 ms 量级的调用：必须**复用**已初始化的 `IADLXPerformanceMonitoringServices` 实例，禁止每次采样重新初始化。

## 7. 内存预算（常驻 ≤ 15 MB / 含 FPS ≤ 25 MB）

口径：只计**私有工作集**，系统 DLL 映像（`nvml.dll`、`ADLX.dll`、`IGCL.dll`）按共享页计、不计入本预算。

| 项 | 预算 | 依据 |
|---|---|---|
| 进程基线（EXE 映像 + `/MT` 静态 CRT + Win32 最小 UI） | ≈ 4 MB | 主方案 §5：产物 2–4 MB、工作集 ≈ 15 MB |
| Direct2D/DirectWrite 资源与字形缓存 | 3–5 MB | 首次绘制后趋于稳定 |
| 帧时间环形缓冲 65 536 帧 × 8 B | 512 KB | `config-schema.md` §3.8：65536 帧 ≈ 1080p60 下 18 分钟；内存 512 KB |
| 帧时间直方图 2 000 桶 × 4 B | 8 KB | 0.1 ms 桶、上限 200 ms（`fps-metrics.md`） |
| 配置 + 校验诊断 | ≤ 256 KB | `config-schema.md` §1 文件上限 256 KB |
| 日志环形缓冲 + CSV 双缓冲 | 128–512 KB | 异步落盘、采集线程零阻塞 |
| 传感器/统计双层缓冲 | < 128 KB | 固定大小结构体数组 |
| **小计（纯监控）** | **≈ 9–11 MB** | 验收余量针对 DLL 映射与 D2D 峰值 |
| FPS 采集增量（会话缓冲、SDK 客户端、更大的 `keepBufferFrames`） | +2 MB（`keepBufferFrames = 262144` → 2 MB） | `config-schema.md` §3.8 上限 262144 |
| **合计上限** | **15 MB / 25 MB** | 主方案 §1 硬指标 |

**验收含义**：M0 的 DoD「内存 ≤ 15 MB」应按上表逐项量测（任务管理器 + `Get-Process` 私有工作集），超预算优先砍 D2D 缓存与日志缓冲，**不砍**环形缓冲（那是 FPS 统计的精度来源）。

## 8. 启动时序（冷启动 ≤ 300 ms 拆解）

| 阶段 | 预算 | 内容 | 失败时 |
|---|---|---|---|
| 1. 进程创建 + CRT 初始化 | ≤ 30 ms | `/MT` 静态 CRT，无运行时初始化、无 .NET/WebView2/Python | — |
| 2. 最小 UI 先出 | ≤ 50 ms | 创建窗口 + 静态骨架（卡片边框/占位符），**不等待任何传感器** | 无 |
| 3. 配置加载 | ≤ 10 ms | `config.json`（≤ 256 KB）用自研 JSON 子集解析 + 校验修复 | 解析失败 → 保留内存配置 + 黄条，不覆盖文件 |
| 4. Direct2D 设备/资源初始化 | ≤ 60 ms | 首次绘制前建立设备与文本格式 | 退回 GDI 绘制并记诊断 |
| 5. PDH CPU 频率/占用首采 | ≤ 80 ms | PDH 计数器首个样本 | 期间显示占位符 `—` |
| 6. NVML 初始化 + 首次读数 | ≤ 100 ms | 与 UI 绘制**并行/异步** | GPU 项显示缺失原因 id |
| 7. 托盘图标 + 热键注册 | ≤ 20 ms | `RegisterHotKey` | 提示热键冲突 |

- **分级加载**：第 5–6 步异步补齐，UI 可交互时点（≤ 300 ms）只保证「窗口出来了、配置生效了」；传感器数值允许在 1 s 内逐项补齐，缺失期间显示占位符而**不是 0**（参考项目 PerfWindow 的分级启动经验：先 CPU+RAM，再异步启主板/GPU；主方案 §10 记录其「按需启动」设计）。
- 免驱模式下第 5 步之后**没有** Ring0 初始化，启动更快；反之有 Ring0 时 PawnIO 设备打开与模块加载应放在第 6 步之后异步进行，不阻塞 UI 出现。
- 各项耗时预算为**设计目标**，M0 需用 `QueryPerformanceCounter` 打点（或 WPA）校准 → **待核实**。

## 9. 配置系统在架构中的位置

| 关注点 | 结论（详见 `config-schema.md` §6） |
|---|---|
| 数据契约 | `Config` 顶层：`schemaVersion` / `meta` / `layout` / `metrics` / `colors` / `text` / `window` / `update` / `fps` |
| 读取模型 | `ConfigStore::Snapshot()` 返回 `shared_ptr<const Config>`，**任意线程、无锁**；采集热路径只用这个 |
| 写入模型 | `Apply` / `Mutate` 只在 UI 线程（或热键处理）调用，内部短临界区；连续改动由 `SetAutoSave(on, debounceMs = 500)` 合并成一次写盘 |
| 热重载 | `SetFileWatch(on, pollMs = 2000)`：由**采集线程按 2 s 轮询 mtime**，不新增线程 |
| 原子写 | 写 `config.json.tmp` → `FlushFileBuffers` → `MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING \| MOVEFILE_WRITE_THROUGH)`，旧文件轮转 `.bak` 一代 |
| 失败语义 | 解析失败**绝不覆盖**用户文件；`schemaVersion` 高于本程序 → 只读加载、禁止自动写回 |
| 依赖约束 | 不变量 6：**零第三方依赖**，配置用自研最小 JSON 子集（约 300 行 + 单测） |
| 生效路径 | 配置变更 → 发布新 `Revision` → UI 重算布局/重绘；`window.topmostPolicy` 等重建窗口样式；`update.*` 重设定时器周期（`ui-settings.md` §9 即时生效矩阵） |

## 10. 空闲降频与省电

| 触发条件 | 行为 | 依据 |
|---|---|---|
| 无前台全屏应用 / 窗口隐藏 | 采样降到 **2 s** | 主方案 §6 |
| 最小化到托盘 | 停止 UI 重绘；**若在录制则保留 FPS** | 主方案 §6 |
| `update.pauseWhenHidden = true` | 隐藏时暂停传感器采样（仅保留 FPS 会话统计） | `config-schema.md` §3.7 |
| 电池供电（`lowPowerOnBattery = true`） | `sensorIntervalMs` 乘降频系数 | `config-schema.md` §3.7 |
| 设备热插拔（GPU 增减、显示器切换） | 重新枚举 GPU 与显示器；`monitorId` 失效则回退主显示器 | 采集调度器职责（§1） |

## 11. 错误处理与降级边界

| 情形 | 架构层应对 | 用户可见行为 |
|---|---|---|
| 无 Ring0 通道（用户未装 PawnIO / 拒绝安装） | provider 回 `no_ring0` → 免驱模式 | CPU 温度/功耗/主板风扇 `N/A` + 原因说明（不是 0） |
| 反作弊受保护进程 | 检测到后自动切 **GPU-only + PresentMon FPS**，绝不注入 | 明确提示当前处于受限模式 |
| PresentMon 未安装 / 版本不符 | 三级探测：SDK DLL → 子进程 → 提示安装 | 帧率页信息条 + 安装指引（不自动下载） |
| PresentMon 服务重启 | 旧会话返回 `PM_STATUS_SESSION_NOT_OPEN` → 自动重开会话 | 帧率项短暂显示 `—` |
| SCM recovery 用尽（2 s → 15 s → 60 s → 5 min，第 5 次后保持停止；30 min 无失败后计数重置） | 我方退避重试 + 状态上报 | 明确提示「帧率采集服务未运行」而非静默空值 |
| 传感器异常值 | 合理性校验（温度 −40…125 °C、功耗 0…1000 W、频率 ≤ 睿频上限） | 打标 `N/A(异常)` 并记录一次诊断 |
| 配置损坏 | 保留内存配置、不写盘、黄条 | 「用默认值重写」需用户点击 |
| UI 崩溃（v2） | 采集进程继续，UI 重启后重新挂接快照 | 采集不中断 |

## 12. 与主方案的冲突记录（需回头修正主方案）

| # | 冲突 | 处理 |
|---|---|---|
| 1 | 主方案 §5 写「依赖 `wil`（RAII/资源）、`nlohmann/json` 或 `simdjson`（配置与 PresentMon JSON）」，而 `config-schema.md` §0 不变量 6 规定「零第三方依赖：不用 nlohmann 等库」 | **以 `config-schema.md` 为准**：配置 JSON 自研。主方案 §5 需改写；`wil` 是否保留为唯一第三方头文件依赖 → **待核实**（需用户拍板，因为它直接决定「零依赖」的口径） |
| 2 | 主方案 §6 给固定采样周期，`config-schema.md` §3.7 把 `sensorIntervalMs` 开放为 200–5000 ms | 固定周期 = **默认档**；低到 200 ms 时必须在设置界面显示运行期实测开销（本文 §6.3） |
| 3 | 主方案 §3.4 要求卡顿用 CapFrameX 无魔数定义 `100 × count(ft > factor × avg) / count`（基线为**平均**），`config-schema.md` §4 把 `fps.stutter` 写作「> 2 × **中位**帧时间 的帧占比」 | 口径必须在 `fps-metrics.md` 收敛（本表只记录）；两边都要能算出并标注当前口径 |
| 4 | 主方案同时出现「PresentMon 回调线程」（§6）与「轮询消费帧数据」（§3.4） | 见 §4：M2 按 SDK 实际形态定线程模型 |
| 5 | 主方案 §1 只给 `≤ 0.3% / ≤ 1.0%` 两个 CPU 指标，未定义 200 ms 间隔下的上界 | 见 §6.3，用「运行期实测值」而非固定常数对外呈现 |

## 13. 开放项（需用户拍板 / 需 M0 实测）

| # | 项 | 影响 |
|---|---|---|
| 1 | 是否保留 `wil` 作为唯一第三方依赖 | 决定主方案 §5 的依赖清单与「零依赖」宣传口径 |
| 2 | ~~产品名 / 仓库名~~ | ✅ **已决**：`winperfmon` → EXE `winperfmon.exe`、`%APPDATA%\winperfmon\config.json`、仓库 `winperfmon`（`config-schema.md` §9 开放问题 1 已结） |
| 3 | 采集线程是否绑核 | 影响多核调度下的占用曲线（§6.3） |
| 4 | v2 服务拆分的时间点 | 决定 M0–M2 是「先单进程跑通再拆」还是「一开始就双进程」；建议 M0 单进程、M2 之后拆（与 `roadmap.md` 对齐） |
