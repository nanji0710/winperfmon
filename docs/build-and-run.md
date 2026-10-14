# M0 骨架：免驱通道跑通（构建 / 运行 / 验收）

本文件记录 M0 阶段的**可复现构建与验收步骤**，供本机复核与 CI 复用。
设计依据：`docs/winperfmon-dev-plan.md` §8 里程碑 M0、§5 技术栈选型。

## 1. 工具链

M0 使用**便携工具链**（免管理员，不依赖 Visual Studio）：

| 组件 | 版本 | 路径（默认） |
|---|---|---|
| MinGW-w64 GCC | 16.2.0（UCRT / posix / SEH，WinLibs） | `<WPM_TC>\mingw\mingw64\bin` |
| CMake | 4.4.x | `<WPM_TC>\cmake\bin` |
| Ninja | 1.13.x | `<WPM_TC>\ninja` |

`WPM_TC` 默认是 `D:\GitHub\winperfmon-toolchain`，可用环境变量覆盖。

> 设计目标是 MSVC + `/MT` 静态 CRT（`docs/winperfmon-dev-plan.md` §5）。
> M0 期间本机没有 MSVC（装 VS Build Tools 需要管理员），因此先用 GCC 跑通链路；
> `CMakeLists.txt` 里 MSVC 分支（`/W4 /permissive- /utf-8 /EHsc /GS`）已经就位，
> 换到 MSVC 时无需改源码。

## 2. 构建

```cmd
call tools\env.cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物：

| 文件 | 说明 |
|---|---|
| `build\winperfmon.exe` | 主程序（M0 是控制台探针） |
| `build\wpm_tests.exe` | 单元测试（零第三方框架） |

体积守卫（`docs/release-github.md` §6）要求 EXE ≤ 5 MB；当前约 1.35 MB。

## 3. 运行

```cmd
build\winperfmon.exe --once       :: 采集一次，打印全部 50 项指标目录
build\winperfmon.exe --all        :: 含默认隐藏项（M0 的 --once 已等价于此）
build\winperfmon.exe --watch 10   :: 连续采样 10 秒
build\winperfmon.exe --selftest   :: 自检，返回码 = 成败（0 = PASS）
build\winperfmon.exe --probe      :: 传感器探针，格式见 docs/release-github.md §11
build\winperfmon.exe --version
```

命令行覆盖（对应 `docs/config-schema.md` §8）：`--interval <ms>`、`--vertical`、
`--horizontal`、`--gpu <n>`。

> 输出重定向（`> out.txt` / 管道）正常工作：程序**优先沿用调用方已接好的标准句柄**，
> 只有在没有有效 stdout（双击运行）时才 `AttachConsole`/`AllocConsole`。

## 4. 单测

```cmd
build\wpm_tests.exe
:: 或
ctest --test-dir build --output-on-failure
```

当前 **22 passed / 0 failed**，覆盖：

- `json_*`：标量 / 对象 / 非法输入（尾随逗号、注释、未闭合 = 空串）/ 往返 / 深度与字节上限 / `\uXXXX` 转义
- `config_*`：默认值稳定（13 项默认可见）、目录全展开、越界夹紧、未知丢弃 + 缺失追加、
  别名与大小写规范、去重留首、强制至少一项可见、字段夹紧、颜色解析、JSON 往返、
  坏 JSON 不抛、未知键忽略、类型不符回退、枚举字符串稳定性
- `catalog_*`：大小写不敏感查找、id 唯一且全小写

## 5. M0 验收记录（本机，2026-10）

- 机器：HP 战 66 14 G8a，AMD Ryzen 7 H 255（Family 25 = 0x19 / Model 117 = 0x75，Hawk Point），
  16 逻辑核，Windows 11 家庭版 26300.9711。
- 非管理员账户，`Performance Log Users` 组成员。
- `--selftest` → `SELFTEST PASS`（可用 6 项，错误 0 项）。
- 免驱链路实测：
  - `cpu.freq.avg` = 基频 `3793 MHz` × PDH `% Processor Performance`
    （空载约 100%，负载下实测 104–113%，即 3.9–4.3 GHz，随负载真实跟踪）
  - `cpu.usage` 走 `% Processor Utility`，缺失时回退 `% Processor Time`
  - `mem.usage` / `mem.used` 走 `GlobalMemoryStatusEx`
- 未接线项**不显示 0**：按 `Availability` 标注为「需要 Ring0 通道（PawnIO），M1 接入」
  「需要厂商 GPU 库（NVML/ADLX/IGCL），M1 接入」「需要 PresentMon，M2 接入」。
- `--probe` 输出的头部字段（`admin=false`、`pawnio=absent`、`hvci=on`、
  `osBuild=26300.9711`）在本机读通，可直接作为 issue 附件。

## 6. 下一步（M1）

按 `docs/winperfmon-dev-plan.md` §8：PawnIO 集成、MSR（`APERF`/`MPERF`、
`IA32_THERM_STATUS`、RAPL）、AMD SMN 温度、Super I/O 探测 + 风扇 RPM，
以及笔记本的 EC / 厂商 WMI 降级链（§3.3.1）。
