# M2：FPS 采集（PresentMon 逐帧 CSV）

本文只讲 M2 已经落地的那一半：**帧统计口径**与 **PresentMon 子进程 + CSV 解析**。
Overlay 呈现（M3）与打包（M4）不在这里。

## 1. 为什么是子进程 + CSV，而不是 SDK

`docs/winperfmon-dev-plan.md` §3.4 定了四条路径，M2 选第二条（命令行 CSV），理由：

| 方案 | 为什么不选 / 为什么选 |
| --- | --- |
| PresentMon Service + SDK（`PresentMonAPI2.dll`） | 只能动态加载**系统已装的那份** DLL；用户没装过就没有。留作 M4 的可选增强 |
| **子进程 `PresentMon.exe --output_stdout --v2_metrics`** | **采用**：官方只给 CSV（没有 JSON），进程边界即故障边界，它崩了拖不走我们 |
| 自建 ETW 消费者 | 工作量大得多，且要自己维护 PresentMon 那套事件解析 |
| DLL 注入 / 挂钩 `Present()` | 违反项目铁律（不注入、不挂钩） |

代价（必须写清楚）：`--output_stdout` 走 ETW 会话，需要管理员**或**
`Performance Log Users` 组。本机（HP ZHAN66 14 G8a）当前用户在
`Performance Log Users` 组里，所以免提权可用——但这不是所有机器的保证。

## 2. 统计口径（`src/fps/framestats.h`）

三条口径钉死在代码注释与单测里，因为它们错一点点用户看不出来：

1. **平均 FPS 是调和平均**：`帧数 × 1000 / Σ帧时间`。
   不是「先算帧时间算术平均再取倒数」，更不是「各帧瞬时 FPS 求算术平均」——
   后者会被单帧尖峰抬高（单测里 99×10 ms + 1×25 ms 的用例：调和 98.52，算术 99.4）。
2. **1% Low 用帧时间的降序尾部**：最差 `k = ceil(n × p)` 帧的帧时间均值 `T`，报 `1000 / T`。
   同时算 **CapFrameX 的分位阈值口径**（帧时间 ≥ 第 k 大的所有帧取均值），两个数都给出：
   无并列时两者相等，有并列时后者集合更大——**差异本身就是诊断信息，不私吞**。
   代码里 `ceil(n × p)` 带一个 `- 1e-9`：`0.01` 在二进制里不精确，`1000 × 0.01` 会算出
   `10.000000000000002`，不减这一下 1% Low 会凭空多算一帧。
3. **卡顿率不用魔数**：`stutter% = 100 × count(帧时间 > factor × 基线) / n`，
   基线取均值或中位（配置项），`factor` 默认 2.0。基线值本身写进 detail，用户能自己验算。

补充规则：

- 异常帧剔除：`帧时间 ≤ 0` 或 `> 1000 ms` 判无效并计入 `rejected`（不是「这一帧慢」，而是
  「这中间没在采」——暂停、切桌面、断电源）。PresentMon 在切换进程时确实会写 0。
- **Dropped 帧**（`Dropped=1`）计入 `dropped` 计数但不进 FPS 统计：它从没上屏。它占用的时间
  体现在下一帧更大的 `MsBetweenPresents` 里，所以不会凭空多出帧。
- `MsCPUBusy` / `MsGPUBusy` / `PresentMode` 是可选列：整列缺失 → 该指标报
  `不可用 + 具体原因`；单帧写 `NA` → 该帧不参与该列的平均（**不当作 0**）。
- 环形缓冲定长（`fps.keepBufferFrames`，默认 65536 帧 ≈ 393 KB），`Add` 不做任何分配。

## 3. 解析器（`src/fps/presentmon_csv.h`）

- 列**按名字**认（`MsBetweenPresents` / `MsCPUBusy` / `MsGPUBusy` / `Dropped` / `PresentMode`），
  不假设列顺序；没认出的列保持「不存在」，不会退化成 0。
- 容错：CRLF/LF、UTF-8 BOM、带引号字段、表头前的警告横幅、被任意切开的管道分片、
  末行没有换行符（进程被强杀时的最后一帧往往就在这里）。
- 坏行只计入 `malformed`，**不中断采集**：采集中途抛异常等于丢掉整局数据。
- 表头之前允许最多 200 行横幅；超过就报错退出——说明对方根本不是 PresentMon 的逐帧输出，
  此时「装作在采」比报错更坏（用户会以为是游戏掉到 0 FPS）。

## 4. 采集链路（`src/fps/fps_provider.{h,cpp}`）

```
winperfmon.exe ──CreateProcess(匿名管道)──▶ PresentMon.exe --process_name X --output_stdout
       │                                            │
       └── FrameRing ◀── PresentMonCsvParser ◀──────┘  （stdout+stderr 同管道）
```

- **PresentMon.exe 的定位顺序**：`<winperfmon.exe 同目录>`（便携版，推荐）→ `PATH` →
  `%LOCALAPPDATA%\PresentMon` → `%ProgramFiles%\PresentMon` → `%ProgramFiles(x86)%\PresentMon`。
  找不到不是错误，`--probe` 的 `[fps]` 段会把**找过的每一个路径**列出来。
- 目标进程：配置 `fps.targetProcess` 有值就用它（来源标 `配置`）；为空则**跟随前台窗口进程**
  （来源标 `前台`），并用一张「明摆着不是游戏」的黑名单过滤（explorer、终端、浏览器、
  Office、IM 等）——判定刻意保守：宁可多采一次无用的 FPS，也不要说「你没在玩」。
- 参数：`--process_name <目标> --output_stdout --no_console_stats --v2_metrics
  --terminate_on_proc_exit`。`--v2_metrics` 是 2.x 的写法，**1.x 会报未知参数**，
  所以第一次秒退且一行表头都没读到时，会自动去掉它重试一次（只重试一次）。
- 退出处理：PresentMon 退出 → 记录 `exitCode` 并把 `fps.*` 标 `Stale`/`Unavailable` 并说明
  「PresentMon 退出」；打不开管道/没有表头则明确写「没有输出任何可解析的帧数据就退出了」。
- 冻结检测：超过 3 s 没有新帧 → 指标状态从 `Ok` 变 `Stale`，detail 写
  「已 N s 没有新帧（游戏暂停/最小化/切了前台？）——下面是最后一帧之前的统计」。
- **`--probe` 不启动子进程**（诊断命令不该有副作用）：`fps.note` 会写
  「诊断模式（--probe）：只报状态，不启动 PresentMon 子进程」。

## 5. 验证到什么程度（诚实边界）

已验证（本机 Win11 26300 / Ryzen 7 H 255 / 非管理员）：

- **单测 71 passed / 0 failed**，其中 15 个帧统计用例（调和平均、1%/0.1% Low 两条口径、
  并列帧时间的口径分歧、卡顿率与两种基线、异常帧剔除、环形绕圈、Dropped 与无效帧分开计数、
  两条计算路径必须一致）与 13 个 CSV 用例（列乱序、引号、NA、坏行、逐字节分片喂入、
  CRLF+BOM、末行无换行、表头缺失致命化、端到端喂进统计）。
- **管道链路端到端**：用 scratch 里的夹具程序冒充 `PresentMon.exe`（只存在于
  `D:\GitHub\winperfmon-dsh-scratch\fake_presentmon.cpp`，**不进仓库**）验证
  `CreateProcess → 管道 → 解析 → 环形缓冲 → 指标`：`fps.csvFrames` 与 `fps.ringFrames`
  一致增长，`--watch` 打印出 `FPS 60（1% Low 35~44）`，与夹具「每 200 帧一个 40 ms 尖峰」
  的预期量级一致。
- 夹具程序的局限：它**不能**验证真实 PresentMon 的列名、参数版本差异与真实 ETW 行为。

**尚未验证**（需要用户机器上装真 PresentMon + 真游戏）：

- 与 CapFrameX 同局对比 1% Low 偏差 ≤0.5 FPS（`docs/winperfmon-dev-plan.md` §8 的 M2 验收项）。
- FPS 采集额外 CPU ≤1%（要在真实游戏负载下测，本机目前没有任何游戏）。
- `--v2_metrics` 自动重试这条路径（需要一台装着 PresentMon 1.x 的机器才会被触发）。

## 6. 指标口径速查

| 指标 | 值 | detail 里写了什么 |
| --- | --- | --- |
| `fps.instant` | 最后一帧的 `1000 / 帧时间` | 样本帧数、目标进程与来源 |
| `fps.avg` | 调和平均 | 「调和平均：帧数 × 1000 / Σ帧时间（N 帧）」 |
| `fps.low1` | 最差 `ceil(n×0.01)` 帧均值取倒数 | 顺带给出 CapFrameX 分位口径值 |
| `fps.low01` | 同上，百分位 0.001 | 同上 |
| `fps.frametime` | 平均帧时间（ms） | 中位数与最差值 |
| `fps.stutter` | 超过 `factor × 基线` 的帧占比（%） | 基线 ms 值、`factor`、`超阈值帧数/总帧数` |
| `fps.presentmode` | 0（值无意义） | 出现次数最多的模式 + 占比 |
| `fps.cpu.busy` / `fps.gpu.busy` | `MsCPUBusy` / `MsGPUBusy` 均值 | 列名；缺列时如实报不可用 |
| `fps.frametime.graph` | 不可用 | 「由 M3 的 Overlay 直接从环形缓冲绘制，不走指标槽」 |
