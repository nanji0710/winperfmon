// SPDX-License-Identifier: MIT
// PresentMon CSV 解析单测。全部使用**手工合成的**夹具文本（release-github.md §6 要求：
// 录制样本不得含可识别信息，这里干脆一条真实样本都不用）。
// 重点覆盖的是「采集不会因为一行坏数据而中断」这条：真实场景里进程被杀、游戏切桌面、
// 驱动重载都会让 CSV 出现半行、空值、NA 与乱码横幅。
#include "fps/presentmon_csv.h"

#include <string>
#include <vector>

#include "test.h"

using namespace wpm::fps;

namespace {

const char* kFullCsv =
    "Application,ProcessID,SwapChainAddress,PresentRuntime,MsBetweenPresents,MsCPUBusy,"
    "MsGPUBusy,Dropped,PresentMode\n"
    "fixture.exe,4242,0x1,DXGI,16.667,3.1,8.2,0,Hardware: Independent Flip\n"
    "fixture.exe,4242,0x1,DXGI,16.700,3.0,8.0,0,\"Hardware: Independent Flip\"\n"
    "fixture.exe,4242,0x1,DXGI,20.000,NA,NA,1,Composed: Copy with GPU GDI\n";

std::vector<FrameSample> ParseAll(const std::string& text, PresentMonCsvParser* parser,
                                 bool* ok = nullptr) {
    std::vector<FrameSample> frames;
    std::string error;
    const bool good = parser->Feed(text, &frames, &error);
    parser->Finish(&frames);
    if (ok != nullptr) *ok = good;
    return frames;
}

}  // namespace

WPM_TEST(presentmon_header_columns_are_mapped_by_name) {
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(kFullCsv, &parser);
    WPM_CHECK(parser.headerSeen());
    WPM_CHECK_EQ(parser.columns().frameTime, 4);
    WPM_CHECK_EQ(parser.columns().cpuBusy, 5);
    WPM_CHECK_EQ(parser.columns().gpuBusy, 6);
    WPM_CHECK_EQ(parser.columns().dropped, 7);
    WPM_CHECK_EQ(parser.columns().presentMode, 8);
    WPM_CHECK_EQ(frames.size(), 3u);
    WPM_CHECK_EQ(parser.frames(), 3u);
    WPM_CHECK_EQ(parser.malformed(), 0u);
}

WPM_TEST(presentmon_column_order_does_not_matter) {
    const char* csv =
        "MsBetweenPresents,PresentMode,Application\n"
        "12.5,Composed: Copy,fixture.exe\n";
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(csv, &parser);
    WPM_CHECK_EQ(parser.columns().frameTime, 0);
    WPM_CHECK_EQ(parser.columns().presentMode, 1);
    WPM_CHECK_EQ(frames.size(), 1u);
    WPM_CHECK_NEAR(frames[0].frameTimeMs, 12.5, 1e-9);
    WPM_CHECK(frames[0].presentMode == "Composed: Copy");
    // 没认出的列保持 -1：不能因为列少就当成 0
    WPM_CHECK_EQ(parser.columns().cpuBusy, -1);
    WPM_CHECK_NEAR(frames[0].cpuBusyMs, -1.0, 1e-12);
    WPM_CHECK_NEAR(frames[0].gpuBusyMs, -1.0, 1e-12);
}

// "NA" 是「这一帧没这个指标」，不是 0：必须落成 -1 让上层知道缺数据
WPM_TEST(presentmon_na_becomes_missing_not_zero) {
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(kFullCsv, &parser);
    WPM_CHECK_EQ(frames.size(), 3u);
    WPM_CHECK_NEAR(frames[0].cpuBusyMs, 3.1, 1e-6);
    WPM_CHECK_NEAR(frames[1].cpuBusyMs, 3.0, 1e-6);
    WPM_CHECK_NEAR(frames[2].cpuBusyMs, -1.0, 1e-12);
    WPM_CHECK_NEAR(frames[2].gpuBusyMs, -1.0, 1e-12);
}

WPM_TEST(presentmon_dropped_flag_and_quoted_mode) {
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(kFullCsv, &parser);
    WPM_CHECK(!frames[0].dropped);
    WPM_CHECK(!frames[1].dropped);
    WPM_CHECK(frames[2].dropped);
    // 带引号的字段必须剥掉引号
    WPM_CHECK(frames[1].presentMode == "Hardware: Independent Flip");
    WPM_CHECK(frames[0].presentMode == "Hardware: Independent Flip");
    WPM_CHECK(frames[2].presentMode == "Composed: Copy with GPU GDI");
}

// 分块喂入（管道会任意切）：逐字节喂进来的结果必须与整块喂完全一致
WPM_TEST(presentmon_chunked_feeding_matches_whole_block) {
    PresentMonCsvParser whole;
    const std::vector<FrameSample> a = ParseAll(kFullCsv, &whole);

    PresentMonCsvParser chunked;
    std::vector<FrameSample> b;
    std::string error;
    const std::string text(kFullCsv);
    for (size_t i = 0; i < text.size(); ++i) {
        const bool ok = chunked.Feed(std::string_view(text).substr(i, 1), &b, &error);
        WPM_CHECK(ok);
    }
    chunked.Finish(&b);

    WPM_CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        WPM_CHECK_NEAR(a[i].frameTimeMs, b[i].frameTimeMs, 1e-9);
        WPM_CHECK_EQ(a[i].dropped, b[i].dropped);
        WPM_CHECK(a[i].presentMode == b[i].presentMode);
    }
}

// 表头之前常有 PresentMon 的横幅/警告行：跳过它们，并且能被数出来
WPM_TEST(presentmon_skips_banner_lines_before_header) {
    const char* csv =
        "warning: falling back to the slower path\n"
        "PresentMon 2.3.0 (fixture)\n"
        "\n"
        "MsBetweenPresents,PresentMode\n"
        "16.0,Hardware: Independent Flip\n";
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(csv, &parser);
    WPM_CHECK_EQ(frames.size(), 1u);
    WPM_CHECK(parser.skippedBeforeHeader() >= 2u);
    WPM_CHECK_EQ(parser.malformed(), 0u);
}

WPM_TEST(presentmon_malformed_rows_are_counted_not_fatal) {
    const char* csv =
        "MsBetweenPresents,PresentMode\n"
        "16.0,Hardware: Independent Flip\n"
        "not-a-number,Composed: Copy\n"
        ",Composed: Copy\n"           // 空帧时间
        "16.5,Hardware: Independent Flip\n";
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(csv, &parser);
    WPM_CHECK_EQ(frames.size(), 2u);
    WPM_CHECK_EQ(parser.malformed(), 2u);
    WPM_CHECK_EQ(parser.frames(), 2u);
}

// 表里没有 MsBetweenPresents 就不是 PresentMon 的逐帧输出 → 别一直傻等
WPM_TEST(presentmon_missing_frametime_column_is_fatal) {
    std::string csv = "Application,ProcessID,MsCPUBusy\n";
    for (int i = 0; i < 300; ++i) csv += "fixture.exe,4242,3.1\n";
    PresentMonCsvParser parser;
    std::vector<FrameSample> frames;
    std::string error;
    const bool ok = parser.Feed(csv, &frames, &error);
    WPM_CHECK(!ok);
    WPM_CHECK(!error.empty());
    WPM_CHECK(!parser.headerSeen());
    WPM_CHECK_EQ(frames.size(), 0u);
}

WPM_TEST(presentmon_crlf_and_bom_are_tolerated) {
    const char* csv =
        "\xEF\xBB\xBFMsBetweenPresents,PresentMode\r\n"
        "16.0,Hardware: Independent Flip\r\n"
        "17.0,Composed: Copy\r\n";
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(csv, &parser);
    WPM_CHECK_EQ(frames.size(), 2u);
    WPM_CHECK_NEAR(frames[0].frameTimeMs, 16.0, 1e-9);
    WPM_CHECK(frames[1].presentMode == "Composed: Copy");
}

// 进程被强杀：最后一行没有换行符，也必须被解析出来（否则丢的正是最关键的那几帧）
WPM_TEST(presentmon_finish_flushes_unterminated_last_line) {
    const char* csv =
        "MsBetweenPresents,PresentMode\n"
        "16.0,Hardware: Independent Flip\n"
        "17.5,Hardware: Independent Flip";  // 注意没有 \n
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(csv, &parser);
    WPM_CHECK_EQ(frames.size(), 2u);
    WPM_CHECK_NEAR(frames[1].frameTimeMs, 17.5, 1e-9);
}

WPM_TEST(presentmon_split_csv_line_handles_quotes_and_empties) {
    std::vector<std::string_view> fields;
    const std::string line = "a,\"b,c\",d,";
    SplitCsvLine(line, &fields);
    WPM_CHECK_EQ(fields.size(), 4u);
    WPM_CHECK(fields[0] == "a");
    WPM_CHECK(fields[1] == "b,c");
    WPM_CHECK(fields[2] == "d");
    WPM_CHECK(fields[3].empty());
}

WPM_TEST(presentmon_parse_double_rejects_junk) {
    double v = 0.0;
    WPM_CHECK(ParseCsvDouble("16.667", &v));
    WPM_CHECK_NEAR(v, 16.667, 1e-9);
    WPM_CHECK(ParseCsvDouble(" 12.5 ", &v));
    WPM_CHECK_NEAR(v, 12.5, 1e-9);
    WPM_CHECK(ParseCsvDouble("1e3", &v));
    WPM_CHECK_NEAR(v, 1000.0, 1e-9);
    WPM_CHECK(!ParseCsvDouble("NA", &v));
    WPM_CHECK(!ParseCsvDouble("", &v));
    WPM_CHECK(!ParseCsvDouble("12abc", &v));
}

// 解析出来的帧直接喂给统计：端到端（夹具 → 帧 → 指标）的口径一致性
WPM_TEST(presentmon_feeds_framestats_end_to_end) {
    std::string csv = "MsBetweenPresents,MsCPUBusy,MsGPUBusy,Dropped,PresentMode\n";
    for (int i = 0; i < 99; ++i) csv += "10.0,2.0,5.0,0,Hardware: Independent Flip\n";
    csv += "50.0,9.0,20.0,0,Hardware: Independent Flip\n";

    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(csv, &parser);
    WPM_CHECK_EQ(frames.size(), 100u);

    std::vector<double> times;
    times.reserve(frames.size());
    for (const FrameSample& f : frames) times.push_back(f.frameTimeMs);
    const FrameStats st = ComputeFromFrameTimes(times);
    WPM_CHECK_NEAR(st.avgFps, 100000.0 / (99.0 * 10.0 + 50.0), 1e-9);
    WPM_CHECK_NEAR(st.lowFps, 1000.0 / 50.0, 1e-9);  // k = 1 → 最差那一帧
}

// PresentMon 2.x 的 --v2_metrics 用的是另一套列名（下面是实测 2.6.0 的表头，进程名与 PID 已替换）。
// 这个用例存在的唯一理由是：真机测试时发现夹具一直用 1.x 列名 → 71 个单测全绿 → 对着真实的
// 2.6.0 输出却一行都解不出来。夹具只能证明「我们的口径自洽」，不能证明「我们认对了别人的输出」。
const char* kV2Csv =
    "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,"
    "PresentMode,CPUStartTime,FrameTime,CPUBusy,CPUWait,GPULatency,GPUTime,GPUBusy,GPUWait,"
    "DisplayLatency,DisplayedTime,AnimationError,AnimationTime,MsFlipDelay,AllInputToPhotonLatency,"
    "ClickToPhotonLatency\n"
    "fixture.exe,4242,0x1,DXGI,0,0,0,Composed: Flip,30.9330,16.6670,3.1000,0.3311,30.9371,0.4213,"
    "8.2000,0.0000,40.7827,33.3094,NA,30.9330,NA,NA,NA\n"
    "fixture.exe,4242,0x1,DXGI,0,0,0,Hardware: Independent Flip,47.6000,16.7000,3.0000,0.3691,"
    "30.2192,0.1442,8.0000,0.0000,43.0303,33.3358,-2.2476,47.6000,NA,NA,NA\n";

WPM_TEST(presentmon_v2_metrics_columns_are_recognised) {
    PresentMonCsvParser parser;
    const std::vector<FrameSample> frames = ParseAll(kV2Csv, &parser);
    WPM_CHECK_EQ(frames.size(), static_cast<size_t>(2));
    WPM_CHECK_NEAR(frames[0].frameTimeMs, 16.6670, 1e-9);
    WPM_CHECK_NEAR(frames[0].cpuBusyMs, 3.1000, 1e-9);
    WPM_CHECK_NEAR(frames[0].gpuBusyMs, 8.2000, 1e-9);
    WPM_CHECK_NEAR(frames[1].frameTimeMs, 16.7000, 1e-9);
    WPM_CHECK(frames[0].presentMode == "Composed: Flip");
    WPM_CHECK(frames[1].presentMode == "Hardware: Independent Flip");
    // 2.x 的 v2 输出里没有 Dropped 列：解析器必须容忍，而不是把第 0 列当成 Dropped
    WPM_CHECK(parser.columns().dropped < 0);
    WPM_CHECK(parser.columns().v2Names);
    WPM_CHECK(parser.HeaderNote().rfind("v2:", 0) == 0);
}