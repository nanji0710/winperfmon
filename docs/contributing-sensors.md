# 传感器支持贡献流程（Super I/O / 风扇 / EC）

> **状态**：设计稿 v1。
> **本文权威定义**：`wpm.exe --probe` 的输出格式（`release-github.md` §10 与 `privacy.md` 都引用本文件）。
> **不重复定义数值**：芯片寄存器映射、入口序列、已知坑一律以 `gpu-fans.md` §3 为准，本文只规定"怎么把数据交上来、怎么变成芯片表条目"。

## 1. 为什么需要这份流程

Windows 上没有可复用的独立 Super I/O 库（`gpu-fans.md` §3.4），主板风扇/温度/电压的可用性**强依赖具体主板与芯片**：同一颗 NCT6779D 在不同主板上，风扇通道对应的物理风扇、分频寄存器、甚至芯片上报的 ID 都可能不同（例：`NCT6796D` 实际是 `NCT5585D`、`NCT6799D` 实际是 `NCT6796DS`）。

因此本项目的 Super I/O 支持是**数据驱动**的：芯片表（`sensor-tables/*.json`）可以随版本更新，用户不需要重新编译程序。社区提交的原始探测数据，就是扩充这张表的唯一可靠来源。

**我们不接受**："帮我加一下 XX 主板"但没有原始数据的请求；也**不接受**直接提交补丁而不给探测数据（无法验证）。

## 2. 三步流程

1. **跑探针**（需要管理员权限，因为要经 Ring0 做端口 I/O）：
   ```
   wpm.exe --probe > probe.txt
   ```
   未安装 PawnIO 时探针仍可运行，但只会输出免驱部分（GPU/C PU 频率），并在 `issues=` 里记 `pawnio=absent`。
2. **自查并匿名化**：默认输出已掩码主机名/用户名/序列号/UUID/MAC/IP；提交前请自己再通读一遍 `probe.txt`，删掉任何你不想公开的内容（写字楼名、公司资产编号等）。**不要提交** `--probe --full` 的输出，除非你已经手工清理并确认可公开。
3. **开 Issue**：选择「传感器支持（Super I/O / 风扇）」模板，附上 `probe.txt`，并填写模板要求的物理含义问卷（哪个通道对应 CPU_FAN / SYS_FAN1 / PUMP 等）。

## 3. `--probe` 输出格式（`probeVersion=1`）

纯文本、UTF-8（无 BOM）、`key=value` 行式格式、按下面的顺序输出、字段名固定；数组型字段用 `.N` 后缀（从 1 开始）、`1` 起计。**格式一旦发布只在 `probeVersion` 提升时变更**，解析方（维护者脚本）按 `probeVersion` 分支。

```ini
probeVersion=1
appVersion=0.1.0
osBuild=26100.1
admin=true
pawnio=present
hvci=off
ring0Backend=PawnIO/AMDFamily17.p

[system]
dmi.board.vendor=<masked-or-value>
dmi.board.name=B650M DS3H
dmi.bios.version=F32
dmi.system.product=<desktop|masked>

[superio]
lpc.portPair=0x2E/0x2F
lpc.entry=ok
vendor=nuvoton
chip.idReg=0x20
chip.id=0xC5
chip.idHi=0x60
chip.candidate=NCT6779D
chip.bank=0
quirks=NCT6796D->NCT5585D

[fan.1]
count.raw=33120
count.loReg=0x4B
count.hiReg=0x4C
divisor=1
rpm=1350000/(33120*1)=40
pwm=0x80
control=manual

[temp.1]
raw=0x41
celsius=65.0
halfDegree=false
source=0x29

[volt.1]
raw=0xC0
volts=1.104
nominal=1.100
gain=0.012

[gpu.1]
vendor=nvidia
deviceId=0x2684
fan.percent=42
fan.rpm=n/a

[issues]
=IT8792 reported 0x8883 at probe time (state machine collision)
```

字段要点（供解析方与提交者共同确认）：

| 段落 | 关键字段 | 说明 |
|---|---|---|
| 头部 | `admin` / `pawnio` / `hvci` / `ring0Backend` | 决定这份数据"探到了哪一层"：`admin=false` 或 `pawnio=absent` 时 `[superio]` 段必然为空或仅含端口探测结果 |
| `[system]` | `dmi.*` | 用于识别"同芯片不同板"的差异；默认掩码策略见 §5 |
| `[superio]` | `chip.*` / `quirks` | 芯片 ID 高字节（Nuvoton 的 `0xC5/0x60`）必须都给；`quirks` 记录入口状态机异常（如 IT8792 谎报 `0x8883`） |
| `[fan.N]` | `count.loReg` / `count.hiReg` / `divisor` | 原始计数与寄存器出处**必须保留**，否则无法判定 8 位还是 16 位计数、以及分频位域 |
| `[temp.N]` | `raw` / `halfDegree` | `raw` 是原始字节，`celsius` 只是换算结果，便于核对 0.5 °C 步进 |
| `[volt.N]` | `raw` / `nominal` / `gain` | 电压通道必须给 `nominal`，不同板的分压比不同 |
| `[gpu.N]` | `fan.percent` / `fan.rpm` | NVIDIA 一般只有百分比（`rpm=n/a`），AMD/Intel 可给 RPM |
| `[issues]` | 任意 | 探测过程中的异常与警告，原样保留；这是最有价值的排障信息 |

**对拍要求**：提交时请在模板里给出至少一项对拍值（BIOS/HWiNFO/AIDA64 的同一读数），例如"我的 CPU_FAN 在 BIOS 里是 1200 RPM，`[fan.1]` 报 1198 RPM"。没有对拍值的数据我们只能当"未验证"。

## 4. 从探测数据到芯片表条目

维护者按下面的流程处理（写在这里是为了让贡献者知道数据会怎么被使用）：

1. 用 `tools/probe2table.py`（待开发）把 `probe.txt` 转成候选条目，人工核对寄存器与公式；
2. 条目落入 `sensor-tables/<vendor>-<chip>.json`，字段形状与 `gpu-fans.md` §3 数据驱动层一致：`{ chip, chipId[], bank, fans[]{index,countLo,countHi,divisorReg,divisorShift,rpmFormula}, temps[], volts[], quirks[] }`。**本文件不重复数值**：偏移与公式的已知取值见 `gpu-fans.md` §3；
3. 为条目加一条**回归用例**：canned 原始寄存器值 → 期望 RPM / °C（无硬件也能在 CI 里跑，见 `roadmap.md` M1）；
4. 在 `CHANGELOG.md` 的「传感器支持变化」节记录：新增/修正了哪些主板或芯片；
5. 若新数据与既有条目冲突，**以探测原始数据为准**；解释不通时优先加"主板特判"而不是改通用公式（已有先例：`NCT6796D→NCT5585D`、`NCT6799D→NCT6796DS`、`NCT6687D→NCT6687DR`）。

## 5. 隐私红线

| 规则 | 说明 |
|---|---|
| 默认掩码 | 主机名、用户名、序列号、UUID、MAC、IP 一律掩码为 `<masked>`；`dmi.board.name` 与芯片 ID 需要保留（否则数据无用） |
| 不做上传 | 探针**永不联网**，只写 stdout/文件（`privacy.md` §2） |
| `--probe --full` | 仅在排障时使用，输出未掩码字段，并在末尾打印醒目警告；我们不接受它作为提交附件 |
| 不接受的数据 | 含未掩码序列号的日志、截图（无原始寄存器值）、要求我方联网采集的任何方案 |

## 6. 不被接受的提交（省彼此时间）

- 只有截图、没有 `probe.txt`；
- 只有"风扇读不到"，没有 `[superio]` 段（通常是没以管理员运行，或端口对被占用）；
- 要求支持某笔记本 EC，但 `[issues]` 里没有 EC 邮箱访问结果——笔记本 EC 映射因 OEM 而异，没有数据无法推进；
- 数据与 `probeVersion` 不匹配（旧版本输出会被直接转成待复核，但不保证处理）。

## 7. 预期管理

芯片表由维护者**人工核对**后并入版本，没有 SLA、也没有"下个版本一定支持"的承诺；首版（M1）只承诺已验证过的芯片与主板。提交前请确认你接受这一点。

## 8. 相关文档

- `gpu-fans.md` §3：Super I/O 寄存器映射、入口序列、已知坑（数值权威）
- `privacy.md`：隐私与安全声明（`--probe` 段落引用本文件）
- `release-github.md` §10：为什么不接受"承诺支持"式请求
- `roadmap.md`：M1 的传感器验收标准与无硬件 CI 方案
