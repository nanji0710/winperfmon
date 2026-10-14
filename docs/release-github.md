# winperfmon — 发布与许可合规方案

> 与 `winperfmon-dev-plan.md`（下称**开发方案**）配套：技术路线以开发方案为准，本文只规定**发布形态、第三方合规动作、仓库与 CI 纪律**。许可证与用法均取自开发方案 §3/§4/§5/§7/§11/§13；不确定项一律标注**待核实**（汇总见附录 A），核实前**不得**据此搬运代码或分发文件。

## 1. 目标与前提

| 项 | 结论 |
|---|---|
| 产品名 / EXE / 仓库 | **winperfmon**（已定）：EXE `winperfmon.exe`、仓库 `winperfmon`；GitHub 重名初查见 §10 |
| 技术栈 | C++20 / MSVC / CMake，`/MT` 静态 CRT，**单 EXE**，**零 .NET / WebView2 / Python 运行时依赖**（开发方案 §5） |
| 发布方式 | 公开发布到 GitHub，**开源免费**（许可证见 §2） |
| 首版交付形态 | portable zip + 单 EXE + 合规文档；**不发布安装器，不发布任何内核驱动** |
| 首版版本号 | `v0.1.0` 并勾选 **pre-release**（硬件矩阵覆盖不足，见 §7.3） |

### 1.1 发布前提（README / Release Notes / 关于页三处必须一致）

- **帧率数据**：依赖用户机器上**已安装**的 PresentMon ≥ 2.3.1；我们只**动态加载**其 `PresentMonAPI2.dll`，**不随包分发**（开发方案 §3.4）。
- **CPU 温度 / 功耗、主板风扇**：依赖 PawnIO（驱动 **GPL-2.0**；其 `.bin` 模块由 PawnIO.Modules 提供，**LGPL-2.1**）；**官方签名安装包随发布包附带**，由程序引导用户确认后静默安装（可拒绝，拒绝即免驱模式）——GPL-2.0 义务见 §3.5（决策 ③=B）。
- 前提未满足时进入**免驱轻量模式**（PDH CPU 频率 + NVML/ADLX/IGCL GPU 指标），其余项显示 `N/A` 并给出明确原因 —— 这条降级路径必须可用，否则等于"装了不能用"。
- 首版无安装器，引导形式为 **README 链接 + 首启提示对话框中的官网链接**：不做静默安装、不做链式安装。
- **不注入、不挂钩**任何进程；不使用低级键盘钩子（热键走 `RegisterHotKey`，开发方案 §7）。

### 1.2 非目标（本期明确不做）

> **决策 ③=B 边界说明：** 本期仍**不自研** 一体化安装器（Inno 等）与**不做自己的驱动签名**；但会**随发布包附带 PawnIO 官方签名安装包**并只做「引导 + 调用其静默安装」（见 §3.5）。发布物形态因此由「单 EXE」调整为「便携 ZIP」：`winperfmon.exe` 仍 ≤5 MB、仍可免安装直接运行（不装 PawnIO 时落免驱模式）。

- 不做内核驱动签名：Win11 22H2 起新内核驱动必须 WHQL，个人/小团队走不通（开发方案 §4）；不做安装器、不做后台自动更新、不做账号体系、不做遥测。

## 2. 许可证选择

### 2.1 MIT vs Apache-2.0 取舍

| 维度 | MIT | Apache-2.0 | 对本项目的含义 |
|---|---|---|---|
| 篇幅 | 约 170 词 | 约 10 KB 全文 | MIT 更易被玩家用户与贡献者读懂 |
| 专利授权 | 无明文条款 | §3 明示专利授权 + 专利报复终止 | 本项目无专利资产；差异主要影响企业采用意愿 |
| 商标 | 无相关条款 | §6 明示不授予商标权 | 无论选哪个，README 都要写"不使用第三方商标、无隶属关系" |
| 修改标注义务 | 无 | §4(b) 要求标注被修改过的文件 | 我们要**大量搬运并改写**上游代码（nwinfo、CapFrameX），MIT 下只需保留原版权行 |
| NOTICE 义务 | 无 | 上游含 NOTICE 时必须随附 | 我们本来就要做 `NOTICE`（§4），差异可忽略 |
| 与上游许可共存 | 可与 MIT / Unlicense / MPL-2.0 文件共存 | 同左；但 Apache-2.0 与 GPL-2.0 **不兼容**（与 GPL-3.0 兼容） | 本项目对 GPL／LGPL 只做运行期调用与聚合分发（§3.4 决策 ③=B 修订版），两种选择都不冲突 |

**结论：本项目自身代码采用 MIT。** 理由：单 EXE 桌面工具、无专利资产、目标是最大化采用与搬运便利；上游主力（nwinfo/Unlicense，CapFrameX / PresentMon / MangoHud/MIT）都是"宽松 + 保留声明"模式，与 MIT 衔接最顺。**注意**：选 MIT 不会让 GPL-2.0 依赖变得可分发 —— 分发 GPL 二进制仍受 GPL 约束，与本项目许可证无关。

### 2.2 LICENSE 文件怎么写

- 位置：仓库根 `LICENSE`（无扩展名），纯文本，内容为 SPDX 官方 MIT 正文，**一字不改、不追加条款**；首行固定 `MIT License`，其后为版权行，不要添加 "All rights reserved" 之类冗余语句。
- 年份用首次发布年，版权持有人填**个人姓名或组织名（待定）**；**发布前必须替换占位符**，禁止带尖括号占位符上线。
- 不做 `LICENSE-MIT` + `LICENSE-APACHE` 双许可（Rust 生态习惯），C++ 单许可证即可。
- 源文件头部：我方文件用 `// SPDX-License-Identifier: MIT` + 版权行；MPL 派生文件用 `MPL-2.0` 并注明派生自 LibreHardwareMonitor 的具体文件与上游版权行。

版权行格式：

```text
MIT License
Copyright (c) <YEAR> <COPYRIGHT HOLDER>
<SPDX 官方 MIT 正文，原样复制，不得改动>
```

## 3. 第三方依赖与合规动作表

### 3.1 代码借用 / 移植类

| 名称 | 许可证 | 我们如何使用 | 必须做的动作 |
|---|---|---|---|
| [nwinfo](https://github.com/a1ive/nwinfo) | **Unlicense**（公共领域） | MSR 寄存器表与位域、Super I/O 探测流程、PawnIO 调用范式与驱动回退链，作为 C→C++ 骨架（开发方案 §5） | 无法律义务；仍在 NOTICE/关于页致谢并记录上游版本或提交号（便于追溯） |
| [CapFrameX](https://github.com/CXWorld/CapFrameX) | **MIT** | 移植帧时间统计口径：1%/0.1% Low、Afterburner 等价 integral、stutter、帧节奏直方图、离群剔除（开发方案 §3.4） | 保留上游版权声明与 MIT 文本；派生文件头写明来源（`source/CapFrameX.Statistics.NetStandard/FrametimeStatisticProvider.cs`）；NOTICE 列条目；**不得**用其名称命名我方产品或暗示背书 |
| [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) | **MPL-2.0** | Super I/O（Nuvoton/ITE）寄存器常数与芯片特判表，**文件级隔离**移植（开发方案 §3.3、§13.3） | 派生文件独立目录（`src/sensors/superio/lhm/`）、保留 MPL-2.0 文件头与上游版权行、**不得改授为 MIT**；仓库内提供这些文件即满足"源码可获取"；修改过的文件需说明修改；升级上游时记录 diff |
| [MangoHud](https://github.com/flightlessmango/MangoHud) | **MIT** | 仅借 HUD 元素化布局与表驱动配置键设计（其实现是 Vulkan layer 注入，不可移植，开发方案 §3.4） | 未复制代码即无义务；若借用配置键命名，在文档/关于页注明灵感来源 |
| [siomon](https://github.com/level1techs/siomon) | **MIT** | NCT67xx / ITE87xx 常数交叉校验（开发方案 §13.1） | 若最终未搬运代码则无需动作；一旦搬运按 MIT 保留声明 |
| [RyzenAdj](https://github.com/FlyGoat/RyzenAdj) | **LGPL-3.0** | **只搬常数**（PM 表字段偏移），不搬代码（开发方案 §13.3） | 常数本身不受版权保护，仍标注来源；**禁止**复制其函数实现或文件结构 |
| [ZenStates-Core](https://github.com/irusanov/ZenStates-Core) | **GPL-3.0** | 只看思路（开发方案 §13.3） | 只看不抄；若必须复现某算法，改为自行推导并注释参考来源 |
| [Special K](https://github.com/SpecialKO/SpecialK) | **GPL-3.0** | 只借 "MPO plane 检测" 思路（开发方案 §13.4） | **绝不并入**任何代码，仅可复现思路 |
| [VirtualSMC / SMCSuperIO](https://github.com/acidanthera/VirtualSMC) | **BSD-3-Clause**（已核实：GitHub REST API `repos/acidanthera/VirtualSMC` → `license.spdx_id = BSD-3-Clause`） | 只作 Super I/O 常数表的只读交叉校验 | 沿用"只读参考"策略；若最终搬运常数，必须保留版权行与免责声明（BSD-3 的免责条款不得删改） |
| [nvml-wrapper](https://github.com/rust-nvml/nvml-wrapper) | **Apache-2.0**（已核实：GitHub REST API `repos/rust-nvml/nvml-wrapper` → `license.spdx_id = Apache-2.0`，**不是 MIT**） | 参考其 `libloading` 动态加载 `nvml.dll` 的做法 | 只借思路即无义务；若复制代码需保留其 LICENSE 与 NOTICE、并标注修改过的文件（Apache-2.0 §4）；注意 Apache-2.0 与 GPL-2.0 **不兼容**，其代码不得与任何 GPL 代码放进同一文件 |

### 3.2 运行期依赖类（**PawnIO 为例外：随包分发，见 §3.5**；其余不随包分发）

> **决策 ③=B 补充（模块许可）：** PawnIO 的**驱动／官方安装包**为 **GPL-2.0**；其 **`.bin` 模块来自 [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules)，许可为 LGPL-2.1**（GitHub REST API `license.spdx_id = LGPL-2.1`；仓库 `COPYING` 记 LGPL-2.1-or-later）。**若本程序必须随包提供模块二进制**（待核实，附录 A-11），则触发 LGPL-2.1 §6 义务：随附模块源码或有效要约（≥3 年）、保留版权与许可声明、**不得改授**、允许用户替换模块文件；模块二进制同样**不进仓库**，构建期下载并 SHA-256 锁定。

> **决策 ③=B 更新：** PawnIO 由「仅运行期依赖」变为**随发布包分发并引导静默安装**，必须按 §3.5 履行 GPL-2.0（驱动）与 LGPL-2.1（模块，若随包提供）义务；PresentMon SDK/DLL、RTSS、厂商 GPU SDK（NVML / ADLX / IGCL）仍**不**随包分发，只动态加载用户机上已有的实现。

| 名称 | 许可证 | 我们如何使用 | 必须做的动作 |
|---|---|---|---|
| [PresentMon](https://github.com/GameTechDev/PresentMon)（`PresentMonAPI2.dll` + `PresentMonSharedService`） | **MIT** | 动态加载用户机器上已安装的 SDK DLL，注册帧查询并轮询消费；**官方不保证自带 DLL 与服务二进制兼容**（开发方案 §3.4） | 不随包分发 DLL/头文件；README 与 Release Notes 写明最低版本（≥ 2.3.1）与安装指引；关于页列 Intel 版权与 MIT 链接；服务重启后自动重开会话（`PM_STATUS_SESSION_NOT_OPEN`） |
| [PawnIO](https://github.com/namazso/PawnIO) + [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) | **GPL-2.0**（驱动）／**LGPL-2.1**（PawnIO.Modules 的 `.bin` 模块） | 运行期打开设备 `\\.\PawnIO`，对**用户已安装**的模块（IntelMSR / AMDFamily* / RyzenSMU / LpcIO）发起 IOCTL（开发方案 §4） | **决策 ③=B 修订：** 随发布包附带**官方签名安装包原件**（不修改、不重签名，并按 §3.5 履行 GPL-2.0 义务）；**仍不得**把驱动 `.bin`/`.sys` 单独拆出分发或自行编译分发；首次运行检测并引导安装（可拒绝 → 免驱模式）；README 明示 PawnIO 是 GPL-2.0 第三方软件、与本项目无隶属关系（安装包随包附带，见 §3.5）；保留"禁用 Ring0 驱动"开关（同 LHM PR #1777 做法） |
| RTSS 共享内存 OSD（`RTSSSharedMemoryV2`） | 结构公开；**使用条款待核实** | **自行定义**结构并读写共享内存：写 `szOSD` + 递增 `dwOSDFrame`，按 `dwSignature/dwVersion/dwAppEntrySize/dwOSDEntrySize/dwOSDArrOffset` 协商；**不注入**（开发方案 §7） | 不复制 RTSS 源码或头文件，不随包分发 RTSS 任何文件；README 说明 RTSS 为用户自装第三方软件；结构不匹配即放弃写入并提示 |
| NVIDIA NVML（`nvml.dll`）/ AMD ADLX / Intel IGCL、Level Zero | 厂商专有 SDK，**再分发条款待核实** | 运行期动态加载**驱动自带**的 DLL，仅调用查询类接口（开发方案 §3.2） | **不**把厂商 DLL / 导入库 / 头文件副本放进仓库或 Release；不声称与厂商存在隶属或背书关系；NOTICE 中列为"运行期可选依赖" |

### 3.3 构建期与工具链类

- **`wil`**（MIT，**以随附许可证原文为准**）：Win32 RAII/资源封装（开发方案 §5）→ 以子模块或源码引入时在 `third_party/` 保留 LICENSE 原文。
- ~~`nlohmann/json` / `simdjson`~~：**已否决，不再引入**（决策 ⑧：配置读写用自研极简 JSON 子集，约 300 行 + 单测，保持零第三方依赖；PresentMon 的数据源是逐帧 CSV，本来也不需要 JSON 库）。若将来确有引入需要，必须同步更新本节、`NOTICE`、`config-schema.md` §0 不变量 6 与 `architecture.md` §12。
- **MSVC / Windows SDK / CMake**：仅构建期使用、不随包分发；`/MT` 静态 CRT 产物的再分发条款以 Visual Studio 许可为准（**待核实**）；**CI 第三方 action**（如 `msvc-dev-cmd`、`action-gh-release`，各自许可证**待核实**）：版本固定到 commit SHA（不用浮动 `@v1`），降低供应链风险。

### 3.4 三条最容易踩的合规红线

> **决策 ③=B 补充（第四条红线）：** 随包分发 PawnIO 后，**必须**同时提供其对应源码（或有效书面要约）与许可证原文，且安装前必须获得用户明确确认 —— 「只给二进制、不给源码」是最典型的 GPL 违约形态；若同时随包提供 PawnIO.Modules 的 `.bin` 模块，还须按 LGPL-2.1 提供模块源码或有效要约、保留声明并允许用户替换。

1. **GPL／LGPL 二进制（决策 ③=B 修订版）**：仓库中仍**不得**存放 PawnIO 与 PawnIO.Modules 的 `.bin`/`.sys`/上游源码副本；但 Release **可以**附带其**官方安装包原件** —— 此时我们的分发行为落入 GPL-2.0 义务范围，必须完整执行 §3.5（随附许可证原文 + 源码获取说明 + 用户确认安装），且"聚合分发不传染 MIT"只是**工程判断，不是法律意见**，发布前必须做法务确认。
2. **MPL-2.0 文件不得改授**：从 LHM 派生的文件必须保留 `MPL-2.0` 文件头，不能因为"整仓是 MIT"就改成 MIT —— 最常见的许可事故。
3. **不复制厂商 SDK**：`nvml.dll` / ADLX / IGCL / `PresentMonAPI2.dll` 一律**动态加载用户机器上的版本**；把 DLL 或头文件放进仓库会造成再分发问题。

### 3.5 随包分发 GPL-2.0／LGPL-2.1 二进制（决策 ③=B：PawnIO 驱动 + PawnIO.Modules 模块）

**分发什么**：便携 ZIP 与 Release 附件内含 PawnIO 官方签名安装包原件（`pawnio-<ver>-installer.exe`，构建期从官方 Release 下载、登记 SHA-256、**不修改不重签名**），以及：

| 物料 | 作用 |
|---|---|
| `third_party/pawnio/LICENSE-GPL-2.0.txt` | PawnIO **驱动**的 GPL-2.0 许可证原文（随原件） |
| `third_party/pawnio/LICENSE-LGPL-2.1.txt` | PawnIO.Modules **模块**的 LGPL-2.1 许可证原文（随模块原件；若上游安装包已自带模块、我们不单独分发，则仅需在 NOTICE 中声明） |
| `third_party/pawnio/PAWNIO-SOURCE-OFFER.txt` | 精确版本号 ↔ 官方源码仓库 tag/源码包 URL 的对应关系与校验和（用于履行 GPLv2 §3 与 LGPL-2.1 §6「提供对应源码」义务） |
| `THIRD-PARTY-NOTICES.txt` | 写明「本程序自身代码为 MIT；随包附带的 PawnIO 是 GPL-2.0 独立程序（mere aggregation，聚合分发），其许可与源码以上表为准」 |
| 安装界面 /「关于」页 | 安装前说明将安装第三方**内核驱动**、用途（CPU 温度 / 功耗 / 风扇）与可拒绝；「关于」页展示同一份第三方清单 |

**必须做（缺一不可）**：
1. 随发布包提供 PawnIO **对应版本**的源码获取方式（同包附源码归档，或书面要约 + 有效可下载 URL；GPLv2 要求要约至少 3 年有效）。
2. 保留其许可证与版权声明，**不得**把 GPL 文件改授为 MIT。
3. 不修改其二进制；若必须修改（例如改安装参数文件），修改版仍受 GPL-2.0 约束，需公布修改内容与源码。
4. 若随包提供 PawnIO.Modules 的 `.bin` 模块：同法提供模块源码获取方式（LGPL-2.1 §6）、保留声明、**不得改授**，并允许用户替换模块文件（LGPL-2.1 允许为调试而反向工程）。
5. 安装必须经用户**明确确认**（一次 UAC）；拒绝时程序仍可用（免驱模式），**不得静默强装**。
6. 卸载 winperfmon 时默认**不移除** PawnIO（其他工具可能依赖），只提供说明与手动卸载指引。

**工程判断（非法务意见）**：本程序只在运行期经 IOCTL 使用 PawnIO 与模块，不链接、不复制其代码；两个独立程序放进同一发布包属于 GPL FAQ 所述 *mere aggregation*，故我方代码可继续 MIT；LGPL-2.1 对「使用库」的运行期调用不产生衍生作品义务，义务仅系于我们**分发模块二进制**这一行为本身 —— **但首次 Release 前应就「聚合分发 + 引导安装 + 是否随包提供模块」这一具体形态做法务确认**（附录 A-4、A-11）。

**待核实**：① PawnIO 安装器的静默参数（按其安装器类型确认，例如 `/S`）；② PawnIO.Modules 的 `.bin` 模块是否必须由我们随包分发（若分发，除本节 GPL-2.0 动作外还须履行 LGPL-2.1 义务：模块源码或有效要约 + 保留声明 + 允许用户替换）→ 见附录 A-9 / A-11。

## 4. NOTICE 与「关于」页

### 4.1 仓库 NOTICE 骨架（可直接落地，占位符用 `<>` 标出）

```text
winperfmon
Copyright (c) <YEAR> <COPYRIGHT HOLDER>
本产品以 MIT 许可证发布，详见 LICENSE。
一、随本产品分发的第三方作品
1) nwinfo — Unlicense（公共领域）— https://github.com/a1ive/nwinfo
   使用方式：MSR 寄存器表与 Super I/O 探测流程移植为 C++（上游版本/提交：<PIN>）
2) CapFrameX — MIT — https://github.com/CXWorld/CapFrameX
   使用方式：帧时间统计口径移植（上游 source/CapFrameX.Statistics.NetStandard/FrametimeStatisticProvider.cs，版本：<PIN>）
3) LibreHardwareMonitor — MPL-2.0 — https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
   使用方式：Super I/O 常数表，文件级隔离于 src/sensors/superio/lhm/（上游 IT87XX.cs / Nct677X.cs / LpcIO.cs，版本：<PIN>）
4) <其余随包分发的库：wil / JSON 库等，逐项列出名称 + 许可证 + 源码链接>
二、不随本产品分发、需用户自行安装的运行期组件
- Intel PresentMon ≥ 2.3.1（MIT）— 本产品仅动态加载其 PresentMonAPI2.dll
- PawnIO（**GPL-2.0**，驱动／官方安装包原件）— 随包附带**官方签名安装包原件**（决策 ③=B；许可证原文与源码获取说明见 §3.5，随包置于 `third_party/pawnio/`）；运行期通过设备 \\.\PawnIO 发起 IOCTL；本项目自身代码为 MIT，不含 GPL 代码
- PawnIO.Modules（**LGPL-2.1**）— 若随包提供 `.bin` 模块，随附其许可证原文与源码获取说明（§3.5），模块可被用户替换
- RivaTuner Statistics Server（用户自装）— 仅向其共享内存写入文本，不注入任何进程
- NVIDIA NVML / AMD ADLX / Intel IGCL 与 Level Zero — 随厂商驱动提供
三、商标声明：本产品与上述第三方产品的名称、商标无隶属或背书关系；相关商标归各自所有者。

四、本产品自身代码不含任何 GPL / AGPL / LGPL 许可的代码；随发布包附带第三方程序 PawnIO（GPL-2.0，官方安装包原件）及其可选模块 PawnIO.Modules（LGPL-2.1），二者均为独立程序、按各自许可分发，许可证原文与源码获取方式见 `third_party/pawnio/`。
```

### 4.2 「关于」页要求

- 展示与 `NOTICE` **同源**的第三方列表；建议把 NOTICE 编成资源字符串，并在 CI 中校验 README / NOTICE / 关于页三者条目一致（避免三处漂移）。
- 提供许可证全文入口（指向仓库 `LICENSE` 固定链接）、**指标口径说明**（1% Low 算法、单位、异常值判定）、**降级原因说明**（为何某项显示 `N/A`）—— 这是开发方案 §9"口径公开"的落地要求。
- 显示 `vX.Y.Z` + 构建提交短哈希 + 构建日期，便于 Issue 定位。

## 5. 仓库结构建议

### 5.1 目录树

```text
winperfmon/                        # 仓库根（名称已定：winperfmon）
├─ README.md / README.en.md        # 英文版是否需要：待定（§10）
├─ LICENSE · NOTICE · CHANGELOG.md · SECURITY.md · CONTRIBUTING.md · .gitignore
├─ CMakeLists.txt · cmake/         # 工具链与依赖查找（FindNVML.cmake 等）
├─ docs/                           # release-github.md(本文件) · metrics.md(指标口径/降级规则)
│                                  # hardware-support.md(CPU/GPU/Super I/O 与主板特判矩阵)
│                                  # privacy.md(隐私与安全声明) · contributing-sensors.md(芯片表贡献流程)
├─ include/winperfmon/             # 公共头（provider 接口）
├─ src/
│  ├─ app/ (入口/单实例/CLI) · ui/ (Win32+Direct2D/托盘/Overlay)
│  ├─ sensors/cpu/ · sensors/gpu/ · sensors/fps/
│  ├─ sensors/superio/lhm/         # MPL-2.0 隔离目录（保留 MPL 文件头）
│  ├─ ring0/                       # PawnIO 运行期调用封装（不含任何 GPL 二进制）
│  └─ core/ (调度器/环形缓冲/统计/无锁快照) · io/ (配置/CSV/日志)
├─ tests/                          # ctest：统计口径、RAPL 回绕、异常值、配置迁移、CSV 解析
├─ third_party/                    # 仅宽松许可依赖源码 + 各自 LICENSE 原文（wil、JSON 库）
├─ tools/                          # 探针与校验脚本（如伪造共享内存做 CI）
└─ .github/workflows/build.yml · ISSUE_TEMPLATE/ · PULL_REQUEST_TEMPLATE.md
```

### 5.2 绝不能进仓库 / Release 的内容

| 禁止项 | 原因 |
|---|---|
| PawnIO / PawnIO.Modules 的 `.bin`、`.sys`、源码副本（**官方安装包原件除外**，按 §3.5 分发） | GPL-2.0／LGPL-2.1 分发义务（§3.4 红线 1 修订版） |
| 厂商 SDK 副本：`nvml.dll`、ADLX、IGCL、`PresentMonAPI2.dll` 及厂商头文件 | 再分发条款不明或禁止（§3.4 红线 3） |
| RTSS 的任何文件或头文件副本 | 第三方闭源软件，非本项目可分发物 |
| 构建产物：`build/`、`*.exe`、`*.pdb`、`*.obj` | 仓库卫生；Release 附件另行上传 |
| 用户配置与日志：`*.json` 配置、CSV 日志、`*.dmp` 崩溃转储 | 可能含机器名、用户名、主板序列号、GPU UUID 等可识别信息 |
| 密钥与证书：`*.pfx`、`*.snk`、CI 令牌；IDE 用户文件 `.vs/`、`*.user` | 安全事故 / 个人环境噪声 |

**`.gitignore` 必须覆盖**：`build/`、`out/`、`*.exe`、`*.dll`、`*.pdb`、`*.ilk`、`*.obj`、`*.dmp`、`.vs/`、`CMakeUserPresets.json`、`winperfmon.json`（用户配置）、`logs/`、`*.csv`、`*.pfx`。

## 6. 构建与 CI

### 6.1 GitHub Actions 要点（windows-latest + MSVC + CMake）

- **触发与环境**：`push` 到 `main`、`pull_request`、tag `v*`（tag 额外触发发布 job）；`windows-latest` 自带 MSVC + CMake + Ninja，无需安装额外依赖 —— 正是"零运行时依赖"的红利。
- **矩阵**：`Release`（`/O2`）为必跑并跑 `ctest`；`Debug` 只构建 + `ctest` 以捕获断言；不做多架构矩阵（x64 单一目标，ARM64 见 §10）。
- **步骤**：configure → build → ctest → 打包 zip（EXE + LICENSE + NOTICE + README）→ 上传 artifact。
- **体积守卫**：CI 检查 EXE ≤ 5 MB，超出即 fail，防止依赖膨胀破坏核心卖点（开发方案 §1）。
- **发布 job**：仅 tag `v*` 时创建 Release，附 zip、裸 EXE、`SHA256SUMS.txt`，Release Notes 套用 §7.4 模板并按版本号后缀自动判定 pre-release。
- **单测范围**：1% Low / 0.1% Low / 调和平均 / stutter 口径、RAPL 32 位回绕、异常值校验、配置默认值与迁移、PresentMon CSV 解析（用录制样本作 fixture，样本不得含可识别信息）。
- **供应链**：第三方 action 固定到 commit SHA；`permissions` 最小化（仅发布 job 需 `contents: write`）。

### 6.2 为什么首版不做驱动签名、不发布安装器

- **不做驱动签名**：Win11 22H2 起新内核驱动必须 WHQL（开发方案 §4），个人/小团队不可行；且本项目本来就**不分发驱动**（PawnIO 由用户自装），没有可签名的产物。
- **不发布安装器**：安装器需要 EV 代码签名证书；portable zip + 单 EXE 无注册表写入、无卸载残留、无运行时依赖，本身就是差异化卖点（开发方案 §1、§10）。
- **代价与话术**：未签名 EXE 首次运行会触发 SmartScreen → README 与 Release Notes 给出"更多信息 → 仍要运行"的说明，并公开源码与可复现的 CI 构建，用透明度替代签名信任。

## 7. 版本与发布物策略

### 7.1 版本与分支

- 语义化版本 `MAJOR.MINOR.PATCH`；"公开接口"指**配置结构**与 **CLI 参数**，`0.x` 阶段的破坏性变更升 `MINOR` 并在 CHANGELOG 标 `BREAKING`；`main` 保持可构建可发布，功能走 `feat/*`，发布以 tag `vX.Y.Z` 为准，预发布用 `v0.1.0-rc1` 并勾选 pre-release。

### 7.2 CHANGELOG 约定

采用 Keep a Changelog 风格：`## [0.1.0] - YYYY-MM-DD` 下分 `### Added` / `### Changed` / `### Fixed` / `### Removed`；**额外固定一节 `### 传感器支持变化`**（新增 Super I/O 芯片 / 主板特判 / CPU 家族），便于用户判断自己机器是否受益 —— 这是本项目最大的维护面（开发方案 §3.3）。

### 7.3 Release 附件清单

> **决策 ③=B 更新（新增附件）：** `pawnio-<ver>-installer.exe`（官方签名原件）、`PawnIO-LICENSE-GPL-2.0.txt`、`PAWNIO-SOURCE-OFFER.txt`（精确版本 ↔ 源码 URL/校验和）、`THIRD-PARTY-NOTICES.txt`；若随包提供模块，另加 `LICENSE-LGPL-2.1.txt` 与模块源码获取说明；如官方 Release 已附源码包，直接给出其下载链接与校验和即可。所有附件仍与 `SHA256SUMS.txt` 一起发布。

| 附件 | 说明 |
|---|---|
| `winperfmon-<ver>-x64.zip` | EXE + LICENSE + NOTICE + README（+ `docs/metrics.md`） |
| `winperfmon.exe` | 裸 EXE，便于老用户直接替换 |
| `SHA256SUMS.txt` | 所有附件的 SHA-256 |
| 源码 zip / tar.gz | GitHub 自动生成 |
| Release Notes | 按 §7.4 模板，含合规声明与校验值 |

首版标记 **pre-release**：Super I/O 与主板特判表只覆盖少数型号，硬件矩阵不足，需要真实用户反馈（开发方案 §9 风险表）。

### 7.4 Release Notes 模板（合规声明为必填段）

```markdown
## winperfmon <ver>（预发布）
### 亮点
- 新增 / 变更 / 修复（对应 CHANGELOG 条目）
### 兼容与依赖
- 系统：Windows 10 1809+ / Windows 11（x64）
- 帧率统计需用户自行安装 PresentMon ≥ 2.3.1：本工具仅动态加载其 SDK DLL，不随包分发
- CPU 温度/功耗与主板风扇需 PawnIO（驱动 GPL-2.0；模块 PawnIO.Modules 为 LGPL-2.1）：官方安装包随发布包附带，由用户在首次运行时确认后安装（可拒绝），并可在设置中完全禁用
- 未安装上述组件时自动进入免驱轻量模式，相关指标显示 N/A
### 安全与隐私

> **决策 ③=B / ④=B 补充（Release Notes 必写）：** ①本版随包附带 **PawnIO**（驱动 GPL-2.0）官方安装包，由程序**引导**用户确认后静默安装，可拒绝，拒绝时 CPU 温度/功耗/风扇显示 N/A 并说明原因；②真独占全屏（FSE）下的 Overlay 采用 **RTSS OSD 共享内存后端**，**本程序不注入任何进程**，注入由用户自行安装的 RTSS 完成；③本程序不联网、不采集、不上传。
- 本工具不注入、不挂钩任何进程，不联网、不上传任何数据（可选"检查更新"默认关闭）
- Ring0 访问为可选：用户自行安装的第三方驱动 + 运行期 IOCTL，可一键禁用
### 校验
- SHA256：见 SHA256SUMS.txt
```

## 8. 首次发布前检查清单

- [ ] `LICENSE` 位于仓库根、MIT 正文未被修改、年份与版权持有人已填实值（无占位符）
- [ ] `NOTICE` / `THIRD-PARTY-NOTICES.md` 与实际依赖逐条对齐：无遗漏、无已移除依赖，每项含许可证与源码链接
- [ ] 每个派生文件带正确 SPDX 头（MPL 派生文件仍为 `MPL-2.0`，未被改写为 MIT）
- [ ] 仓库内无任何 GPL / AGPL / LGPL 代码或二进制（人工 `git grep` + 全仓体积扫描）；Release 附带的 PawnIO 安装包／模块属聚合分发，按 §3.5 单独校验（许可证原文 + 源码获取说明 + 模块可替换）
- [ ] 无厂商 SDK 再分发副本（`nvml.dll`、ADLX、IGCL、`PresentMonAPI2.dll` 等）
- [ ] README 写明依赖前提、降级路径、SmartScreen 提示、最低系统版本
- [ ] `README.en.md` 是否提供已决定（待定，见 §10）
- [ ] `docs/metrics.md` 写明 1% Low / 平均 FPS / stutter 口径与异常值规则；`docs/privacy.md` 写明不联网、不采集、不注入、Ring0 可选
- [ ] `SECURITY.md` 提供漏洞报告渠道；`CONTRIBUTING.md` 含芯片表提交日志格式；Issue 模板要求附 `--probe` 输出并提示删除序列号等可识别信息；PR 模板含"代码来源与许可证"声明项
- [ ] `.gitignore` 覆盖构建产物、用户配置、日志、转储（§5.2）
- [ ] 仓库无密钥 / 证书 / CI 令牌；历史提交无敏感信息泄露（必要时 filter-repo 重写历史）
- [ ] Actions 在 `main` 与最新 tag 全绿，体积守卫生效
- [ ] Release Notes 使用 §7.4 模板且合规段未删
- [x] 仓库名 / 产品名 / EXE 名已定：**winperfmon**（EXE `winperfmon.exe`、仓库 `winperfmon`）；GitHub 重名初查已完成（同名仓库 2 个、均无许可证且不活跃，见 §10）
- [ ] 无遥测复核：全仓无网络调用（`git grep -i "winhttp\|wininet\|socket\|curl"` 为零，或仅存在于默认关闭且可编译期移除的检查更新模块）

## 9. 隐私与安全声明要点

- **不采集、不上传、不联网**：无遥测、无账号、无云端；可选"检查更新"默认**关闭**，仅在用户主动点击后发起一次 HTTPS 请求，建议提供编译开关使"从不联网"版本在构建期完全剔除网络代码。
- **不注入、不挂钩**：不向任何进程加载 DLL、不装 API 钩子、不用低级键盘钩子（热键用 `RegisterHotKey`）；RTSS OSD 后端**只写共享内存文本**，注入由 RTSS 自身完成，我们既不承担也不掩盖该事实。
- **Ring0 可选且可禁用**：PawnIO（驱动 GPL-2.0）官方安装包随发布包附带，由用户确认后安装，本项目只在用户态发起 IOCTL；拒绝安装或关闭开关后进入免驱模式（CPU 温度/功耗/主板风扇为 `N/A`）。
- **数据仅本地**：日志与 CSV 仅在用户开启时写本地；导出内容不含机器名 / 用户名 / 序列号 / GPU UUID（或提供匿名化开关）。
- **反作弊场景**：检测到受保护进程时不触碰其进程，自动退到 GPU-only + PresentMon FPS（开发方案 §9）；README 明示"不保证在所有反作弊环境可用"，避免预期落差。
- **免责**：读数为参考值，不用于质保申诉或超频安全决策。

## 10. 风险与开放问题

| 项 | 现状 / 待决 |
|---|---|
| 仓库名 / 产品名 / EXE 名 | ✅ **已定：winperfmon**（EXE `winperfmon.exe`、仓库 `winperfmon`）。GitHub 重名初查结果：`jusu1234/WinPerfMon`（2017，C#，0★，无许可证）、`AronZhi/WinPerfMonDll`（2022，C++，0★，无许可证）——均非活跃同类产品，我方以全名「用户名/winperfmon」区分；**商标检索仍建议发布前补做**（附录 A-1） |
| 是否需要英文 README | 待定：英文可获国际用户与更多 Super I/O 反馈，但有双份文档同步成本 |
| 是否接受社区提交 Super I/O 芯片表 | **已定义流程，见 `contributing-sensors.md`**：`--probe` 输出格式固定为 `probeVersion=1`（chip ID、候选型号、主板 DMI 字符串、端口探测结果、每通道原始计数与寄存器出处），要求贡献者确认已匿名化并提供至少一项对拍值（BIOS/HWiNFO）；维护者人工核对后并入芯片表，**无 SLA、不承诺"下个版本一定支持"** |
| PresentMon 未安装时的提示措辞 | README / UI / Release Notes 三处须一致；**不提供内置一键下载**（会引入联网行为，与"不联网"声明冲突），只给网页链接 |
| 首版 pre-release 转正标准 | 待定：建议以"覆盖 N 个真实用户机器报告、无高危崩溃"为门槛 |
| 是否开启 Discussions / Sponsors | 待定，取决于维护精力 |
| 法务边界 | 本文的合规判断为工程判断，非法律意见；若要捆绑 GPL 二进制或商业化，必须先做专业法律审查；已核实项（无需再查）：PresentMon 服务/客户端免提权路径、WinRing0 被 Defender 拦截、Win11 22H2 起新驱动需 WHQL（开发方案 §3.4、§4） |

### 10.1 决策 ③=B（随包分发 PawnIO）与 ④=B（RTSS 为主路径）带来的新增问题

| 问题 | 现状 | 影响 |
|---|---|---|
| 随包分发 GPL-2.0／LGPL-2.1 二进制的法务边界 | 工程判断为 mere aggregation（模块 LGPL-2.1 义务系于分发行为本身），**未做法务确认** | 首次 Release 前必须确认（附录 A-4、A-11） |
| PawnIO 安装器静默参数 | 待核实（附录 A-9） | 决定 M4 引导安装流程能否全自动 |
| `.bin` 模块是否需我们分发 | 待核实（附录 A-11） | 若需分发，除 GPL-2.0 动作外还须履行 LGPL-2.1（模块源码/要约 + 声明 + 允许替换） |
| RTSS 成为硬需求的依赖项 | 用户未装 RTSS 时真 FSE 下无 Overlay（自动回退自绘） | 安装提示与降级文案纳入 M3/M4 验收 |
| RTSS 注入与反作弊 | 注入由 RTSS 完成，非本程序 | README 与 Release Notes 中「不注入」的表述必须限定为**「本程序不注入」** |

## 附录 A：本文标注「待核实」的条目（共 12 项）

1. 商标冲突初查（产品名 / EXE 已定为 `winperfmon`；GitHub 重名已查，仅剩商标检索，建议首次发布前补做）。
2. 计划引入的 `wil` 的许可证与版本（以 `third_party/wil/LICENSE` 原文为准）。
3. MSVC `/MT` 静态 CRT 产物的再分发条款（以 Visual Studio 许可协议为准）。
4. `PawnIO` 驱动的 GPL-2.0 完整文本，以及"运行期 IOCTL 不构成衍生作品"的法务确认。
5. `PawnIO.Modules` 的 LGPL-2.1 完整文本，以及随包分发模块二进制时的义务确认（模块源码或有效要约 + 保留声明 + 允许替换）。
6. `PresentMon` 的 MIT 许可证文本与 Intel 版权行原文（以该仓库 `LICENSE` 为准，写入 NOTICE）。
7. RTSS 共享内存结构的使用条款（我方只自行定义结构、不复制其头文件）。
8. CI 所用第三方 GitHub Actions（`msvc-dev-cmd`、`action-gh-release` 等）的许可证与 SHA 锁定目标。
9. 若将来分发安装器：EV 代码签名证书与 SmartScreen 现状（**待定**，本期不做）。
10. PawnIO 官方安装包的**静默安装参数**（按其安装器类型核实；核实后写入构建脚本与本文档 §3.5）。
11. PawnIO.Modules 的 `.bin` 模块是否必须由我们随包分发（若必须，除 §3.5 的 GPL-2.0 动作外，还须履行 LGPL-2.1 义务并附模块源码）。
12. RTSS OSD 文本标签（颜色 / 字号 / 透明度 / 粗体）在主流 RTSS 版本中的支持矩阵（决策 ④=B 的字号颜色映射依赖它）。

> **已核实并回填（截至本次文档整理）：** `rust-nvml/nvml-wrapper` = **Apache-2.0**、`acidanthera/VirtualSMC` = **BSD-3-Clause**（来源：GitHub REST API 的 `license.spdx_id`，见 §3.1 表），故从本清单移除。
> 复核方式：以上条目在首次 Release 前逐项给出结论并回填本文档；未闭环的条目不得进入 Release Notes 的"已支持"表述。
