# FPS、帧时间与 1% Low 口径规范

> 本文由 winperfmon-dev-plan.md §3.4 拆出并细化；上游冲突时以本文为准，并回头修正主方案。
> 关联：`architecture.md`（FPS/统计线程与环形缓冲）、`config-schema.md` §3.8（`fps` 配置块）与 §4（指标 ID）、`gpu-fans.md`（GPU 备用遥测）。
> 术语：1% Low / 免驱模式 见 `README.md` §五。**帧字段名、CLI 参数、issue 号、路径必须与本文字面一致。**

## 0. 指标 ID 与口径总表

| 指标 ID | 默认名称 | 单位 | 精度 | 默认可见 | 口径（一句话） |
|---|---|---|---|---|---|
| `fps.instant` | FPS | FPS | 0 | ✅ | 最近统计窗（1 s）内的即时帧率 |
| `fps.avg` | 平均 FPS | FPS | 0 | ✅ | **调和平均：帧数 × 1000 / Σ帧时间** |
| `fps.low1` | 1% Low | FPS | 0 | ✅ | 最差 1% 帧时间均值取倒数（§4） |
| `fps.low01` | 0.1% Low | FPS | 0 | ❌ | 同上，百分位 0.001 |
| `fps.frametime` | 帧时间 | ms | 1 | ❌ | PresentMon `MsBetweenPresents` |
| `fps.frametime.graph` | 帧时间曲线 | — | — | ❌ | 环形缓冲 sparkline（30–200 px） |
| `fps.stutter` | 卡顿率 | % | 1 | ❌ | 见 §6（口径收敛中） |
| `fps.presentmode` | 呈现模式 | — | — | ❌ | PresentMon `PresentMode`（Overlay 降级自检） |
| `fps.cpu.busy` | CPU 忙 | ms | 1 | ❌ | PresentMon `MsCPUBusy` |
| `fps.gpu.busy` | GPU 忙 | ms | 1 | ❌ | PresentMon `MsGPUBusy` |

## 1. 采集路径（按推荐顺序）

> **决策 ④=B 更新：** 帧数据采集路径不变（PresentMon Service 首选）。变的只是**显示**：真独占全屏（FSE）下改用 RTSS OSD 共享内存后端作为主路径（见 `architecture.md` §3、`ui-settings.md` §6.1）。RTSS 只负责显示，不参与帧统计，1% Low / 0.1% Low 口径不受影响。

### 1.1 PresentMon Service + SDK（首选）

| 项 | 内容 |
|---|---|
| 版本前提 | **PresentMon 2.3.1+** 安装后部署 `PresentMonAPI2.dll` |
| 服务 | Windows 服务 **`PresentMonSharedService`** 以 **SYSTEM** 运行（ETW 由它跑） |
| SDK 路径 | **`C:\Program Files\Intel\PresentMon\SDK`**（`PresentMonAPI.h`、可选 `PresentMonAPI2Loader.lib/.dll`） |
| 我方调用 | **动态加载 `PresentMonAPI2.dll`** → 打开会话 → 注册帧查询 → **轮询消费帧数据**；所有 API 返回 **`PM_STATUS`** 枚举（详细诊断见 `PresentMonDiagnostics.h`） |
| 额外收益 | 服务同时提供它自己聚合的硬件遥测（GPU 温度/功耗/占用，走 NVAPI 等），可作 GPU 指标备用来源 |
| 优点 | **客户端零提权、不注入、不挂钩**，反作弊风险为零，维护成本最低 |

**必须注意（三条硬约束）**

1. **只能动态加载系统已安装的那份 DLL** —— 官方明确：客户端自带 `PresentMonAPI2.dll` 时**不保证与服务二进制兼容**。
2. **链路有固有秒级延迟**：ETW → 分析库 → 服务缓冲 → 客户端查询。Intel 自家 Capture Application 的 overlay 也是低频刷新，维护者亦确认数据源自 ETW 有延迟。→ **Overlay 只做 1 Hz 级聚合显示；帧级精确统计放到会话结束后计算；不要承诺逐帧实时曲线。**
3. **服务异常恢复后旧会话不重连**：安装器为服务配置 SCM recovery（**2 s → 15 s → 60 s → 5 min，第 5 次后保持停止；30 min 无失败后计数重置**），下一次调用返回 **`PM_STATUS_SESSION_NOT_OPEN`**，**必须由我方自动重开会话**。

### 1.2 子进程 `PresentMon.exe`（服务不可用时的降级路径）

```
PresentMon.exe --process_name xxx.exe --output_stdout --v2_metrics --no_console_stats
```

| 项 | 内容 |
|---|---|
| 输出 | **逐帧 CSV 到 stdout** —— 官方**没有 JSON 输出选项**（若需要结构化，只能自己把 CSV 解析成结构体） |
| 其他可用参数 | `--output_file`、`--multi_csv`、`--hotkey ALT+SHIFT+F11`、`--timed N`、`--session_name`、`--stop_existing_session`、`--restart_as_admin`、`--exclude_dropped` |
| 权限代价 | **需要管理员权限，或把用户加入 `Performance Log Users` 组**，否则报 **`failed to start trace session (access denied)`** |
| 定位 | 只作为"服务不可用"时的兜底；多一个进程 + 文本解析 |

### 1.3 自建 ETW 会话（零依赖，需管理员）

| 项 | 内容 |
|---|---|
| API | `StartTrace` / `EnableTraceEx2` / `OpenTrace` / `ProcessTrace` |
| 订阅 | `Microsoft-Windows-DxgKrnl` 的 Present 事件 |
| 成本 | 自研等于**重新实现 D3D9/DXGI/D3D12 的 present 语义**（PresentMode 判定、Dropped、失焦、Runtime 识别），代码量最大 |
| 定位 | **仅作长期选项**；Rust 侧可参考 `n4r1b/ferrisetw`（MIT） |

### 1.4 不采用：DLL 注入 / `Present()` 挂钩

- 反作弊封号风险、需要签名 DLL、版本适配成本高（RTSS 那套）。
- 代价须知：**真独占全屏下想显示 Overlay，注入几乎是唯一路径**（主方案 §7）。

## 2. 关键帧字段（PresentMon 2.x 口径）

| 字段 | 含义 | 我们怎么用 |
|---|---|---|
| `MsBetweenPresents` | 帧时间 | `fps.frametime`、所有派生统计的输入 |
| `MsBetweenDisplayChange` | 显示换面间隔 | 诊断掉帧/重复帧 |
| `MsUntilDisplayed` | 到显示的延迟 | 延迟类诊断 |
| `DisplayLatency` | 显示延迟 | 延迟类诊断 |
| `MsCPUBusy` / `MsCPUWait` | CPU 忙/等 | `fps.cpu.busy`（`MsCPUBusy`）与 CPU 瓶颈判定 |
| `MsGPUTime` / `MsGPUBusy` / `MsGPUWait` | GPU 时间/忙/等 | `fps.gpu.busy`（`MsGPUBusy`）与 GPU 瓶颈判定 |
| `VideoBusy` | 视频引擎忙 | 录屏/编码干扰诊断 |
| `MsAnimationError` | 动画误差 | 帧节奏诊断 |
| `ClickToPhotonLatency` / `MsPCLatency` | 点击到光子延迟 / PC 延迟 | 延迟类指标（v2） |
| `Dropped` | 是否丢弃帧 | 剔除异常帧的依据之一 |
| `PresentRuntime` | `DXGI` / `D3D9` | 判定渲染 API |
| `AllowsTearing` | 是否允许撕裂 | 撕裂模式诊断 |
| `PresentMode` | 呈现模式 | **Overlay 降级自检**（§3） |

> 控制台 CSV 另含 `MsUntilRenderComplete`（官方 `README-ConsoleApplication.md` 的列清单）；本表只列我们实际消费的字段。

## 3. `PresentMode` 取值与 Overlay 降级自检

| PresentMode 取值 | 含义 | 对我们的意义 |
|---|---|---|
| `Hardware: Legacy Flip` | **真独占全屏（FSE）**，应用接管屏幕 | **外部窗口无法覆盖**（Overlay 不可见，见主方案 §7） |
| `Hardware: Legacy Copy to front buffer` | 旧式全屏拷贝 | 同上，覆盖不可用 |
| `Hardware: Independent Flip` | **iFlip，延迟最低** | **我们最不想破坏的状态** |
| `Hardware Composed: Independent Flip` | 拿到 **MPO 硬件 overlay plane** | 覆盖有可能不降级 |
| `Composed: Flip` | 走 DWM 合成，延迟更高 | **置顶 Overlay 导致的降级目标态** |
| `Composed: Copy with GPU GDI` | 最差档之一 | 严重退化 |
| `Composed: Copy with CPU GDI` | 最差档 | 严重退化 |

**自检机制（M3 DoD 要求）**：记录 Overlay 打开前 / 打开后的 `PresentMode`，一旦检测到 **`Hardware: Independent Flip` → `Composed: Flip`**：
1. 明确提示「本 Overlay 正在增加延迟 / 影响 VRR」；
2. 提供**一键关闭**（另提供「游戏中自动隐藏」）；
3. 在状态栏常显 `PresentMode`（`fps.showPresentMode = true`）。

依据：Intel 自家 PresentMon Overlay 就是活案例 —— issue **#235 "Lose iFlip if the overlay is brought up"**、**#367 "Opening overlay changes PresentMode"**、**#689 "Overlay disables VRR"**；FSE 覆盖不可用见 issue **#212**（维护者确认）。

## 4. 派生指标口径（必须写进关于页）

| 指标 | 公式 | 反例/注意 |
|---|---|---|
| 平均 FPS | **帧数 × 1000 / Σ帧时间**（调和平均） | **不能**用帧时间算术平均再取倒数（两者在帧时间波动大时明显不同） |
| **1% Low** | 帧时间降序，取最差 1% 帧的**平均帧时间** `T1`，**1% Low = 1000 / T1**；`N = ceil(0.01 × 帧数)` | 与 CapFrameX 的口径**不等价**（见下） |
| 0.1% Low | 同上，百分位 0.001 | 样本少时抖动大，需同时显示样本数 |
| 双口径并存 | **会话累计**（整段游戏）+ **滚动窗**（最近 10 s，供 Overlay 显示） | Overlay 上要标明当前显示的是哪一种 |

**CapFrameX 口径差异（MIT，`source/CapFrameX.Statistics.NetStandard/FrametimeStatisticProvider.cs`）**

| 口径 | 实现 | 与我们的差别 |
|---|---|---|
| CapFrameX 1% Low | `1000 / seq.Where(e => e >= Quantile(0.99)).Average()` —— **分位阈值选样**："≥ P99 分位的所有帧取平均" | 与"最差 `ceil(N × 0.01)` 个"在**样本少时不等价** |
| Afterburner integral 口径 | 同文件的 `GetPercentageHighIntegralSequence`，注释自称"MSI Afterburner 等价"，为**时间加权** | 要与 Afterburner 对拍时切到它 |

**结论**：建议**两种都算**，并在 tooltip 标注当前口径（避免与别家对不上引发质疑）。

## 5. 异常帧与统计实现

| 项 | 规则 |
|---|---|
| 异常帧剔除 | `frameTime ≤ 0`、`> 1000 ms`、**加载/失焦期帧**标记无效；不参与统计并**记录剔除数量** |
| 离群剔除（可选，照 CapFrameX） | `TAU = 0.999`、`ceil(N × 0.001 − 1e-9)`，按值删除最慢的 0.1% 且**保持帧序** |
| 环形缓冲 | **16k~64k 帧时间（128~512 KB）**；配置默认 `keepBufferFrames = 65536`（≈ 1080p60 下 18 分钟，内存 512 KB），上限 262144（≈ 2 MB） |
| 统计频率 | **每秒排序一次（≤ 0.5 ms）**算均值/百分位 |
| 直方图 | **0.1 ms 桶、上限 200 ms**，供实时曲线 |
| 分配策略 | **全程零动态分配**（预分配 + 复用；参考 CapFrameX 的 `[ThreadStatic]` 缓冲复用思路） |
| 禁止照抄 | CapFrameX 的**分箱是 `O(bins × N)`，只能离线跑**；实时侧必须用**排序 + 部分和** |

## 6. 卡顿（stutter）口径收敛

| 来源 | 定义 | 状态 |
|---|---|---|
| 主方案 §3.4（有出处，可与 CapFrameX 对拍） | **`100 × count(ft > factor × avg) / count`**（无魔数定义，"factor" 由用户/预设给，默认 2），基线为**平均帧时间** | ✅ **作为 v1 默认口径** |
| 主方案 §3.4「时间版」 | 以**移动平均**为基线，窗口 **`round(sqrt(avgFrameTime) × 10)`**（16.7 ms 平均 → **40 帧**） | ✅ 作为可选口径（长时卡顿更能反映"体感"） |
| `config-schema.md` §4 | 「`fps.stutter` = 帧时间 > **2 × 中位帧时间** 的帧占比」 | ⚠️ 与上两行**不一致**（基线为中位数、且 `factor` 被硬编码为 2） → **需回头修正 `config-schema.md` §4 该行措辞**，改为引用本文口径 |
| 帧节奏直方图 | 按 **`|ft[i] − ft[i−1]|`** 分 **<2 / <4 / <8 / <12 / 其它** 五档 | ✅ 作为辅助展示（离线/会话后） |

**收敛决定**：v1 界面上 `fps.stutter` 的 tooltip 必须写明「相对基线与系数」，且**允许切换基线（平均 / 中位）**——两边都能算，避免用户拿我们的数字与 CapFrameX/Afterburner 对比时产生争议。

## 7. PresentMon 缺失 / 服务异常时的降级与提示

| 情形 | 探测顺序与行为 |
|---|---|
| 未安装 | 三级探测：**SDK DLL → 子进程 → 提示安装**（主方案 §9）；帧率页顶部信息条说明「未检测到 PresentMon 服务/SDK，帧率功能不可用」+ 手动安装指引 + 「本次忽略」；**不自动下载安装**（`ui-settings.md` §8） |
| SDK 存在但服务未运行 | 走 1.2 子进程路径，或提示用户启动服务；UI 显示当前实际路径（服务/CLI/关闭） |
| 服务恢复后会话失效 | `PM_STATUS_SESSION_NOT_OPEN` → 自动重开会话；期间帧率项短暂显示 `—` |
| SCM recovery 用尽（第 5 次后保持停止） | 不静默空值：明确提示「帧率采集服务未运行」，并给出重启入口 |
| `fps.mode` 配置 | `auto`（优先服务，失败退化 CLI）/ `service` / `cli` / `off`；切换模式需要**重开会话**（`ui-settings.md` §9） |

## 8. 配置映射（`config-schema.md` §3.8）

| 字段 | 范围 | 默认 | 说明 |
|---|---|---|---|
| `fps.mode` | `auto` / `service` / `cli` / `off` | `auto` | 见 §7 |
| `fps.targetProcess` | 0–260 字符 | `""` = 前台游戏 | PresentMon 侧用 `--process_name` 对应 |
| `fps.autoStartOnGame` | bool | `true` | 检测到前台全屏游戏自动开采集 |
| `fps.keepBufferFrames` | 4096–262144 | `65536` | 显示等价时长与内存占用 |
| `fps.lowPercentile` | 0.001–0.05 | `0.01` | 1% Low 的百分位（UI 写清"降序取最差 `N = ceil(p × 帧数)` 帧的帧时间均值取倒数"） |
| `fps.showPresentMode` | bool | `true` | 状态栏显示 PresentMode（§3 自检） |
| `update.overlayRefreshFps` | 1–10 | `2` | Overlay 刷新；非注入 Overlay 建议 ≤ 2 Hz（主方案 §7 对策 3） |

## 9. 借用项与许可

| 项目 | 许可 | 借用内容 | 边界 |
|---|---|---|---|
| PresentMon | MIT | SDK 接入方式 + 字段语义权威来源 + `PresentMode` 自检依据 | 客户端只动态加载系统已装 DLL |
| CapFrameX | MIT | 1%/0.1% Low、Afterburner integral、stutter、帧节奏直方图、离群剔除、`[ThreadStatic]` 复用思路 | 热路径分箱 `O(bins × N)` **不可照抄**；保留版权声明 |
| MangoHud | MIT | HUD 元素化布局（文字/折线/直方图）+ 表驱动配置键 | 实现是 Vulkan layer 注入，**不可移植** |
| ferrisetw | MIT | 日后自研 ETW 的会话/回调封装参考 | 仅参考 |
| Special K | **GPL-3.0** | **只借"MPO plane 检测"思路，代码不可并入** | 传染性，绝不进仓库 |

## 10. 验收与自测要点

| 项 | 标准 | 方法 |
|---|---|---|
| 1% Low 一致性（M2 DoD） | 与 CapFrameX **同局对比偏差 ≤ 0.5 FPS** | 同一段游戏录制，两边分别导出会话统计 |
| 采集开销（M2 DoD） | FPS 采集额外 CPU **≤ 1%** | 任务管理器 + `Get-Process` 对照采集开关前后 |
| 统计耗时 | 每秒排序 **≤ 0.5 ms**（65536 帧） | 运行期打点，超过则降级为增量部分和 |
| 延迟现实性 | Overlay 数值刷新 ≈ **1–2 Hz** | 目视 + 日志时间戳；**不得宣传逐帧实时曲线** |
| 降级路径 | 服务缺失 → CLI → 提示，三条路都走通并给出准确原因 | 卸载/停服务/改名三种环境回归 |
| 自检有效性 | 人为制造降级能被检测到 | 打开自绘 Overlay 后观察 `Hardware: Independent Flip` → `Composed: Flip` |

## 11. 陷阱清单

1. **别把平均 FPS 写成"帧时间的算术平均取倒数"** —— 必须用调和平均。
2. **别把 1% Low 写成"最差 ceil(N) 个之外"的实现**（顺序/边界差一个样本就会与别家对不上）。
3. **别自带 `PresentMonAPI2.dll`** —— 与服务二进制不保证兼容。
4. **别忽略 `PM_STATUS` 返回值** —— 会话失效必须重开，否则静默无数据。
5. **别指望 1 Hz 之外的数据** —— 链路固有秒级延迟，逐帧实时曲线做不到。
6. **别用 `--output_stdout` 找 JSON** —— 官方没有 JSON 选项，输出是逐帧 CSV。
7. **别把 CLI 路径当默认** —— 它需要管理员或 `Performance Log Users` 组。
8. **别在 Overlay 上只显示"好看的数字"** —— 必须能显示 `PresentMode` 与当前口径，否则无法自证没伤害游戏。
9. **别把加载期/失焦帧算进 1% Low** —— 剔除并记录数量。
