// SPDX-License-Identifier: MIT
// FPS 采集 provider（M2 的 CLI 路径）：
//   winperfmon → CreateProcess(PresentMon.exe --process_name X --output_stdout …) → 匿名管道 → CSV 解析 → FrameRing
//
// 为什么走子进程而不是 SDK（dev-plan §3.4）：
//   ① SDK 路径（PresentMonAPI2.dll + PresentMonSharedService）只能动态加载**系统已装的那份**，
//      且我们无法假设用户装过它；② 子进程崩了拖不走我们，进程边界就是故障边界；
//   ③ 官方只给 CSV（没有 JSON），所以解析器是必要条件而不是偷懒。
// 代价：`--output_stdout` 这条路径需要管理员**或** `Performance Log Users` 组（ETW 会话权限）。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"
#include "fps/framestats.h"
#include "fps/presentmon_csv.h"
#include "sensors/sensor.h"

namespace wpm::fps {

/// PresentMon.exe 的定位结果。找不到不是错误：免驱模式下本来就没有它。
struct PresentMonLocation {
    std::string path;  ///< 空 = 没找到
    std::string note;  ///< 人话：找过哪些地方 / 找到的是哪一个
    bool found() const noexcept { return !path.empty(); }
};

/// 依次找：<exe 目录>\PresentMon.exe（便携版）→ PATH → %LOCALAPPDATA%\PresentMon →
/// %ProgramFiles%\PresentMon → %ProgramFiles(x86)%\PresentMon。找到第一个存在的就停。
PresentMonLocation LocatePresentMon();

/// 前台窗口所属进程的可执行文件名（小写，不含路径）。取不到返回空串。
/// 用来实现 `fps.targetProcess` 为空时的「跟随前台」。
std::string ForegroundProcessName();

/// 看起来像游戏吗？——只排除明摆着不是的（shell、终端、我们自己和常见工具）。
/// 判定要保守：宁可多采一次无用的 FPS，也不要对着游戏说「你没有在玩」。
bool LooksLikeGameProcess(std::string_view exeName);

class FpsProvider final : public wpm::sensors::ISensorProvider {
public:
    FpsProvider();
    ~FpsProvider() override;

    FpsProvider(const FpsProvider&) = delete;
    FpsProvider& operator=(const FpsProvider&) = delete;

    const char* name() const noexcept override { return "fps"; }
    bool Init(std::string& detail) override;
    void Sample(wpm::sensors::Snapshot& snap) override;
    const std::vector<size_t>& Slots() const noexcept override { return slots_; }
    bool Available() const noexcept override { return available_; }

    /// 来自 config.json 的 fps 段。必须在 Init 之前调用。
    void SetOptions(const wpm::config::FpsConfig& cfg);

    /// 诊断模式（--probe）：只报状态，不启动 PresentMon 子进程。
    /// 诊断命令不该有副作用——用户跑 --probe 只是想看卡在哪一步。
    void SetDiagnosticOnly(bool on) noexcept { diagnosticOnly_ = on; }

    struct Diag {
        std::string exePath;      ///< PresentMon.exe 路径（空 = 未找到）
        std::string locateNote;   ///< LocatePresentMon 的说明
        bool running = false;     ///< 子进程是否在跑
        std::string target;       ///< 实际采集的进程名
        std::string targetSource; ///< "配置" / "前台"
        uint32_t processId = 0;
        uint64_t csvLines = 0;
        uint64_t csvFrames = 0;
        uint64_t csvMalformed = 0;
        uint64_t skippedBeforeHeader = 0;
        std::string headerNote;   ///< 认出的列
        uint64_t ringFrames = 0;
        double ringSpanMs = 0.0;  ///< 环内帧时间总和（≈这局已经采了多久）
        std::string note;         ///< 为什么没在采 / 当前状态
        int exitCode = -1;
    };
    Diag Probe();

    const std::string& Note() const noexcept { return note_; }

    /// 采集链条是否具备条件（找到 PresentMon.exe 且没被配置关闭）。
    /// 用来区分「还没开始采」与「根本采不了」——只有后者该改写成降级原因。
    bool CaptureReady() const noexcept {
        return pm_.found() && cfg_.mode != wpm::config::FpsMode::Off;
    }

private:
    bool StartCapture(const std::string& target, const char* source, std::string* detail);
    void StopCapture(const char* why);
    void PumpPipe();
    bool EnsureStarted();
    void WriteMetrics(wpm::sensors::Snapshot& snap);
    size_t SlotOf(std::string_view id) const;

    // —— 配置 ——
    wpm::config::FpsConfig cfg_{};

    // —— 定位 ——
    PresentMonLocation pm_;

    // —— 子进程 ——
    void* hProcess_ = nullptr;
    void* hThread_ = nullptr;
    void* hRead_ = nullptr;
    bool running_ = false;
    bool retriedWithoutV2_ = false;
    bool usedV2Metrics_ = true;
    int exitCode_ = -1;
    std::string target_;
    std::string targetSource_;
    uint32_t targetPid_ = 0;

    // —— 数据 ——
    PresentMonCsvParser parser_;
    FrameRing ring_;
    FrameStatsOptions opt_{};
    uint64_t lastFrameTick_ = 0;
    uint64_t lastStartAttemptTick_ = 0;
    uint64_t framesSeen_ = 0;

    std::vector<size_t> slots_;
    bool available_ = false;
    bool diagnosticOnly_ = false;
    std::string note_;
};

}  // namespace wpm::fps
