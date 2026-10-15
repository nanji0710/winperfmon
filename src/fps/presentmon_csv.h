// SPDX-License-Identifier: MIT
// PresentMon 逐帧 CSV 解析（降级路径：子进程输出到 stdout）。
//
// 为什么不用 SDK：`PresentMonAPI2.dll` 走服务 + 共享内存，只能动态加载**系统已装的那份**；
// 而 `PresentMon.exe --output_stdout --v2_metrics` 是官方给的文本接口，进程边界清晰、
// 崩了也拖不走我们（docs/winperfmon-dev-plan.md §3.4 的第二条路径）。代价是要自己解析 CSV。
//
// 解析器不假设列顺序，只看表头名字；认出以下列（大小写敏感，与 PresentMon 一致）：
//   MsBetweenPresents（必需）、MsCPUBusy、MsGPUBusy、Dropped、PresentMode
// 兼容：CRLF/LF、BOM、表头前夹杂的警告行、被任意切开的块（网络/管道分片）、带引号字段、
// 末行无换行。任何一行坏掉都只计入 malformed，不中断采集 —— 采集中途抛异常等于丢掉整局数据。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fps/framestats.h"

namespace wpm::fps {

class PresentMonCsvParser {
public:
    /// 列名 → 列下标。未找到的列为 -1。
    struct Columns {
        int frameTime = -1;
        int cpuBusy = -1;
        int gpuBusy = -1;
        int dropped = -1;
        int presentMode = -1;
        int headerFields = 0;
        /// true = 表头用的是 PresentMon 2.x `--v2_metrics` 的列名（FrameTime/CPUBusy/GPUBusy）。
        /// 只影响回显与排障文案：两种列名的数据语义相同。
        bool v2Names = false;
    };

    PresentMonCsvParser() = default;

    /// 送入一段字节。完整行会被解析并 append 到 out。返回 false 表示致命错误（写进 error）。
    bool Feed(std::string_view chunk, std::vector<FrameSample>* out, std::string* error);

    /// 收尾：把没有换行符结尾的最后一行也解析掉。
    void Finish(std::vector<FrameSample>* out);

    bool headerSeen() const noexcept { return headerSeen_; }
    const Columns& columns() const noexcept { return columns_; }
    uint64_t lines() const noexcept { return lines_; }
    uint64_t frames() const noexcept { return frames_; }
    uint64_t malformed() const noexcept { return malformed_; }
    uint64_t skippedBeforeHeader() const noexcept { return skippedBeforeHeader_; }

    /// 人话版表头摘要，写进 detail 用（如 "MsBetweenPresents,MsCPUBusy,PresentMode"）。
    std::string HeaderNote() const;

private:
    bool ParseHeaderLine(const std::string& line);
    bool ParseDataLine(const std::string& line, FrameSample* out) const;

    std::string pending_;
    Columns columns_;
    bool headerSeen_ = false;
    uint64_t lines_ = 0;
    uint64_t frames_ = 0;
    uint64_t malformed_ = 0;
    uint64_t skippedBeforeHeader_ = 0;
};

/// 把一行按 CSV 规则切成字段（支持双引号包裹；不做 "" 反转义，PresentMon 输出里没有）。
/// 返回的视图指向 line 本身，line 必须比它们活得久。
void SplitCsvLine(std::string_view line, std::vector<std::string_view>* fields);

/// 解析一个字段为 double；失败返回 false。会跳过字段两侧的空格。
bool ParseCsvDouble(std::string_view field, double* out);

}  // namespace wpm::fps
