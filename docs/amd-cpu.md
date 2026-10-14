# AMD CPU 支持设计（Zen 架构）

> 文档状态：**设计稿 v2（8 项技术事实已核实：6 ✅ / 2 ❌ 无一手来源并给出探测方案，详见 `amd-facts.md`）** · 关联：`winperfmon-dev-plan.md` §3.1（Intel 对照路径）与 §2（模块边界）、`config-schema.md` §4（指标 ID 目录）
> 来源：主方案 §3.1 的 AMD 补充行（数值已对照 nwinfo 源码与 LHM/RyzenAdj 实现核实），专项核实任务已完成，结论与证据见 `amd-facts.md`；本文被改动处均标注「（已核实，见 amd-facts.md N）」。

## 1. 目标与范围

| 项 | 内容 |
|---|---|
| 覆盖范围 | family **17h**（Zen / Zen+ / Zen2）、**19h**（Zen3 / Zen4）、**1Ah**（Zen5）的桌面 CPU、移动 APU、HEDT（Threadripper 视 CCD 数量自适应展示） |
| 不做 | family 0F/10（K10 及更早）只提供免驱降级；不做超频/调压/调功耗（**本工具是只读监控**） |
| 对外一致性 | 必须输出与 Intel 路径**同一套指标 ID**（`config-schema.md` §4），UI 层不区分厂商；差异只体现在「能不能拿到」与提示文案 |
| 硬约束 | 不注入、不挂钩；Ring0 一律经 PawnIO；硬件不暴露的项按 `hideMissing` 隐藏或标 `N/A`，**绝不填假值** |

## 2. CPU 识别与拓扑

| CPUID | 用途 |
|---|---|
| `0x00000001` | 基础 family/model/stepping（base family `0xF` + extended family → 得 `0x17` / `0x19` / `0x1A`） |
| `0x80000001` | 扩展特性位（拓扑扩展支持；**APERF/MPERF 不适用于 AMD**——已核实，见 amd-facts.md 3） |
| `0x80000002..4` | 品牌串（如 `AMD Ryzen 7 5800X3D 8-Core Processor`）→ 界面显示 + 型号特判（`X3D`、`HS/U/HX` 等后缀） |
| `0x80000008` | 物理地址位宽、核心数（NC 字段） |
| `0x8000001E` | 拓扑：threads per core、compute unit（CCX）id、node id |
| `0x80000026` | 扩展拓扑（Zen4/Zen5 的 CCD 层级） |

**家族判定与代表型号**

| family | 判定 | 代号 | 代表型号 | 备注 |
|---|---|---|---|---|
| `0x17` | ext family = `0x8` | Zen / Zen+ / Zen2 | 1800X / 2700X / 3900X | 早期 PM 表版本差异最大 |
| `0x19` | ext family = `0xA` | Zen3 / Zen4 | 5800X3D / 7950X | 含 3D V-Cache 特例 |
| `0x1A` | ext family = `0xB` | Zen5 | 9950X / 9800X3D | 频率公式疑似变更（§5.1） |

**必须解析出的运行时信息**：物理核数、逻辑核数、CCX/CCD 划分（用于「每核频率/温度」的分组展示）、是否含 3D V-Cache（品牌串 + model 号双条件判定，**不能只看品牌串**）。这些信息在启动时探测一次，缓存到进程生命周期，并写入诊断导出（供社区排障）。

## 3. 访问通道矩阵

| 通道 | 前提 | 能拿到 | 拿不到 / 失败后果 |
|---|---|---|---|
| **免驱**（PDH / `NtQuerySystemInformation`） | 无 | CPU 占用、内存；频率的粗略值（PDH `% Processor Performance` × 基频，**AMD 上的可用性与精度待核实**） | 温度、功耗、电压、风扇 |
| **Ring0 · MSR**（PawnIO `AMDFamily17.p`，17h/19h/1Ah 同一模块覆盖——已核实，见 amd-facts.md 6） | PawnIO 已安装 + 模块加载成功 | RAPL 能量（封装功耗）、PState（当前频率定义） | 有效频率（`IA32_APERF/MPERF` 在 AMD 上不可用——已核实，见 amd-facts.md 3）、每核频率、Tdie、SVI2 电压、PPT/TDC/EDC |
| **Ring0 · SMN**（PawnIO `ioctl_read_smn`；其内部实现**就是** PCI 配置空间 device `0:0:0` 的 `0x60` 地址 / `0x64` 数据间接访问——已核实，见 amd-facts.md 1） | 同上 | Tctl 温度（SMN `0x00059800`）、SMU 邮箱交互 | 结构化遥测 |
| **SMU 邮箱 + PM 表**（PawnIO `RyzenSMU.p`：`ioctl_resolve_pm_table` / `ioctl_update_pm_table` / `ioctl_read_pm_table` / `ioctl_send_smu_command`——已核实，见 amd-facts.md 5、6） | 同上 + 家族/型号偏移表匹配 | Tdie、每 CCD/每核温度、每核频率、PPT/TDC/EDC、SVI2 电压 | 表不匹配 → 整体降级 |

**提供者内部按「能力探测 → 首选 → 降级」链实现**，探测只在启动时做一次，结果与原因一起进诊断导出（例：`SMN: unavailable (PawnIO module load failed, code 0x…)`）。

## 4. 指标映射（对外 ID 与 `config-schema.md` §4 一致）

| 指标 ID | 首选来源 | 备用 | 免驱 | 期望精度 |
|---|---|---|---|---|
| `cpu.freq.avg` | PM 表每核频率求平均（`IA32_APERF/MPERF` 在 AMD 上不可用，首选已改为 PM 表——已核实，见 amd-facts.md 3） | 绑核 PState 当前值 | PDH × 基频 | ±50 MHz |
| `cpu.freq.percore` | PM 表每核频率 | 绑核 PState 当前值 | ✗ | ±50 MHz |
| `cpu.temp.package` | PM 表 Tdie | SMN Tctl（按 §5.2 的 `−49 °C` 量程与型号 offset 修正——已核实，见 amd-facts.md 2） | ✗ | ±2 °C（对拍 HWiNFO） |
| `cpu.temp.cores` | PM 表每核/每 CCD 温度 | ✗ | ✗ | 参考值 |
| `cpu.temp.headroom` | 按 SKU 取的温度上限 − 当前温度（无家族级常数；取不到 SKU 即不可用——已核实，见 amd-facts.md 7） | ✗ | ✗ | 上限表精度 |
| `cpu.power.package` | RAPL `0xC001029B` 能量差分 | PM 表 PPT（并列显示，语义不同见 §5.3） | ✗ | ±5% |
| `cpu.power.soc` | PM 表（SoC/Uncore 域） | ✗ | ✗ | 参考值 |
| `cpu.power.cores` | ✗（AMD 无核级 RAPL） | PM 表核心功耗（若暴露） | ✗ | 参考值 |
| `cpu.voltage` | SVI2（PM 表） | ✗ | ✗ | ±10 mV |
| `cpu.throttle.reason` | PM 表限制位 / 温度上限接近判定 | ✗ | ✗ | 定性 |

## 5. 寄存器与数据源细节

### 5.1 频率

- **有效频率**：**`IA32_APERF (0xE7)` / `IA32_MPERF (0xE8)` 是 Intel 的寄存器，锐龙上不可用**（Linux 只对 Intel 型号授予 `X86_FEATURE_APERFMPERF`）；AMD 自己的对应物是 `MSRC000_00E7 (0xC00000E7)` / `MSRC000_00E8 (0xC00000E8)`，PawnIO 只读白名单可读，但**未找到任何权威实现**用它算有效频率——**AMD 频率一律以 PM 表每核频率为首选**（已核实，见 amd-facts.md 3）。若日后要试 `0xC00000E7/E8`：MSR 为**核私有**，读某一核必须 `SetThreadAffinityMask` 绑核后再读；多核平均按「轮询全部核，每次采样取一个核」分摊开销（例如 16 核 × 500 ms 周期 → 每核 32 ms 一次）；且只在探测阶段与 PM 表值比对，偏差 < 3% 才启用。
- **PState**：当前 PState 寄存器 `0xC0010063`；PState 定义 `0xC0010064 + n`（n = 0..7），`CpuFid = bits[7:0]`、`CpuDid = bits[13:8]`。
- **CoreCOF 公式**（已核实，见 amd-facts.md 4）：**17h/19h：`CoreCOF = 200 MHz × CpuFid / CpuDid`**（`CpuFid = bits[7:0]`、`CpuDid = bits[13:8]`；等价 `fid/did × 2` 再乘 100 MHz 基准）；**Zen5（1Ah）为 `CoreCOF = CpuFid × 5 MHz`，且 `CpuFid` 扩到 12 位 `bits[11:0]`**——原文「Zen5 疑似 `CpuFid × 50 MHz`」**已更正**（差一个数量级，依据 `LibreHardwareMonitorLib/Hardware/Cpu/Amd17Cpu.cs:439-441`）。实现上按家族分支，并用「PState 推算值 vs PM 表实测频率」做启动自检：两者长期偏差 > 10% 则记录诊断并只显示 PM 表频率。
- **基频/参考时钟**：默认 100 MHz；型号表可覆写。自检：用两个已知 PState 的理论频率与实测比对反推基准，偏差 > 1 MHz 则采用实测值。

### 5.2 温度

- **Tctl**：SMN `0x00059800`（`THM_TCON_CUR_TMP`，17h/18h/19h/1Ah 共用同一控制寄存器），`Tctl = ((reg >> 21) & 0x7FF) × 0.125 °C`（已核实，见 amd-facts.md 2）。
- **Tdie = Tctl − offset**（已核实，见 amd-facts.md 2）：offset **不是 PM 表字段**，来源有两个——① 通用 **`−49 °C` 量程偏移**：`THM_TCON_CUR_TMP` 的 `bit19 (CUR_TEMP_RANGE_SEL)` 置位、或 `bits[17:16] (CUR_TEMP_TJ_SEL) == 0b11` 时应减 49 °C（BIOS 写入，EPYC/服务器常见）；② **按品牌串查表的型号常数**（只有 17h 的 X / Threadripper 条目：1600X/1700X/1800X 20 °C、2700X 10 °C、TR 19xx/29xx 27 °C，表内记负值再相加）。两者都放进数据驱动表（§6），并做合理性约束（`Tdie` 必须落在 `-10..125 °C`，否则回退显示 Tctl 并记诊断）。
- **每核 / 每 CCD 温度**：来自 PM 表字段（按家族/型号偏移表取）。
- **温度上限（TjMax 等效）**：用于 `cpu.temp.headroom`。**AMD 没有 Intel `MSR_IA32_TEMPERATURE_TARGET (0x1A2)` 那样的 Tjmax 寄存器，也没有可用的家族级常数**（原文「19h 95/115 °C」**已更正**，「115 °C」未找到一手来源）；上限只能**按 SKU 从 amd.com 官方规格页的 `Max. Operating Temperature (Tjmax)` 取**，随包维护 SKU 表：9950X = 95 °C、7800X3D = 89 °C、5800X3D = 90 °C、Ryzen 7 PRO 2700X = 95 °C（已核实，见 amd-facts.md 7）。拿不到 SKU 匹配时就显示「距上限」为不可用，而不是猜一个值。
- 已知特例（**已更正**，已核实，见 amd-facts.md 2、7）：**X3D 型号未发现专属的 Tctl offset**——19h/1Ah/X3D 在所有公开 offset 表中都没有条目，一律按 offset = 0（Tctl == Tdie）处理；X3D 的实测差异体现在 **Tjmax（5800X3D 90 °C / 7800X3D 89 °C）**，不是读数偏置。探测式验证：启动时读 `THM_TCON_CUR_TMP` 的 bit19 / bits[17:16] 判定量程，并与 HWiNFO 对拍，偏差 > 5 °C 记诊断。

### 5.3 功耗

- **RAPL 单位**：MSR `0xC0010299`；能量单位 `bits[12:8]` → 1 J = `1 / 2^ESU`，功率单位 `bits[3:0]`。**封装能量**：MSR `0xC001029B`（32 位，按 `2^32` 回绕处理，差分得到 ΔE，功耗 = ΔE / Δt）。AMD 侧**只有封装级 RAPL**，没有 Intel 那样的 PP0/PP1/DRAM 分项。
- **「读数翻倍」自检（**已更正**，已核实，见 amd-facts.md 8）**：**「同一物理域存在两个能量 MSR、读数成 2 倍关系」未找到一手来源**，因此**不再「自动折半」**。已证实的 2× 级误差全部来自软件聚合：① 域叠加求和（`PKG` 已包含 `CORE/PP0`）；② `0xC001029A` 是 core/PP0 域、`0xC001029B` 是 package 域，**不是同一物理域**；③ 每个逻辑处理器读同一个 package 计数再求和 → N 倍；④ 能量单位 `ESU` 取错。实现约束：只在**同一个 MSR** 上做前后差分与比值，禁止跨域/跨线程求和；与 PM 表 PPT 的长期比值只作为**诊断信号**（长期 ≈ 2.0 时记录一次诊断并提示可能误聚合，不静默改数）。
- **PM 表 PPT / TDC / EDC**：语义是**限制值/墙**，与 RAPL 的实测能量**不同**——UI 上必须用不同指标名展示（PPT 不作 `cpu.power.package`），避免用户把「功耗墙」误读为「当前功耗」。

### 5.4 电压

SVI2 遥测来自 PM 表字段（核心电压）。免驱与仅-MSR 模式下该指标不可用。

### 5.5 访问实现

| 操作 | 实现 | 备注 |
|---|---|---|
| 读 MSR | PawnIO 模块 IOCTL（读） | 只读；逐寄存器容错（单个 MSR 触发 `#GP` 不应导致整个提供者失败） |
| 读/写 SMN | 首选 PawnIO `AMDFamily17.p` 的 **`ioctl_read_smn`**（in[0] = SMN 地址 → out[0] = 值）；该 IOCTL 的内部实现**就是** PCI 配置空间 device `0:0:0` 的 `0x60`（地址）/ `0x64`（数据）间接访问，故 `0x60/0x64` 作为自建备选路径（已核实，见 amd-facts.md 1）。SMU 邮箱另可用 `RyzenSMU.p` 的 `ioctl_read_smu_register` / `ioctl_write_smu_register` / `ioctl_send_smu_command`（内部走 `0xC4/0xC8`，Carrizo 系 `0xB8/0xBC`）（已核实，见 amd-facts.md 6） | SMN「写」**仅**用于向 SMU 邮箱投递查询命令（必要的间接读），绝不写电压/频率/功耗控制寄存器；触碰 PCI 的 IOCTL 调用前须获取 `\BaseNamedObjects\Access_PCI` 互斥体 |
| 读 PM 表 | 优先 `RyzenSMU.p` 的 `ioctl_resolve_pm_table`（out = [版本, 表基址]）→ `ioctl_update_pm_table` → `ioctl_read_pm_table`（out 为表内容 QWORD 数组，最多 `PAGE_SIZE/8` = 512 项）（已核实，见 amd-facts.md 6）；退回自建路径时按「邮箱取基址 → Ring0 读物理地址」解析 | 表头含版本/大小，**版本不匹配就放弃**（不猜偏移） |

### 5.6 SMU 邮箱与 PM 表获取流程

1. 按家族/型号组从数据驱动表取 SMU 邮箱三元组（message / argument / response；**已核实，见 amd-facts.md 5**）：主集合 `msg 0x3B10528 / arg 0x3B10998 / rsp 0x3B10564`（17h、19h 桌面与服务器）；APU 组（Vangogh/Rembrandt/Phoenix/Mendocino/Hawk Point/Strix Halo 等）用 `rsp 0x3B10578`；Strix Halo/Glinda 类用 `0x3B10928 / 0x3B10998 / 0x3B10978`。**arg 基址恒为 `0x3B10998`**；数据表存**候选数组**，运行时以「发只读探针 `getVersion (0x02)`、响应非 0 且 `SMU_OK`」择优，胜出项写入诊断导出。
2. 写入查询命令（命令号按家族/型号组取，**已核实，见 amd-facts.md 5**）：**「取版本 / 取表基址 / 刷新到 DRAM」是三条不同命令**——Zen2/Zen3 桌面与服务器 `0x08 / 0x06 / 0x05`，Zen4/Zen5 `0x05 / 0x04 / 0x03`，APU 组 `0x06 / 0x66 / 0x65`（刷新时 arg = 3），17h 早期/HEDT `0x0D / 0x0B,0x0C,0x0E / 0x0A`；轮询 response 直到非 0 或超时（超时 → 判为不支持，落降级链）。
3. 校验返回值里的表版本/大小与数据驱动表匹配，然后读取 PM 表。
4. **全程只读语义**：除「向邮箱投递查询命令」外不发任何控制命令；命令号白名单固定写在代码里，不从数据包加载（防止数据包被替换后执行任意 SMU 命令）。

## 6. 数据驱动表 `amd_tables.json`（随包分发、可热更新）

家族/型号做键，偏移与常数做值，程序不重编译即可支持新型号（主方案 §11「数据驱动层」）。

```json
{
  "tableVersion": 3,
  "entries": [
    {
      "family": "0x19", "models": ["0x21"],
      "brandMatch": ["5800X3D"],
      "refClockMHz": 100,
      "tctlOffset": 0,
      "tempLimitC": 90,
      "tempLimitSource": "https://www.amd.com/en/products/processors/desktops/ryzen/5000-series/amd-ryzen-7-5800x3d.html",
      "smuMailboxCandidates": [
        { "msg": "0x3B10528", "arg": "0x3B10998", "rsp": "0x3B10564" },
        { "msg": "0x3B10528", "arg": "0x3B10998", "rsp": "0x3B10578" },
        { "msg": "0x3B10928", "arg": "0x3B10998", "rsp": "0x3B10978" }
      ],
      "smuCmds": { "getVersion": "0x02", "pmTableVersion": "0x08", "pmTableBase": "0x06", "pmTableTransfer": "0x05" },
      "pmTable": { "version": "0x380804", "fields": { "tdie": 0, "svi2CoreV": 0, "ppt": 0, "coreClk": 0 } }
    }
  ]
}
```

| 约束 | 说明 |
|---|---|
| 数值来源 | 只搬常数（RyzenAdj LGPL-3.0 / ZenStates-Core GPL-3.0 / LHM MPL-2.0 的**数值**），不搬代码；来源与许可证在 NOTICE 里列明 |
| 校验 | `tableVersion` + 每条目的 `family`/`model` 精确匹配；匹配失败 → 用「仅 Tctl + RAPL」的保守模式 |
| 安全 | 数据包**不含**可执行逻辑与命令号白名单之外的内容；SMU 命令号在代码内固定 |
| 失败语义 | 表加载失败 → 内置表兜底 → 仍失败 → 降级为 SMN Tctl + MSR RAPL；界面提示「AMD 遥测表未匹配，部分指标不可用」 |

## 7. 降级矩阵

| 场景 | 频率 | 温度 | 功耗 | 电压 | 界面提示 |
|---|---|---|---|---|---|
| 无 Ring0（免驱） | 粗（PDH） | ✗ | ✗ | ✗ | 「CPU 温度/功耗需要驱动：安装后可解锁」+ 一键查看说明 |
| 有 MSR、无 SMN/PM 表 | PState（`IA32_APERF/MPERF` 在 AMD 上不可用——已核实，见 amd-facts.md 3） | ✗ | RAPL 封装 | ✗ | 「仅能读取功耗：温度遥测不可用」 |
| 仅 SMN（无 PM 表匹配） | PState | Tctl（不修正） | RAPL | ✗ | 「型号未匹配遥测表，温度为 Tctl 未修正值」 |
| 全通 | 全部 | Tdie + 每 CCD | RAPL + PPT | SVI2 | 正常 |
| 虚拟机 / 嵌套虚拟化 | 视 MSR 可读性，逐寄存器容错 | ✗ | ✗ | ✗ | 「虚拟化环境：硬件遥测不可用」 |

## 8. 验收与测试矩阵

| 平台 | 型号示例 | 验收内容 |
|---|---|---|
| Zen2 | 3900X / 3700X | 频率 ±50 MHz、Tctl/Tdie 与 HWiNFO ±2 °C、封装功耗 ±5% |
| Zen3 | 5800X / **5800X3D** | 同上 + X3D offset 特例不出现 ±10 °C 级偏差 |
| Zen4 | 7950X / **7800X3D** | 同上 + 多 CCD 温度分组正确 |
| Zen5 | 9950X / 9800X3D | 同上 + `CoreCOF` 家族分支正确（§5.1） |
| APU / 移动 | 7840HS / 8845H | 温度与功耗可用性、笔记本 EC 限制下的提示文案 |
| Threadripper | 7960X | 多 CCD 展示不退化、核心数 > 32 时的采样开销仍在预算内 |

**环境矩阵**：HVCI 开 / 关；无 PawnIO；无管理员权限；PresentMon 有 / 无；纯电池模式。

**自动化（无硬件 CI）**：用可注入的伪 Ring0 后端喂 canned 寄存器值，单测覆盖——回绕（`2^32`）、**RAPL 聚合与能量单位校验**（同 MSR 差分、禁止跨域/跨线程求和——已核实，见 amd-facts.md 8）、Tctl/Ddie 越界回退、`−49 °C` 量程分支（bit19 / bits[17:16]）、PM 表版本不匹配、家族分支公式（含 Zen5 `CpuFid × 5`）、邮箱候选择优、绑核采样调度；这些单测必须能在 GitHub Actions 的 windows-latest 上跑（不需要真实硬件、不需要驱动）。

## 9. 待核实清单（编码前必须关闭）

> 核实结果见 `amd-facts.md`；下表**未删除任何一行**，每行追加「状态与证据」列。

| # | 待核实项 | 影响 | 状态与证据（证据指向 `amd-facts.md` 节号） |
|---|---|---|---|
| 1 | SMN 访问通道（PCI `0x60/0x64` vs PawnIO SMN IOCTL 名称） | Ring0 层接口设计 | ✅ 已关闭（amd-facts.md 1：`AMDFamily17.p:193` 的 `ioctl_read_smn` 内部即 device `0:0:0` 的 `0x60/0x64`；`RyzenSMU.p` 另用 `0xC4/0xC8`） |
| 2 | Tdie offset 的来源与符号方向 | 温度正确性（±10 °C 级误差风险） | ✅ 已关闭（机制与符号：amd-facts.md 2）；X3D / 19h / 1Ah 专属 offset ❌ 未找到一手来源 → 探测方案：启动读 `THM_TCON_CUR_TMP` 的 bit19 / bits[17:16] 判 `−49 °C` 量程，型号表缺省 0，与 HWiNFO 对拍偏差 > 5 °C 记诊断并回退 Tctl |
| 3 | AMD 上 APERF/MPERF 是否可用 | 频率路径首选/备用顺序 | ✅ 已关闭（不可用：amd-facts.md 3，首选已改为 PM 表每核频率）；探测方案：试读 `0xC00000E7/0xC00000E8` 差分 × 基频，与 PM 表值比对偏差 < 3% 才启用 |
| 4 | Zen5（1Ah）`CoreCOF` 公式 | Zen5 频率正确性 | ✅ 已关闭（`CpuFid × 5 MHz`、`CpuFid = bits[11:0]`：amd-facts.md 4；原文 `× 50 MHz` 已更正） |
| 5 | SMU 邮箱地址（17h/19h/1Ah）与 PM 表获取命令号 | PM 表能否拿到 | ✅ 已关闭（地址集合与三条命令号：amd-facts.md 5）；「家族 → 三元组」映射为**推断** → 探测方案：数据表存候选数组，运行时以只读探针 `getVersion (0x02)` 择优 |
| 6 | PawnIO.Modules 里 AMD 相关模块的文件名与 IOCTL | 驱动集成方式 | ✅ 已关闭（`AMDFamily17.p` / `RyzenSMU.p` 及全部导出 IOCTL：amd-facts.md 6） |
| 7 | 各家族/型号温度上限值 | `cpu.temp.headroom` 是否可用 | ❌ 未找到一手来源（家族级常数与可读 Tjmax 寄存器都不存在；已核 4 个 SKU 的官方值见 amd-facts.md 7）→ 探测方案：按 SKU 从 amd.com 规格页维护表，未匹配即显示不可用；可用「满负载 Tctl 峰值 + HWiNFO 的 Tjmax」对拍，仅记诊断，不自动填值 |
| 8 | 「读数翻倍」现象的范围与判据 | 功耗正确性（2× 误差风险） | ❌ 未找到一手来源（AMD 硬件 2× 无证据：「同一物理域两个 MSR」前提不成立，见 amd-facts.md 8）→ 探测方案：只在同一 MSR 上差分做比值、禁止跨域/跨线程求和、`ESU` 从 `0xC0010299 bits[12:8]` 动态取，与 PM 表 PPT 的长期比值仅作诊断信号（**不自动折半**） |

## 10. 风险与开放问题

| 风险 | 缓解 |
|---|---|
| PM 表按家族/型号维护成本高（每代新 CPU 都要新表） | 数据驱动表 + 诊断导出让用户提交一条型号记录；匹配失败自动落保守模式而不是算错值 |
| AGESA/BIOS 行为差异导致同一型号读数不同 | 不依赖单一来源：RAPL 与 PM 表交叉校验，出现矛盾时优先 RAPL 并记诊断 |
| VBS/HVCI 下个别 MSR 触发异常 | 逐寄存器 try/catch 容错，失败的寄存器记入诊断，不影响其它指标 |
| 笔记本 EC 屏蔽部分遥测 | 明确提示「本机（笔记本）可能不暴露该传感器」，不伪造 |
| 老平台（family 0F/10） | 明确不支持，只提供免驱降级，README 写清 |
