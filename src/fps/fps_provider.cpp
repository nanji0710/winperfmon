// SPDX-License-Identifier: MIT
#include "fps/fps_provider.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config/catalog.h"
#include "core/text.h"

namespace wpm::fps {
namespace {

using wpm::sensors::Reading;
using wpm::sensors::Snapshot;
using wpm::sensors::Status;

constexpr uint64_t kStartRetryIntervalMs = 3000;  ///< 启动失败后的重试间隔（别每 0.5 s 试一次）
/// 子进程跑了这么久还没有一行 CSV，就在降级文案里给出排查方向（PresentMon 静默失败很难猜）
constexpr uint64_t kNoOutputHintSeconds = 10;
constexpr uint64_t kFrameStaleMs = 3000;          ///< 超过这么久没有新帧就认为采集停了
constexpr size_t kMaxChunk = 64 * 1024;

std::string ToLowerAsciiCopy(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string FileNameOf(std::string path) {
    const size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(pos + 1);
    return path;
}

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return std::wstring();
    return std::wstring(buf, n);
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return std::wstring();
    std::wstring path(buf, n);
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

void AppendSearchNote(std::string* note, const std::string& line) {
    if (!note->empty()) *note += "；";
    *note += line;
}

}  // namespace

PresentMonLocation LocatePresentMon() {
    PresentMonLocation loc;
    std::vector<std::wstring> candidates;
    const std::wstring exeDir = ExeDir();
    if (!exeDir.empty()) candidates.push_back(exeDir + L"\\PresentMon.exe");
    const std::wstring local = EnvVar(L"LOCALAPPDATA");
    if (!local.empty()) candidates.push_back(local + L"\\PresentMon\\PresentMon.exe");
    const std::wstring pf = EnvVar(L"ProgramFiles");
    if (!pf.empty()) {
        candidates.push_back(pf + L"\\PresentMon\\PresentMon.exe");
        candidates.push_back(pf + L"\\PresentMon\\PresentMon_x64.exe");
    }
    const std::wstring pf86 = EnvVar(L"ProgramFiles(x86)");
    if (!pf86.empty()) candidates.push_back(pf86 + L"\\PresentMon\\PresentMon.exe");

    for (const std::wstring& c : candidates) {
        if (FileExists(c)) {
            loc.path = wpm::text::toUtf8(c);
            loc.note = "找到 PresentMon.exe：" + loc.path;
            return loc;
        }
    }

    // PATH 兜底
    wchar_t found[MAX_PATH * 2] = {};
    const DWORD n = SearchPathW(nullptr, L"PresentMon.exe", nullptr,
                                static_cast<DWORD>(std::size(found)), found, nullptr);
    if (n > 0 && n < std::size(found)) {
        loc.path = wpm::text::toUtf8(std::wstring(found, n));
        loc.note = "找到 PresentMon.exe（PATH）：" + loc.path;
        return loc;
    }

    std::string tried;
    for (const std::wstring& c : candidates) {
        if (!tried.empty()) tried += " / ";
        tried += wpm::text::toUtf8(c);
    }
    AppendSearchNote(&tried, "PATH 里也没有");
    loc.note = "没找到 PresentMon.exe；找过：" + tried +
               "。把官方 PresentMon 便携版放到 winperfmon.exe 同目录即可（无需安装）";
    return loc;
}

std::string ForegroundProcessName() {
    HWND hwnd = GetForegroundWindow();
    if (hwnd == nullptr) return std::string();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return std::string();
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) return std::string();
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(buf));
    std::string name;
    if (QueryFullProcessImageNameW(proc, 0, buf, &size)) {
        name = ToLowerAsciiCopy(FileNameOf(wpm::text::toUtf8(std::wstring(buf, size))));
    }
    CloseHandle(proc);
    return name;
}

bool LooksLikeGameProcess(std::string_view exeName) {
    if (exeName.empty()) return false;
    const std::string name = ToLowerAsciiCopy(std::string(exeName));
    static const char* kNotGames[] = {
        "explorer.exe",      "winperfmon.exe",     "cmd.exe",       "powershell.exe",
        "pwsh.exe",          "windowsterminal.exe","conhost.exe",   "openconsole.exe",
        "taskmgr.exe",       "code.exe",           "devenv.exe",    "notepad.exe",
        "browser.exe",       "chrome.exe",         "msedge.exe",    "firefox.exe",
        "wechat.exe",        "qq.exe",             "dingtalk.exe",  "wps.exe",
        "winword.exe",       "excel.exe",          "powerpnt.exe",  "explorer",
        "dwm.exe",           "searchapp.exe",      "shellexperiencehost.exe",
    };
    for (const char* n : kNotGames) {
        if (name == n) return false;
    }
    return true;
}

FpsProvider::FpsProvider() : ring_(65536) {}

FpsProvider::~FpsProvider() { StopCapture("对象销毁"); }

void FpsProvider::SetOptions(const wpm::config::FpsConfig& cfg) {
    cfg_ = cfg;
    const int keep = std::clamp(cfg.keepBufferFrames, 4096, 262144);
    if (static_cast<size_t>(keep) != ring_.Capacity()) {
        ring_ = FrameRing(static_cast<size_t>(keep));
    }
    opt_.lowPercentile = std::clamp(static_cast<double>(cfg.lowPercentile), 0.0001, 0.5);
    opt_.stutterFactor = std::clamp(static_cast<double>(cfg.stutterFactor), 1.1, 20.0);
    opt_.baseline = cfg.stutterBaseline;
}

size_t FpsProvider::SlotOf(std::string_view id) const {
    const auto catalog = wpm::config::MetricCatalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        if (catalog[i].id == id) return i;
    }
    return static_cast<size_t>(-1);
}

bool FpsProvider::Init(std::string& detail) {
    pm_ = LocatePresentMon();
    slots_.clear();
    static const char* kIds[] = {
        "fps.instant", "fps.avg",     "fps.low1",      "fps.low01",     "fps.frametime",
        "fps.frametime.graph", "fps.stutter", "fps.presentmode", "fps.cpu.busy", "fps.gpu.busy",
    };
    for (const char* id : kIds) {
        const size_t slot = SlotOf(id);
        if (slot != static_cast<size_t>(-1)) slots_.push_back(slot);
    }
    available_ = true;  // provider 本身可用；能不能采到数据取决于 PresentMon 与目标进程

    if (cfg_.mode == wpm::config::FpsMode::Off) {
        note_ = "配置里 fps.mode = off，FPS 采集已关闭";
    } else if (!pm_.found()) {
        note_ = pm_.note;
    } else if (!cfg_.targetProcess.empty()) {
        note_ = "待采目标（配置指定）：" + cfg_.targetProcess;
    } else {
        note_ = "待采目标：跟随前台进程" + pm_.note;
    }
    detail = note_;
    return true;
}

bool FpsProvider::StartCapture(const std::string& target, const char* source, std::string* detail) {
    if (!pm_.found()) {
        if (detail) *detail = pm_.note;
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        if (detail) *detail = "创建管道失败（错误 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    // 参数顺序按 PresentMon 官方文档；--v2_metrics 是 2.x 的写法，1.x 会报未知参数，
    // 因此失败后会在 PumpPipe 里去掉它重来一次（见 retriedWithoutV2_）。
    // --exclude_dropped：只要真正上屏的帧。2.x 的 --v2_metrics 输出里**没有** Dropped 列
    // （实测 2.6.0 表头没有该字段），靠 PresentMon 自己过滤比在 CSV 里猜标志位可靠。
    // --session_name winperfmon：不占默认的 "PresentMon" 会话名，避免和 CapFrameX 等工具互踢。
    // --stop_existing_session：上一次被强杀（Kill/Ctrl+C）会留下同名会话一直处于 Running，
    //   此后每次抓取都**静默拿到 0 帧**——本机实测踩过这个坑（抓不到任何数据，stderr 里只有
    //   一句与非管理员相关的警告）。注意不能用 --terminate_existing_session：官方语义是
    //   「停掉已有会话然后退出」，那样我们永远采不到东西。
    std::wstring cmdline = L"\"" + wpm::text::toWide(pm_.path) + L"\" --process_name " +
                           wpm::text::toWide(target) +
                           L" --output_stdout --no_console_stats --exclude_dropped"
                           L" --session_name winperfmon --stop_existing_session";
    if (!retriedWithoutV2_) cmdline += L" --v2_metrics";
    cmdline += L" --terminate_on_proc_exit";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;  // stderr 也收进来：PresentMon 的报错只有这里能看到
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);  // 我们的副本必须关掉，否则读端永远等不到 EOF
    if (!ok) {
        CloseHandle(readEnd);
        if (detail) {
            *detail = "启动 PresentMon 失败（错误 " + std::to_string(GetLastError()) +
                      "）：" + pm_.path;
        }
        return false;
    }
    CloseHandle(pi.hThread);
    hProcess_ = pi.hProcess;
    hRead_ = readEnd;
    hThread_ = nullptr;
    running_ = true;
    usedV2Metrics_ = !retriedWithoutV2_;
    target_ = target;
    targetSource_ = source != nullptr ? source : "";
    targetPid_ = pi.dwProcessId;
    exitCode_ = -1;
    framesSeen_ = 0;
    lastFrameTick_ = GetTickCount64();
    if (detail) {
        *detail = "已启动 PresentMon（PID " + std::to_string(pi.dwProcessId) + "）采集 " + target +
                  "（目标来源：" + targetSource_ + "）";
    }
    return true;
}

void FpsProvider::StopCapture(const char* why) {
    if (hProcess_ != nullptr) {
        if (running_) {
            DWORD code = 0;
            if (GetExitCodeProcess(static_cast<HANDLE>(hProcess_), &code)) {
                if (code == STILL_ACTIVE) {
                    TerminateProcess(static_cast<HANDLE>(hProcess_), 0);
                } else {
                    exitCode_ = static_cast<int>(code);
                }
            }
        }
        CloseHandle(static_cast<HANDLE>(hProcess_));
        hProcess_ = nullptr;
    }
    if (hThread_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(hThread_));
        hThread_ = nullptr;
    }
    if (hRead_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(hRead_));
        hRead_ = nullptr;
    }
    running_ = false;
    if (why != nullptr && note_.empty()) note_ = why;
}

void FpsProvider::PumpPipe() {
    if (!running_ || hRead_ == nullptr) return;
    std::vector<char> chunk(kMaxChunk);
    for (int guard = 0; guard < 64; ++guard) {  // 一轮最多读 4 MB，防极端情况饿死采样
        DWORD avail = 0;
        if (!PeekNamedPipe(static_cast<HANDLE>(hRead_), nullptr, 0, nullptr, &avail, nullptr)) {
            // 管道断了：进程已退出
            DWORD code = 0;
            if (hProcess_ != nullptr && GetExitCodeProcess(static_cast<HANDLE>(hProcess_), &code)) {
                exitCode_ = static_cast<int>(code);
            }
            if (!parser_.headerSeen() && !retriedWithoutV2_) {
                // PresentMon 1.x 不认 --v2_metrics：去掉它重来一次（只重试一次）
                StopCapture("PresentMon 不认 --v2_metrics，准备重试");
                retriedWithoutV2_ = true;
                std::string detail;
                if (StartCapture(target_, targetSource_.c_str(), &detail)) return;
            }
            note_ = parser_.headerSeen()
                        ? ("采集已结束（PresentMon 退出，退出码 " + std::to_string(exitCode_) + "）")
                        : ("PresentMon 没有输出任何可解析的帧数据就退出了（退出码 " +
                           std::to_string(exitCode_) + "）");
            StopCapture(nullptr);
            return;
        }
        if (avail == 0) return;
        const DWORD want = std::min<DWORD>(avail, static_cast<DWORD>(chunk.size()));
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(hRead_), chunk.data(), want, &got, nullptr) || got == 0) return;
        std::vector<FrameSample> frames;
        std::string error;
        if (!parser_.Feed(std::string_view(chunk.data(), got), &frames, &error)) {
            note_ = "PresentMon 输出解析失败：" + error;
            StopCapture(nullptr);
            return;
        }
        for (FrameSample& f : frames) {
            // 加载帧 / 失焦帧：这里先不判定（M3 用窗口焦点事件），只把明显不合理的帧剔掉
            ring_.Add(f);
        }
        if (!frames.empty()) {
            framesSeen_ += frames.size();
            lastFrameTick_ = GetTickCount64();
        }
    }
}

bool FpsProvider::EnsureStarted() {
    if (running_) return true;
    if (diagnosticOnly_) {
        note_ = "诊断模式（--probe）：只报状态，不启动 PresentMon 子进程";
        return false;
    }
    if (cfg_.mode == wpm::config::FpsMode::Off) {
        note_ = "配置里 fps.mode = off，FPS 采集已关闭";
        return false;
    }
    if (!pm_.found()) {
        note_ = pm_.note;
        return false;
    }
    const uint64_t now = GetTickCount64();
    if (lastStartAttemptTick_ != 0 && now - lastStartAttemptTick_ < kStartRetryIntervalMs) return false;
    lastStartAttemptTick_ = now;

    std::string target = cfg_.targetProcess;
    const char* source = "配置";
    if (target.empty()) {
        const std::string fg = ForegroundProcessName();
        if (fg.empty()) {
            note_ = "跟随前台：取不到前台窗口的进程名（没有前台窗口？）";
            return false;
        }
        if (!LooksLikeGameProcess(fg)) {
            note_ = "跟随前台：前台是 " + fg + "（不像游戏），等前台切到游戏再开始";
            return false;
        }
        target = fg;
        source = "前台";
    }
    std::string detail;
    const bool ok = StartCapture(target, source, &detail);
    note_ = detail;
    return ok;
}

void FpsProvider::WriteMetrics(Snapshot& snap) {
    auto setReading = [&snap](size_t slot, double value, Status status, std::string detail) {
        if (slot >= snap.metrics.size()) return;
        Reading& r = snap.metrics[slot];
        r.value = value;
        r.status = status;
        r.detail = std::move(detail);
    };
    auto setUnavailable = [&](const char* id, std::string why) {
        setReading(SlotOf(id), 0.0, Status::Unavailable, std::move(why));
    };

    const bool haveFrames = !ring_.Empty();
    const FrameStats st = ring_.Compute(opt_);
    FrameStatsOptions low01 = opt_;
    low01.lowPercentile = 0.001;
    const FrameStats stLow01 = ring_.Compute(low01);

    if (!haveFrames) {
        std::string why = note_.empty() ? "还没有帧数据（PresentMon 未启动或游戏未在前台）" : note_;
        // 「进程在跑、表头却一行都没读到」是这套链路最难查的状态：PresentMon 是**静默**失败的
        // （stderr 只有一句跟非管理员有关的常规警告）。这里如实说清「等了多久、一行都没有」，
        // 并把本机实测过的两个成因摆出来，避免用户对着「FPS --」干猜。
        if (running_ && lastStartAttemptTick_ != 0) {
            const uint64_t waited = (GetTickCount64() - lastStartAttemptTick_) / 1000;
            if (waited >= kNoOutputHintSeconds && parser_.lines() == 0) {
                why += "；PresentMon 已运行 " + std::to_string(waited) +
                       " 秒但一行 CSV 都没输出——常见成因：残留的 ETW 会话（本机实测过一次，"
                       "已用 --session_name winperfmon --stop_existing_session 规避）、"
                       "或需要以管理员身份运行（PresentMon 对非管理员只能跟踪同账户的长进程）";
            }
        }
        for (size_t slot : slots_) setReading(slot, 0.0, Status::Unavailable, why);
        return;
    }

    const uint64_t now = GetTickCount64();
    const bool stalled = (now - lastFrameTick_) > kFrameStaleMs;
    const double lastFrameMs = static_cast<double>(ring_.FrameTimeAt(ring_.Count() - 1));
    std::string aging = stalled
                            ? ("已 " + std::to_string((now - lastFrameTick_) / 1000) +
                               " s 没有新帧（游戏暂停/最小化/切了前台？）——下面是最后一帧之前的统计")
                            : ("目标 " + target_ + "（" + targetSource_ + "），" +
                               std::to_string(ring_.Count()) + " 帧样本");
    const Status liveStatus = stalled ? Status::Stale : Status::Ok;

    setReading(SlotOf("fps.instant"), lastFrameMs > 0.0 ? 1000.0 / lastFrameMs : 0.0, liveStatus,
               aging);
    setReading(SlotOf("fps.avg"), st.avgFps, liveStatus,
               "调和平均：帧数 × 1000 / Σ帧时间（" + std::to_string(st.frames) + " 帧）");
    setReading(SlotOf("fps.low1"), st.lowFps, liveStatus,
               "最差 " + std::to_string(static_cast<uint64_t>(
                                      static_cast<double>(st.frames) * opt_.lowPercentile + 0.999)) +
                   " 帧的帧时间均值取倒数；CapFrameX 分位口径为 " + wpm::text::formatDouble(st.lowFpsQuantile, 1) +
                   " FPS（两者不同时说明有并列帧时间）");
    setReading(SlotOf("fps.low01"), stLow01.lowFps, liveStatus, "同上，百分位 0.001");
    setReading(SlotOf("fps.frametime"), st.frameTimeAvgMs, liveStatus,
               "平均帧时间；中位 " + wpm::text::formatDouble(st.frameTimeMedianMs, 2) + " ms，最差 " +
                   wpm::text::formatDouble(st.frameTimeWorstMs, 2) + " ms");
    setReading(SlotOf("fps.stutter"), st.stutterPct, liveStatus,
               "帧时间 > " + wpm::text::formatDouble(st.stutterBaselineMs, 2) + " ms × " +
                   wpm::text::formatDouble(opt_.stutterFactor, 1) + " 的帧占比（" +
                   std::to_string(st.stutterFrames) + "/" + std::to_string(st.frames) + "）");
    if (!st.presentMode.empty()) {
        setReading(SlotOf("fps.presentmode"), 0.0, liveStatus,
                   st.presentMode + "（" + std::to_string(st.presentModeCount) + "/" +
                       std::to_string(st.frames) + " 帧）");
    } else {
        setUnavailable("fps.presentmode", "PresentMon 输出里没有 PresentMode 列");
    }

    if (parser_.columns().cpuBusy >= 0) {
        setReading(SlotOf("fps.cpu.busy"), st.cpuBusyAvgMs, liveStatus, "PresentMon MsCPUBusy 均值");
    } else {
        setUnavailable("fps.cpu.busy", "PresentMon 输出里没有 MsCPUBusy 列（版本较旧？）");
    }
    if (parser_.columns().gpuBusy >= 0) {
        setReading(SlotOf("fps.gpu.busy"), st.gpuBusyAvgMs, liveStatus, "PresentMon MsGPUBusy 均值");
    } else {
        setUnavailable("fps.gpu.busy", "PresentMon 输出里没有 MsGPUBusy 列（版本较旧？）");
    }
    setUnavailable("fps.frametime.graph",
                   "由 M3 的 Overlay 直接从帧时间环形缓冲绘制（" +
                       std::to_string(ring_.Count()) + " 帧在手），不走指标槽");
}

void FpsProvider::Sample(Snapshot& snap) {
    if (!available_) return;
    PumpPipe();
    if (!running_) EnsureStarted();
    WriteMetrics(snap);
}

FpsProvider::Diag FpsProvider::Probe() {
    Diag d;
    d.exePath = pm_.path;
    d.locateNote = pm_.note;
    d.running = running_;
    d.target = target_;
    d.targetSource = targetSource_;
    d.processId = targetPid_;
    d.csvLines = parser_.lines();
    d.csvFrames = parser_.frames();
    d.csvMalformed = parser_.malformed();
    d.skippedBeforeHeader = parser_.skippedBeforeHeader();
    d.headerNote = parser_.HeaderNote();
    d.ringFrames = ring_.Count();
    d.exitCode = exitCode_;
    double span = 0.0;
    for (size_t i = 0; i < ring_.Count(); ++i) span += ring_.FrameTimeAt(i);
    d.ringSpanMs = span;
    d.note = note_;
    return d;
}

}  // namespace wpm::fps
