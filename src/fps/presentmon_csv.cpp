// SPDX-License-Identifier: MIT
#include "fps/presentmon_csv.h"

#include <cstdlib>
#include <cstring>

namespace wpm::fps {
namespace {

constexpr std::string_view kFrameTimeCol = "MsBetweenPresents";
constexpr std::string_view kCpuBusyCol = "MsCPUBusy";
constexpr std::string_view kGpuBusyCol = "MsGPUBusy";
constexpr std::string_view kDroppedCol = "Dropped";
constexpr std::string_view kPresentModeCol = "PresentMode";

/// 表头之前允许出现多少行横幅/警告。超过这个数说明对方根本不是 PresentMon 的逐帧输出，
/// 宁可报错也别装作在采集（用户会以为 FPS 是 0 是游戏的问题）。
constexpr uint64_t kMaxLinesBeforeHeader = 200;

void TrimInPlace(std::string_view* s) {
    while (!s->empty() && (s->front() == ' ' || s->front() == '\t' || s->front() == '\r')) s->remove_prefix(1);
    while (!s->empty() && (s->back() == ' ' || s->back() == '\t' || s->back() == '\r')) s->remove_suffix(1);
}

/// 去掉整行两端空白，顺便剥掉 UTF-8 BOM。
std::string_view CleanLine(std::string_view line) {
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.remove_prefix(3);
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    return line;
}

}  // namespace

void SplitCsvLine(std::string_view line, std::vector<std::string_view>* fields) {
    fields->clear();
    const size_t n = line.size();
    size_t i = 0;
    while (i <= n) {
        if (i == n) {  // 尾随逗号 => 尾随空字段
            fields->push_back(std::string_view{});
            break;
        }
        if (line[i] == '"') {
            const size_t open = i + 1;
            const size_t close = line.find('"', open);
            if (close == std::string_view::npos) {  // 引号没闭合：整行剩余算一个字段
                fields->push_back(line.substr(open));
                break;
            }
            // 不做 "" 反转义：PresentMon 的 PresentMode 里不含引号，保持视图零拷贝更重要
            fields->push_back(line.substr(open, close - open));
            const size_t next = line.find(',', close + 1);
            if (next == std::string_view::npos) break;
            i = next + 1;
            continue;
        }
        const size_t comma = line.find(',', i);
        if (comma == std::string_view::npos) {
            fields->push_back(line.substr(i));
            break;
        }
        fields->push_back(line.substr(i, comma - i));
        i = comma + 1;
    }
}

bool ParseCsvDouble(std::string_view field, double* out) {
    TrimInPlace(&field);
    if (field.empty()) return false;
    // 用 std::string 兜住（strtod 需要 NUL 结尾）；字段都很短，不构成热路径问题：
    // 一帧一行、一行几十字节，每行最多 5 次转换。
    char buf[64];
    if (field.size() >= sizeof(buf)) return false;
    std::memcpy(buf, field.data(), field.size());
    buf[field.size()] = '\0';
    // PresentMon 偶尔写 "NA" / 空值表示该帧没有该指标
    if (buf[0] == 'N' || buf[0] == 'n') return false;
    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end == buf) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    *out = v;
    return true;
}

bool PresentMonCsvParser::ParseHeaderLine(const std::string& line) {
    std::vector<std::string_view> fields;
    SplitCsvLine(line, &fields);
    columns_ = Columns{};
    for (size_t i = 0; i < fields.size(); ++i) {
        std::string_view f = fields[i];
        TrimInPlace(&f);
        if (f.size() >= 2 && f.front() == '"' && f.back() == '"') f = f.substr(1, f.size() - 2);
        const int idx = static_cast<int>(i);
        if (f == kFrameTimeCol) columns_.frameTime = idx;
        else if (f == kCpuBusyCol) columns_.cpuBusy = idx;
        else if (f == kGpuBusyCol) columns_.gpuBusy = idx;
        else if (f == kDroppedCol) columns_.dropped = idx;
        else if (f == kPresentModeCol) columns_.presentMode = idx;
    }
    columns_.headerFields = static_cast<int>(fields.size());
    return columns_.frameTime >= 0;
}

bool PresentMonCsvParser::ParseDataLine(const std::string& line, FrameSample* out) const {
    std::vector<std::string_view> fields;
    SplitCsvLine(line, &fields);
    if (columns_.frameTime < 0 || static_cast<size_t>(columns_.frameTime) >= fields.size()) return false;

    out->frameTimeMs = 0.0;
    out->cpuBusyMs = -1.0;
    out->gpuBusyMs = -1.0;
    out->dropped = false;
    out->valid = true;
    out->presentMode.clear();

    if (!ParseCsvDouble(fields[static_cast<size_t>(columns_.frameTime)], &out->frameTimeMs)) return false;

    auto readOptional = [&fields](int col, double* dst) {
        if (col < 0 || static_cast<size_t>(col) >= fields.size()) return;
        double v = 0.0;
        if (ParseCsvDouble(fields[static_cast<size_t>(col)], &v)) *dst = v;
    };
    readOptional(columns_.cpuBusy, &out->cpuBusyMs);
    readOptional(columns_.gpuBusy, &out->gpuBusyMs);

    if (columns_.dropped >= 0 && static_cast<size_t>(columns_.dropped) < fields.size()) {
        double v = 0.0;
        if (ParseCsvDouble(fields[static_cast<size_t>(columns_.dropped)], &v)) out->dropped = (v != 0.0);
    }
    if (columns_.presentMode >= 0 && static_cast<size_t>(columns_.presentMode) < fields.size()) {
        std::string_view m = fields[static_cast<size_t>(columns_.presentMode)];
        TrimInPlace(&m);
        if (m.size() >= 2 && m.front() == '"' && m.back() == '"') m = m.substr(1, m.size() - 2);
        out->presentMode.assign(m);
        // PresentMon 用 "NA" 表示这一帧没拿到模式，那不是一种模式
        if (out->presentMode == "NA" || out->presentMode == "n/a") out->presentMode.clear();
    }
    return true;
}

bool PresentMonCsvParser::Feed(std::string_view chunk, std::vector<FrameSample>* out, std::string* error) {
    pending_.append(chunk.data(), chunk.size());
    size_t start = 0;
    while (true) {
        const size_t nl = pending_.find('\n', start);
        if (nl == std::string::npos) {
            // 保留未完成的尾行
            if (start > 0) pending_.erase(0, start);
            break;
        }
        const std::string line = pending_.substr(start, nl - start);
        start = nl + 1;
        std::string_view clean = CleanLine(line);
        if (clean.empty()) continue;
        ++lines_;
        if (!headerSeen_) {
            // 表头之前可能有 PresentMon 的警告/横幅行，跳过直到认出表头
            std::string_view probe = clean;
            if (probe.find(kFrameTimeCol) == std::string_view::npos) {
                if (++skippedBeforeHeader_ > kMaxLinesBeforeHeader) {
                    if (error) {
                        *error = "前 " + std::to_string(kMaxLinesBeforeHeader) +
                                 " 行里都没有 MsBetweenPresents 列，这不像是 PresentMon 的逐帧输出";
                    }
                    return false;
                }
                continue;
            }
            if (!ParseHeaderLine(std::string(clean))) {
                if (error) *error = "PresentMon CSV 表头里没有 MsBetweenPresents 列";
                return false;
            }
            headerSeen_ = true;
            continue;
        }
        FrameSample s;
        if (!ParseDataLine(std::string(clean), &s)) {
            ++malformed_;
            continue;
        }
        ++frames_;
        if (out) out->push_back(std::move(s));
    }
    // 防御：进程被强杀时可能永远不给我们换行符，别让 pending_ 无限长
    if (pending_.size() > (1u << 20)) {
        if (error) *error = "PresentMon 输出里出现超过 1 MB 的单行，疑似不是 CSV";
        return false;
    }
    return true;
}

void PresentMonCsvParser::Finish(std::vector<FrameSample>* out) {
    if (pending_.empty()) return;
    // 先搬出来再 clear：clean 是 pending_ 的视图，clear 之后它指向的是被释放的缓冲
    const std::string last = pending_;
    pending_.clear();
    std::string_view clean = CleanLine(last);
    if (clean.empty()) return;
    ++lines_;
    if (!headerSeen_) {
        if (clean.find(kFrameTimeCol) == std::string_view::npos) {
            ++skippedBeforeHeader_;
            return;
        }
        if (!ParseHeaderLine(std::string(clean))) return;
        headerSeen_ = true;
        return;
    }
    FrameSample s;
    if (!ParseDataLine(std::string(clean), &s)) {
        ++malformed_;
        return;
    }
    ++frames_;
    if (out) out->push_back(std::move(s));
}

std::string PresentMonCsvParser::HeaderNote() const {
    if (!headerSeen_) return "(未读到表头)";
    std::string note;
    auto add = [&note](std::string_view name, int col) {
        if (col < 0) return;
        if (!note.empty()) note += ",";
        note.append(name);
    };
    add(kFrameTimeCol, columns_.frameTime);
    add(kCpuBusyCol, columns_.cpuBusy);
    add(kGpuBusyCol, columns_.gpuBusy);
    add(kDroppedCol, columns_.dropped);
    add(kPresentModeCol, columns_.presentMode);
    return note;
}

}  // namespace wpm::fps
