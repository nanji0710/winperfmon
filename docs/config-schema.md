# 配置项数据结构与读写接口规范

> 文档状态：**设计稿 v1（待确认）** · 适用版本：v0.1 起 · 关联：`ui-settings.md`（交互行为）、`amd-cpu.md`、`release-github.md`
> 本文是用户自定义设置（排版方式、指标可见性与排序、置顶、字号与颜色）的**数据契约**，编码前需冻结。

## 0. 设计目标与不变量

| # | 不变量 | 说明 |
|---|---|---|
| 1 | **单一事实来源** | 指标「可见性」与「顺序」合并到同一个有序数组 `metrics`，避免两份清单互相矛盾（例如顺序表里有、可见性表里没有）；同理 GPU 选择只有 `gpu.selectedIndex` 一处，不设「默认 GPU」旁路 |
| 2 | **前向兼容** | 新版新增指标自动追加到末尾并按其默认可见性出现；旧版读新文件时忽略未知字段、丢弃未知指标并记诊断，不崩溃 |
| 3 | **写入不破坏** | 原子替换 + 保留上一版 `.bak`；**解析失败绝不覆盖用户文件** |
| 4 | **校验即修复** | 非法值不抛异常、不阻断启动：夹紧到边界或回退默认，并产出诊断项（界面可提示） |
| 5 | **热路径零锁** | UI 线程改配置，采集线程只读 `shared_ptr` 快照（原子发布），采集路径不持有互斥量 |
| 6 | **零第三方依赖** | 配置用自研最小 JSON 子集读写（约 300 行 + 单测）；不引入 nlohmann 等库（体积与供应链考虑） |

## 1. 文件位置、格式与写入语义

**路径解析优先级（自上而下，先命中者生效）：**

| 优先级 | 条件 | 路径 |
|---|---|---|
| 1 | 命令行 `--config <path>` | 指定路径 |
| 2 | 环境变量 `winperfmon_CONFIG`（供测试/CI 使用） | 指定路径 |
| 3 | 便携模式：`<exe目录>\portable.flag` 存在 | `<exe目录>\config.json` |
| 4 | 默认（每用户） | `%APPDATA%\winperfmon\config.json`（产品名已定：winperfmon） |
| 5 | 上述目录不可写 | 只读模式：内存配置 + 界面提示「设置无法保存」，退出前提示「另存为」 |

**格式：** UTF-8 **无 BOM**、缩进 2 空格、字段顺序固定（便于 diff 与人工编辑）；文件大小上限 256 KB；JSON 嵌套深度上限 16；单个字符串上限 4 KB。

**原子写入序列：** 写同目录 `config.json.tmp` → `FlushFileBuffers` → `MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`；成功后把旧文件轮转为 `config.json.bak`（只保留 1 代）。

**失败语义：**

| 情形 | 行为 |
|---|---|
| 文件不存在 | 使用内置默认，`fileExisted = false`，退出时（或首次改动时）写出一份完整默认文件 |
| 解析失败 / 超限 | **保留当前内存配置运行**，记 Error 诊断 + 界面黄条；不写盘；仅当用户点「用默认值重写」才覆盖 |
| 部分字段非法 | 逐字段修复 + Warning 诊断，可正常写盘 |
| `schemaVersion` 高于本程序 | 以只读方式加载（禁止自动写回），Error 诊断并提示升级 |

## 2. 顶层结构

```json
{
  "schemaVersion": 1,
  "meta":    { },
  "layout":  { },
  "metrics": [ ],
  "colors":  { },
  "text":    { },
  "window":  { },
  "update":  { },
  "fps":     { },
  "gpu":     { }
}
```

## 3. 字段表

类型约定：`bool` / `int`（32 位有符号）/ `float` / `string` / `enum` / `ColorARGB`（`"#AARRGGBB"`，alpha 在前）。所有数值字段均在加载时按「取值范围」夹紧。

### 3.1 `meta`（元信息，程序写入）

| 字段 | 类型 | 取值 | 默认 | 说明 |
|---|---|---|---|---|
| `writtenBy` | string | ≤64 字符 | `"winperfmon 0.1.0"` | 写入者版本，用于排查 |
| `lastSavedUtc` | string | ISO-8601 UTC | `""` | 上次保存时间，仅供诊断 |

### 3.2 `layout`（排版方式）

| 字段 | 类型 | 取值范围 | 默认 | 说明 |
|---|---|---|---|---|
| `orientation` | enum | `horizontal` \| `vertical` | `horizontal` | **横向排布** = 指标从左到右一行；**纵向排布** = 指标从上到下一列 |
| `groupByCategory` | bool | — | `true` | 按分组（CPU/GPU/风扇/帧率/系统）分段，段内保持 `metrics` 顺序；段间插入 `groupGapDp` |
| `showLabels` | bool | — | `true` | 是否显示指标名称（如 `CPU 温度`）；关闭则只显示数值+单位 |
| `showUnits` | bool | — | `true` | 是否显示单位（`°C`/`MHz`/`W`/`RPM`/`FPS`） |
| `itemGapDp` | int | 0–64 | `8` | 同段内相邻指标的间距（Dp） |
| `groupGapDp` | int | 0–64 | `14` | 分组之间的额外间距（Dp） |
| `align` | enum | `start` \| `center` \| `end` | `start` | 仅横向：整行相对窗口的对齐方式 |
| `wrap` | bool | — | `false` | 仅横向：超出 `maxWidthDp` 时换行 |
| `maxWidthDp` | int | 120–4096 | `960` | `wrap = true` 时的换行宽度 |
| `hideMissing` | bool | — | `true` | 指标无数据（如免驱模式下的 CPU 温度）时整项隐藏；`false` 则显示 `N/A` |

### 3.3 `metrics`（可见性与排序 —— 核心）

```json
"metrics": [
  { "id": "cpu.temp.package", "visible": true },
  { "id": "cpu.power.package", "visible": true, "label": "CPU", "precision": 1 },
  { "id": "gpu.temp.core", "visible": true, "color": "#FF4FC3F7", "sizeScale": 1.1 },
  { "id": "cpu.temp.cores", "visible": false }
]
```

| 字段 | 类型 | 取值范围 | 默认 | 说明 |
|---|---|---|---|---|
| `id` | string | 必须存在于指标目录（§4） | 必填 | 指标标识；**发布后不得改名**（改名走别名表 §8） |
| `visible` | bool | — | 目录的 `defaultVisible` | 是否展示 |
| `label` | string | 0–24 字符 | `""` = 目录默认名 | 自定义显示名（用户可把「CPU 封装温度」改成「CPU」） |
| `color` | string | `""` 或 `"#AARRGGBB"` | `""` = 随主题 | 该项数值的颜色覆盖 |
| `sizeScale` | float | 0.5–2.0 | `1.0` | 该项字号倍率，最终字号 = `text.baseSizeDp × window.scale × sizeScale` |
| `precision` | int | −1–3 | `-1` = 目录默认 | 小数位数 |

**顺序语义：** 数组下标即展示顺序（横向=从左到右，纵向=从上到下）。拖拽排序在 UI 上直接改数组顺序，不额外维护序号字段。

**一致性规则（加载时按下表修复）：**

| 情形 | 处理 | 级别 |
|---|---|---|
| 目录中存在但数组里缺失的指标 | 按目录顺序**追加到数组末尾**，`visible` 取目录默认 | Info |
| 数组里出现未知 `id` | 丢弃该条目 | Warning |
| 同一 `id` 出现多次 | 保留第一处，丢弃其余 | Warning |
| `metrics` 缺失 / 为空 | 用目录默认全量生成 | Info |
| 全部 `visible = false` | 把目录中第一个默认可见项强制置 `true` | Warning |
| 数组中 `id` 大小写不符 | 按目录做一次大小写不敏感匹配后改写为规范形式 | Info |

**渲染约束：** `hideMissing = false` 时，缺数据显示 `N/A` 且颜色自动降为 `colors.unit`（避免误读为异常值）；横向 `wrap = false` 时超出窗口宽度直接裁剪（不缩字号），并在设置界面提示。

### 3.4 `colors`（颜色）

| 字段 | 类型 | 默认（深色主题） | 说明 |
|---|---|---|---|
| `themeId` | enum | `dark` | `dark` \| `light` \| `custom`；选 `dark`/`light` 时忽略下面的自定义值 |
| `background` | ColorARGB | `#CC101418` | 面板/Overlay 背景 |
| `panelBorder` | ColorARGB | `#40FFFFFF` | 边框 |
| `label` | ColorARGB | `#B3FFFFFF` | 指标名称文字 |
| `value` | ColorARGB | `#FFFFFFFF` | 数值文字（默认继承色） |
| `unit` | ColorARGB | `#99FFFFFF` | 单位文字 |
| `ok` | ColorARGB | `#FF4CAF50` | 正常状态色 |
| `warn` | ColorARGB | `#FFFFB300` | 警告阈值色 |
| `critical` | ColorARGB | `#FFE53935` | 严重阈值色 |
| `accent` | ColorARGB | `#FF4FC3F7` | 强调色（帧率、曲线等） |

**状态色规则（沿用主方案）：** 状态色必须**同时打印数值**，不允许只用颜色表达状态；颜色随数值阈值的映射由指标目录提供默认阈值（如 CPU 温度 80/95 °C → warn/critical），阈值可后续开放为用户配置（v2）。
**可读性校验：** 当 `value` 与 `background` 的对比度 < 3:1 时，在设置界面给出可读性警告（不阻断）。

### 3.5 `text`（文字大小）

| 字段 | 类型 | 取值范围 | 默认 | 说明 |
|---|---|---|---|---|
| `baseSizeDp` | int | 8–48 | `14` | 基准字号（Dp）；实际像素 = `baseSizeDp × window.scale × DPI缩放` |
| `boldValue` | bool | — | `true` | 数值是否加粗（数值用中等字重更易读） |
| `fontFamily` | string | 0–64 字符 | `""` = 系统 UI 字体 | 自定义字体族（v2 开放选择列表；首版仅接受空值或已安装字体名） |
| `minSizeDp` | int | 6–24 | `10` | 缩放到最小显示器/最小 scale 时的字号下限，防止不可读 |

### 3.6 `window`（置顶与窗口）

**决策 ④=B 新增子段 `overlay`（JSON 路径 `window.overlay`）：**

| 字段 | 类型 | 取值范围 | 默认值 | 说明 |
|---|---|---|---|---|
| `backend` | enum | `auto` \| `native` \| `rtss` | `auto` | `auto` = 真 FSE 且 RTSS 共享内存可用时用 RTSS，否则自绘；`native`/`rtss` = 强制 |
| `rtssUpdateMs` | int | 100–2000 | `500` | 写 RTSS OSD 的最小间隔（过密无益，RTSS 自身有刷新上限） |

- **RTSS 模式下的字段语义**：`position` / `opacity` / `clickThrough` / `scale` **不生效**（由 RTSS 的 OSD 设置控制）→ 设置界面置灰并注明"由 RTSS 控制"；`colors.*` 与 `text.baseSizeDp` 只能按 RTSS 文本标签近似映射（支持矩阵**待核实**），映射不到的项在设置界面提示"RTSS 模式不支持，已忽略"。
- `layout.orientation` 在 RTSS 模式下映射为分隔符：横向 → 同行拼接，纵向 → 换行。

> JSON 中位置是嵌套对象 `position`（便于人工编辑成一组）；内存结构 `WindowConfig` 为读取方便拍平为 `posMode / corner / x / y`，由序列化层负责映射。

| 字段 | 类型 | 取值范围 | 默认 | 说明 |
|---|---|---|---|---|
| `topmostPolicy` | enum | `always` \| `gameOnly` \| `never` | `always` | **置顶设置**：始终置顶 / 仅检测到游戏（FPS 采集中）时置顶 / 从不置顶 |
| `clickThrough` | bool | — | `true` | Overlay 鼠标穿透（`WS_EX_TRANSPARENT`），不抢焦点 |
| `opacity` | float | 0.2–1.0 | `0.90` | 整体不透明度（叠乘到背景 alpha） |
| `scale` | float | 0.5–3.0 | `1.0` | 全局缩放（同时影响字号与间距） |
| `position.mode` | enum | `preset` \| `free` | `preset` | 位置模式 |
| `position.preset` | enum | `topLeft` \| `topRight` \| `bottomLeft` \| `bottomRight` | `topLeft` | 预设角落 |
| `position.x` / `position.y` | int | −32768–32767 | `0` / `0` | `free` 模式下的左上角坐标（虚拟屏坐标） |
| `marginDp` | int | 0–200 | `12` | 预设角落时相对屏幕边缘的留白 |
| `monitorId` | string | 0–64 字符 | `""` = 主显示器 | 记忆所在显示器（设备名），多屏场景 |
| `hideInFullscreen` | bool | — | `false` | 检测到独占全屏时自动隐藏（避免无意义叠层） |
| `showTrayIcon` | bool | — | `true` | 托盘图标 |
| `startWithWindows` | bool | — | `false` | 开机自启（写入 HKCU Run，需用户显式开启） |

### 3.6.1 `gpu`（设备选择 —— 决策：§6.1-2 已闭环）

| 字段 | 类型 | 取值范围 | 默认 | 说明 |
|---|---|---|---|---|
| `selectedIndex` | int | `-1` \| `0`–15 | `-1` | 监控哪一个 GPU。`-1` = **自动**：优先取 PresentMon 报告的正在渲染的 GPU，其次取使用率最高者；`0`–`15` = 强制指定枚举下标 |

**设备枚举顺序（下标语义，UI 必须显示同一顺序）：** 物理适配器优先，**虚拟/间接显示适配器恒排在物理适配器之后**；虚拟适配器可被手动选中（调试用），但在 UI 中标注 `虚拟显示适配器（通常无有效遥测）`。判定依据：`Win32_VideoController.PNPDeviceID` 以 `PCI\` 开头的为物理，`ROOT\` / `INDIRECTDSP\` 等为虚拟。

> 本机实测旁证（2026-10，HP ZHAN66 14 G8a）：`Win32_VideoController` 返回 4 个适配器 = `AMD Radeon(TM) Graphics`（`PCI\VEN_1002&DEV_1900`，Radeon 780M）+ 3 个虚拟适配器（GameViewer / OrayIddDriver / Honor）。**若按枚举顺序取「第一个」，拿到的是虚拟适配器** —— 这是本字段存在的直接原因。

**修复规则：** 下标越界（≥ 本机适配器数）→ 回退 `-1` 并记 Warning 诊断；被选中的 GPU 缺少某项遥测时按 §4 说明与 `layout.hideMissing` 处理，**绝不回退到另一个 GPU 的读数值**。


| 字段 | 类型 | 取值范围 | 默认 | 说明 |
|---|---|---|---|---|
| `sensorIntervalMs` | int | 200–5000 | `500` | 硬件传感器采样周期 |
| `overlayRefreshFps` | int | 1–10 | `2` | Overlay 刷新频率（主方案：非注入 Overlay 只用 1–2 Hz，避免拖累游戏） |
| `panelRefreshFps` | int | 1–30 | `4` | 主面板刷新频率 |
| `pauseWhenHidden` | bool | — | `true` | 界面隐藏时暂停传感器采样（仅保留 FPS 会话统计） |
| `lowPowerOnBattery` | bool | — | `false` | 电池供电时自动降频采样 |

### 3.8 `fps`

| 字段 | 类型 | 取值范围 | 默认 | 说明 |
|---|---|---|---|---|
| `mode` | enum | `auto` \| `service` \| `cli` \| `off` | `auto` | `auto`：优先 PresentMon SDK（服务），失败退化到命令行采集 |
| `targetProcess` | string | 0–260 字符 | `""` = 前台游戏 | 采集目标进程名（如 `cs2.exe`） |
| `autoStartOnGame` | bool | — | `true` | 检测到前台全屏游戏时自动开采集 |
| `keepBufferFrames` | int | 4096–262144 | `65536` | 帧时间环形缓冲容量（65536 帧 ≈ 1080p60 下 18 分钟；内存 512 KB） |
| `lowPercentile` | float | 0.001–0.05 | `0.01` | 1% Low 口径的百分位（0.01 = 最差 1%） |
| `stutterFactor` | float | 1.5–10.0 | `2.0` | 卡顿判定系数：`帧时间 > stutterFactor × 基线帧时间` 记为卡顿帧（口径定义见 `winperfmon-dev-plan.md` §3.4） |
| `stutterBaseline` | enum | `mean` \| `median` | `mean` | 卡顿判定基线：`mean` = 平均帧时间（v1 默认，可与 CapFrameX / Afterburner 对拍）、`median` = 中位帧时间；切换只影响 `fps.stutter` |
| `showPresentMode` | bool | — | `true` | 状态栏显示 PresentMode，用于自查 Overlay 是否把游戏从 Independent Flip 降级为 Composed: Flip |

## 4. 指标目录（编译期内置，不是用户配置）

命名规范：`<域>.<对象>.<量>`，全小写点分；风扇槽位 `fan.<槽位>.rpm`（`cpu` / `chassis1..6` / `pump`）；GPU 风扇归入 `gpu.fan.*`。

| # | 指标 ID | 分组 | 默认名称 | 单位 | 精度 | 默认可见 | 数据来源（首选 → 降级） |
|---|---|---|---|---|---|---|---|
| 1 | `cpu.usage` | cpu | CPU | % | 0 | ✅ | PDH / `NtQuerySystemInformation` |
| 2 | `cpu.freq.avg` | cpu | CPU 频率 | MHz | 0 | ✅ | APERF/MPERF 差分（Intel/AMD）→ PDH `% Processor Performance` × 基频 |
| 3 | `cpu.freq.percore` | cpu | 每核频率 | MHz | 0 | ❌ | 绑核读 MSR |
| 4 | `cpu.temp.package` | cpu | CPU 温度 | °C | 0 | ✅ | Intel `0x19C`/`0x1B1`；AMD SMN `0x00059800`（Tctl/Tdie） |
| 5 | `cpu.temp.cores` | cpu | 每核温度 | °C | 0 | ❌ | Intel 每核 DTS；AMD PM 表 |
| 6 | `cpu.temp.headroom` | cpu | 距温度上限 | °C | 0 | ❌ | Intel `TjMax − DTS`；AMD 家族上限表 − Tctl |
| 7 | `cpu.power.package` | cpu | CPU 功耗 | W | 1 | ✅ | RAPL 能量差分（Intel `0x611` / AMD `0xC001029B`）；AMD 可并列 PM 表 PPT |
| 8 | `cpu.power.cores` | cpu | 核心功耗 | W | 1 | ❌ | Intel RAPL PP0（`0x639`）；AMD 无核级 RAPL（可用 PM 表核心功耗，标注为参考值） |
| 9 | `cpu.power.soc` | cpu | SoC / Uncore 功耗 | W | 1 | ❌ | Intel RAPL PP1（`0x641`）；AMD PM 表 SoC 域 |
| 10 | `cpu.power.dram` | cpu | 内存功耗 | W | 1 | ❌ | Intel RAPL DRAM（`0x619`） |
| 11 | `cpu.voltage` | cpu | CPU 电压 | V | 3 | ❌ | Intel `0x198`；AMD SVI2（PM 表） |
| 12 | `cpu.throttle.reason` | cpu | 降频原因 | — | — | ❌ | Intel 热/功耗/电流限制位；AMD PM 表限制位 |
| 13 | `mem.usage` | sys | 内存 | % | 0 | ✅ | `GlobalMemoryStatusEx` |
| 14 | `mem.used` | sys | 已用内存 | GB | 1 | ❌ | 同上 |
| 15 | `gpu.usage` | gpu | GPU | % | 0 | ✅ | NVML / ADLX / IGCL |
| 16 | `gpu.freq.core` | gpu | GPU 频率 | MHz | 0 | ✅ | 同上 |
| 17 | `gpu.freq.mem` | gpu | 显存频率 | MHz | 0 | ✅ | 同上 |
| 18 | `gpu.temp.core` | gpu | GPU 温度 | °C | 0 | ✅ | 同上 |
| 19 | `gpu.temp.hotspot` | gpu | 热点温度 | °C | 0 | ❌ | NVML / ADLX |
| 20 | `gpu.temp.mem` | gpu | 显存温度 | °C | 0 | ❌ | NVML `NVML_FI_DEV_MEMORY_TEMP`；ADLX |
| 21 | `gpu.power` | gpu | GPU 功耗 | W | 1 | ✅ | NVML `GetPowerUsage`(mW)；ADLX；IGCL |
| 22 | `gpu.fan.percent` | gpu | 显卡风扇 | % | 0 | ❌ | NVML（多为百分比）；ADLX/IGCL |
| 23 | `gpu.fan.rpm` | gpu | 显卡风扇 | RPM | 0 | ❌ | ADLX / IGCL（NVIDIA 通常不提供 RPM） |
| 24 | `gpu.vram.usage` | gpu | 显存占用 | % | 0 | ❌ | NVML / ADLX |
| 25 | `gpu.vram.used` | gpu | 显存已用 | GB | 1 | ❌ | 同上 |
| 26 | `gpu.throttle.reason` | gpu | 显卡降频原因 | — | — | ❌ | NVML `GetCurrentClocksThrottleReasons`；ADLX |
| 27 | `gpu.enc.usage` | gpu | 编码器 | % | 0 | ❌ | NVML / ADLX |
| 28 | `gpu.dec.usage` | gpu | 解码器 | % | 0 | ❌ | NVML / ADLX |
| 29 | `fan.cpu.rpm` | fan | CPU 风扇 | RPM | 0 | ❌ | 台式机 Super I/O（Nuvoton/ITE）→ 笔记本 EC / 厂商 WMI（见主方案 §3.3.1） |
| 30 | `fan.chassis1..6.rpm` | fan | 机箱风扇 N | RPM | 0 | ❌ | 同上（笔记本通常只有 1 个风扇，映射到 `fan.cpu.rpm`） |
| 31 | `fan.pump.rpm` | fan | 水泵 | RPM | 0 | ❌ | 同上 |
| 32 | `mb.temp.vrm` | fan | 供电温度 | °C | 0 | ❌ | Super I/O |
| 33 | `mb.temp.board` | fan | 主板温度 | °C | 0 | ❌ | Super I/O |
| 34 | `fps.instant` | fps | FPS | FPS | 0 | ✅ | PresentMon（服务 SDK → 命令行） |
| 35 | `fps.avg` | fps | 平均 FPS | FPS | 0 | ✅ | 调和平均：`帧数 × 1000 / Σ帧时间` |
| 36 | `fps.low1` | fps | 1% Low | FPS | 0 | ✅ | 最差 1% 帧时间均值取倒数（口径见 `winperfmon-dev-plan.md` §3.4） |
| 37 | `fps.low01` | fps | 0.1% Low | FPS | 0 | ❌ | 同上，百分位 0.001 |
| 38 | `fps.frametime` | fps | 帧时间 | ms | 1 | ❌ | PresentMon `MsBetweenPresents` |
| 39 | `fps.frametime.graph` | fps | 帧时间曲线 | — | — | ❌ | 环形缓冲 sparkline（30–200 px） |
| 40 | `fps.stutter` | fps | 卡顿率 | % | 1 | ❌ | `帧时间 > stutterFactor × 基线帧时间`（默认 `2.0 × 平均帧时间`，基线可切中位）的帧占比，口径见 `winperfmon-dev-plan.md` §3.4 |
| 41 | `fps.presentmode` | fps | 呈现模式 | — | — | ❌ | PresentMon `PresentMode`（检测 Overlay 是否导致降级） |
| 42 | `fps.cpu.busy` | fps | CPU 忙 | ms | 1 | ❌ | PresentMon `MsCPUBusy` |
| 43 | `fps.gpu.busy` | fps | GPU 忙 | ms | 1 | ❌ | PresentMon `MsGPUBusy` |
| 44 | `sys.time` | sys | 时间 | — | — | ❌ | 本地时钟 |
| 45 | `sys.session` | sys | 采集时长 | s | 0 | ❌ | 会话计时器 |

> 说明：`④ GPU 指标`由 NVML/ADLX/IGCL 能力探测决定——硬件不暴露的项按 `hideMissing` 规则隐藏，**绝不填假值**（沿用主方案「缺失读数规则」）。**所有 GPU 指标只对 `gpu.selectedIndex` 选中的那一个适配器求值**，切换设备等于换数据源（`Revision` 递增、缓冲清零）。
> `④ 笔记本补充路径`：`cpu.temp.package` 在无 Ring0 的笔记本上可降级到 ACPI 热区域（`CPUZ` 区域）；`fan.*.rpm` 在笔记本上走 EC / 厂商 WMI（主方案 §3.3.1），**三条路径都读不到时显示 N/A 并说明原因**。

## 5. 校验与修复规则（ValidationPolicy）

| 情形 | 处理 | 诊断级别 |
|---|---|---|
| 数值越界 | 夹紧到边界 | Warning |
| 枚举取值非法 / 类型不符 | 回退默认 | Warning |
| 颜色格式非法 | 回退 `""`（随主题） | Warning |
| `metrics` 中指标重复 / 未知 | 去重 / 丢弃（§3.3） | Warning |
| `schemaVersion` 低于当前 | 依次执行迁移链 | Info |
| `schemaVersion` 高于当前 | 只读加载，禁止写回 | Error |
| 未知顶层字段 / 未知区块 | 忽略；写回时可选保留（`PreserveUnknownKeys`） | Info |
| 文件超 256 KB、深度 > 16、字符串 > 4 KB | 拒绝解析，保留内存配置 | Error |

策略开关（`ValidationPolicy`）：`clampNumbers`（默认 true）、`dropUnknownMetrics`（true）、`appendMissingMetrics`（true）、`preserveUnknownKeys`（false）。

## 6. 读写接口（C++20）

```cpp
namespace wpm::config {

// ── 枚举 ────────────────────────────────────────────────────────────────
enum class Orientation  { Horizontal, Vertical };
enum class Align        { Start, Center, End };
enum class ThemeId      { Dark, Light, Custom };
enum class TopmostPolicy{ Always, GameOnly, Never };
enum class PositionMode { Preset, Free };
enum class Corner       { TopLeft, TopRight, BottomLeft, BottomRight };
enum class FpsMode      { Auto, Service, Cli, Off };
enum class StutterBaseline { Mean, Median };   // 卡顿判定基线：平均帧时间（默认）/ 中位帧时间

// ── 值类型 ──────────────────────────────────────────────────────────────
struct ColorARGB { uint32_t v = 0; };            // 0xAARRGGBB；v == 0 表示"随主题"
struct MetricEntry {
    std::string id;            // 必须存在于指标目录
    bool        visible = true;
    std::string label;         // 空 = 目录默认名
    ColorARGB   color{};       // 0 = 随主题
    float       sizeScale = 1.0f;   // [0.5, 2.0]
    int         precision = -1;     // -1 = 目录默认
};
struct LayoutConfig { Orientation orientation = Orientation::Horizontal; bool groupByCategory = true;
                      bool showLabels = true; bool showUnits = true; int itemGapDp = 8; int groupGapDp = 14;
                      Align align = Align::Start; bool wrap = false; int maxWidthDp = 960;
                      bool hideMissing = true; };
struct ColorsConfig { ThemeId theme = ThemeId::Dark; ColorARGB background{0xCC101418}, panelBorder{0x40FFFFFF},
                      label{0xB3FFFFFF}, value{0xFFFFFFFF}, unit{0x99FFFFFF},
                      ok{0xFF4CAF50}, warn{0xFFFFB300}, critical{0xFFE53935}, accent{0xFF4FC3F7}; };
struct TextConfig   { int baseSizeDp = 14; bool boldValue = true; std::string fontFamily; int minSizeDp = 10; };
struct WindowConfig { TopmostPolicy topmost = TopmostPolicy::Always; bool clickThrough = true;
                      float opacity = 0.90f; float scale = 1.0f;
                      PositionMode posMode = PositionMode::Preset; Corner corner = Corner::TopLeft;
                      int x = 0, y = 0; int marginDp = 12; std::string monitorId;   // 空 = 主显示器
                      bool hideInFullscreen = false; bool showTrayIcon = true; bool startWithWindows = false; };
struct GpuConfig    { int selectedIndex = -1; };   // -1 = 自动（渲染中的 GPU），0..15 = 枚举下标
struct UpdateConfig { int sensorIntervalMs = 500; int overlayRefreshFps = 2; int panelRefreshFps = 4;
                      bool pauseWhenHidden = true; bool lowPowerOnBattery = false; };
struct FpsConfig    { FpsMode mode = FpsMode::Auto; std::string targetProcess; bool autoStartOnGame = true;
                      int keepBufferFrames = 65536; float lowPercentile = 0.01f;
                      float stutterFactor = 2.0f; StutterBaseline stutterBaseline = StutterBaseline::Mean;
                      bool showPresentMode = true; };

struct Config {
    int                       schemaVersion = 1;
    LayoutConfig              layout;
    std::vector<MetricEntry>  metrics;     // 顺序 = 展示顺序（单一事实来源）
    ColorsConfig              colors;
    TextConfig                text;
    WindowConfig              window;
    UpdateConfig              update;
    FpsConfig                 fps;
    GpuConfig                 gpu;
};

// ── 指标目录（编译期内置，非用户配置）──────────────────────────────────
struct MetricInfo {
    std::string_view id;
    std::string_view group;          // "cpu" | "gpu" | "fan" | "fps" | "sys"
    std::string_view defaultLabel;
    std::string_view unit;           // 可为空（文本类指标）
    bool             defaultVisible;
    int              defaultPrecision;   // -1 = 不适用
    std::string_view source;         // 数据来源说明（用于"为什么读不到"提示）
};
std::span<const MetricInfo> MetricCatalog();
const MetricInfo*           FindMetric(std::string_view id);   // 大小写不敏感

// ── 默认值 / 诊断 ───────────────────────────────────────────────────────
Config MakeDefaultConfig();

enum class Severity { Info, Warning, Error };
struct Diagnostic { Severity sev; std::string field; std::string message; std::string action; };

struct ValidationPolicy { bool clampNumbers = true, dropUnknownMetrics = true,
                          appendMissingMetrics = true, preserveUnknownKeys = false; };
bool ValidateAndRepair(Config&, const ValidationPolicy&, std::vector<Diagnostic>&);

// ── 路径 / 加载 / 保存 ──────────────────────────────────────────────────
struct ConfigPaths { std::filesystem::path file; bool portable = false; bool writable = true; };
ConfigPaths ResolveConfigPaths(int argc, const wchar_t* const* argv);

struct LoadResult {
    Config                   config;
    std::vector<Diagnostic>  diagnostics;
    bool                     fileExisted = false;
    bool                     repaired    = false;   // 有字段被夹紧/回退/补齐
    bool                     readonly    = false;   // 文件不可写或版本过高
};
LoadResult LoadConfig(const ConfigPaths&);
bool       SaveConfig(const Config&, const ConfigPaths&, std::string* error);   // 原子写 + .bak

// ── JSON 层（纯函数，可脱离 Windows API 单测）───────────────────────────
std::string ConfigToJson(const Config&);                       // 固定字段顺序，2 空格缩进，尾随换行
LoadResult  ConfigFromJson(std::string_view utf8, const ValidationPolicy& = {});

// ── 运行时持有：原子快照 + 热重载 + 订阅 ────────────────────────────────
class ConfigStore {
public:
    ConfigStore(ConfigPaths paths, Config initial);

    std::shared_ptr<const Config> Snapshot() const noexcept;   // 任意线程；采集热路径只用这个
    uint64_t Revision() const noexcept;                        // 单调递增，变更计数

    bool Apply(const Config& next, bool persist);              // 校验→修复→发布→(可选)落盘
    bool Mutate(const std::function<void(Config&)>& fn, bool persist);  // UI 侧批量改（读-改-写合并）

    void SetAutoSave(bool on, int debounceMs = 500);           // 连续改动合并成一次写盘
    void SetFileWatch(bool on, int pollMs = 2000);             // mtime 轮询（不占用额外线程池资源）
    void Flush();                                              // 退出前强制落盘

    using Callback = std::function<void(uint64_t rev, const Config&)>;
    void Subscribe(Callback);                                  // 仅在实际变更时回调
    const std::vector<Diagnostic>& Diagnostics() const;
    std::string LastError() const;
};

// ── 命令行/会话覆盖（不落盘）────────────────────────────────────────────
bool ApplyCommandLineOverrides(Config&, int argc, const wchar_t* const* argv,
                               std::vector<Diagnostic>&);

} // namespace wpm::config
```

**线程模型**

| 操作 | 调用方 / 线程 | 阻塞性 |
|---|---|---|
| `Snapshot()` | 采集线程、渲染线程 | 无锁（原子 `shared_ptr` 加载） |
| `Apply` / `Mutate` | UI 线程（设置界面、热键） | 内部短临界区（仅改写者之间互斥） |
| 文件监听 | 采集线程每 2 s 检查 mtime | 非阻塞 |
| 订阅回调 | 发布变更的线程（可能是监听线程） | **UI 侧需自行 marshal 到 UI 线程** |

**热重载语义：** 检测到 mtime 变化 → 解析 → 校验修复 → 若内容与当前不同则发布新 `Revision` 并回调；若解析失败则保留当前配置并记 Error（不覆盖文件）。

## 7. 完整默认配置（可作为随包默认文件）

```json
{
  "schemaVersion": 1,
  "meta": { "writtenBy": "winperfmon 0.1.0", "lastSavedUtc": "" },
  "layout": {
    "orientation": "horizontal", "groupByCategory": true, "showLabels": true, "showUnits": true,
    "itemGapDp": 8, "groupGapDp": 14, "align": "start", "wrap": false, "maxWidthDp": 960,
    "hideMissing": true
  },
  "metrics": [
    { "id": "cpu.temp.package", "visible": true },
    { "id": "cpu.power.package", "visible": true },
    { "id": "cpu.freq.avg", "visible": true },
    { "id": "cpu.usage", "visible": true },
    { "id": "gpu.temp.core", "visible": true },
    { "id": "gpu.power", "visible": true },
    { "id": "gpu.freq.core", "visible": true },
    { "id": "gpu.freq.mem", "visible": true },
    { "id": "gpu.usage", "visible": true },
    { "id": "mem.usage", "visible": true },
    { "id": "fps.instant", "visible": true },
    { "id": "fps.avg", "visible": true },
    { "id": "fps.low1", "visible": true },
    { "id": "cpu.temp.cores", "visible": false }
  ],
  "colors": {
    "themeId": "dark",
    "background": "#CC101418", "panelBorder": "#40FFFFFF",
    "label": "#B3FFFFFF", "value": "#FFFFFFFF", "unit": "#99FFFFFF",
    "ok": "#FF4CAF50", "warn": "#FFFFB300", "critical": "#FFE53935", "accent": "#FF4FC3F7"
  },
  "text": { "baseSizeDp": 14, "boldValue": true, "fontFamily": "", "minSizeDp": 10 },
  "window": {
    "topmostPolicy": "always", "clickThrough": true, "opacity": 0.90, "scale": 1.0,
    "position": { "mode": "preset", "preset": "topLeft", "x": 0, "y": 0 },
    "marginDp": 12, "monitorId": "", "hideInFullscreen": false,
    "showTrayIcon": true, "startWithWindows": false
  },
  "update": {
    "sensorIntervalMs": 500, "overlayRefreshFps": 2, "panelRefreshFps": 4,
    "pauseWhenHidden": true, "lowPowerOnBattery": false
  },
  "fps": {
    "mode": "auto", "targetProcess": "", "autoStartOnGame": true,
    "keepBufferFrames": 65536, "lowPercentile": 0.01,
    "stutterFactor": 2.0, "stutterBaseline": "mean", "showPresentMode": true
  },
  "gpu": { "selectedIndex": -1 }
}
```

> 上面示例的 `visible: true` 共 **13 项**，与 §4 表格「默认可见」列的 ✅ 完全一致（`cpu.usage`、`cpu.freq.avg`、`cpu.temp.package`、`cpu.power.package`、`mem.usage`、`gpu.usage`、`gpu.freq.core`、`gpu.freq.mem`、`gpu.temp.core`、`gpu.power`、`fps.instant`、`fps.avg`、`fps.low1`）。
> 程序写盘时会把 `metrics` **按指标目录全量展开**（不只是上面示例的 14 项），保证「顺序 + 可见性」在一个文件里完整可见、可手工编辑。

## 8. 版本与迁移

| 变更类型 | 是否递增 `schemaVersion` |
|---|---|
| 新增可选字段（有默认值） | 否 |
| 新增指标 ID | 否（自动追加到末尾） |
| 删除字段、改名、改变字段语义 | **是** |
| 枚举新增取值 | 否（旧值语义不变） |

迁移链：`Migrate_v1_to_v2(JsonValue&, std::vector<Diagnostic>&)` 逐级纯函数，可单测；迁移在 `ConfigFromJson` 内部自动执行。
指标别名表（改名兜底）：`{ "cpu.freq": "cpu.freq.avg", "gpu.temp": "gpu.temp.core" }`，加载时先查别名再查目录。

## 9. 开放问题（待拍板）

| # | 问题 | 影响 |
|---|---|---|
| 1 | ~~产品名 / 仓库名~~ | ✅ **已决**：产品名 / EXE / 仓库名 = `winperfmon`，配置目录 = `%APPDATA%\winperfmon\config.json`；本行保留以便引用（不再是开放问题） |
| 2 | 是否需要「每游戏独立配置档案」（按进程名切换布局与指标集） | 需要新增 `profiles[]` 区块，建议 v2 |
| 3 | 是否需要配置导入/导出、`--print-config` 诊断输出 | 便于社区互助排障，成本低，建议 v1 就做 |
| 4 | 是否开放阈值（warn/critical）自定义 | 当前由指标目录内置，v2 可开放 |
| 5 | 是否允许自定义字体族 | 首版只允许系统 UI 字体，v2 再开放列表 |
| 6 | 多显示器位置是否需要逐屏记忆 | 当前只记 `monitorId` + 一组坐标 |
| 7 | ~~是否需要 GPU 选择字段~~ | ✅ **已决（§6.1-2）：采纳 `gpu.selectedIndex`**（`-1` = 自动选渲染 GPU，`0`–`15` = 枚举下标），新增 §3.6.1 `gpu` 区块与 `GpuConfig` 类型；枚举顺序与虚拟适配器排序规则见该节。本行保留以便引用（不再是开放问题） |
