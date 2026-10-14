// SPDX-License-Identifier: MIT
// 配置数据契约（config-schema.md）：字段、默认值、校验即修复、读写接口。
//
// §0 六条不变量：
//   1 单一事实来源 —— 可见性与顺序合并到有序数组 metrics；GPU 选择只有 gpu.selectedIndex
//   2 前向兼容 —— 未知字段保留（策略可关）或忽略，未知指标 id 丢弃并记诊断
//   3 写入不破坏 —— 原子替换 + .bak，解析失败绝不覆盖用户文件
//   4 校验即修复 —— 夹紧/回退默认 + 诊断，不抛异常
//   5 热路径零锁 —— 采集线程只读 shared_ptr 快照
//   6 零第三方依赖 —— 自研 JSON 子集
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wpm::config {

// ── 枚举（config-schema.md §6）─────────────────────────────────────────────
enum class Orientation { Horizontal, Vertical };
enum class Align { Start, Center, End };
enum class ThemeId { Dark, Light, Custom };
enum class TopmostPolicy { Always, GameOnly, Never };
enum class PositionMode { Preset, Free };
enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };
enum class FpsMode { Auto, Service, Cli, Off };
enum class StutterBaseline { Mean, Median };
enum class OverlayBackend { Auto, Native, Rtss };  // 决策 ④=B 新增
enum class Severity { Info, Warning, Error };

// ── 值类型 ────────────────────────────────────────────────────────────────
struct ColorARGB {
    uint32_t v = 0;  // 0xAARRGGBB；0 表示"随主题"
    [[nodiscard]] bool isAuto() const noexcept { return v == 0; }
    [[nodiscard]] uint8_t a() const noexcept { return static_cast<uint8_t>(v >> 24); }
    [[nodiscard]] uint8_t r() const noexcept { return static_cast<uint8_t>(v >> 16); }
    [[nodiscard]] uint8_t g() const noexcept { return static_cast<uint8_t>(v >> 8); }
    [[nodiscard]] uint8_t b() const noexcept { return static_cast<uint8_t>(v); }
};

struct MetricEntry {
    std::string id;                 // 必须存在于指标目录（或可经别名解析）
    bool visible = true;
    std::string label;              // 空 = 目录默认名
    ColorARGB color{};              // 0 = 随主题
    float sizeScale = 1.0f;         // [0.5, 2.0]
    int precision = -1;             // -1 = 目录默认；0..3
};

struct LayoutConfig {
    Orientation orientation = Orientation::Horizontal;
    bool groupByCategory = true;
    bool showLabels = true;
    bool showUnits = true;
    int itemGapDp = 8;      // 0..64
    int groupGapDp = 14;    // 0..64
    Align align = Align::Start;
    bool wrap = false;
    int maxWidthDp = 960;   // 120..4096
    bool hideMissing = true;
};

struct ColorsConfig {
    ThemeId theme = ThemeId::Dark;
    ColorARGB background{0xCC101418};
    ColorARGB panelBorder{0x40FFFFFF};
    ColorARGB label{0xB3FFFFFF};
    ColorARGB value{0xFFFFFFFF};
    ColorARGB unit{0x99FFFFFF};
    ColorARGB ok{0xFF4CAF50};
    ColorARGB warn{0xFFFFB300};
    ColorARGB critical{0xFFE53935};
    ColorARGB accent{0xFF4FC3F7};
};

struct TextConfig {
    int baseSizeDp = 14;  // 8..48
    bool boldValue = true;
    std::string fontFamily;  // 空 = 系统 UI 字体
    int minSizeDp = 10;      // 6..24
};

struct WindowConfig {
    TopmostPolicy topmost = TopmostPolicy::Always;
    bool clickThrough = true;
    float opacity = 0.90f;  // 0.2..1.0
    float scale = 1.0f;     // 0.5..3.0
    PositionMode posMode = PositionMode::Preset;
    Corner corner = Corner::TopLeft;
    int x = 0;
    int y = 0;
    int marginDp = 12;  // 0..200
    std::string monitorId;  // 空 = 主显示器
    bool hideInFullscreen = false;
    bool showTrayIcon = true;
    bool startWithWindows = false;
    // window.overlay（决策 ④=B）
    OverlayBackend overlayBackend = OverlayBackend::Auto;
    int rtssUpdateMs = 500;  // 100..2000
};

struct GpuConfig {
    int selectedIndex = -1;  // -1 = 自动（渲染中的 GPU）；0..15 = 枚举下标
};

struct UpdateConfig {
    int sensorIntervalMs = 500;  // 200..5000
    int overlayRefreshFps = 2;   // 1..10
    int panelRefreshFps = 4;     // 1..30
    bool pauseWhenHidden = true;
    bool lowPowerOnBattery = false;
};

struct FpsConfig {
    FpsMode mode = FpsMode::Auto;
    std::string targetProcess;  // 空 = 跟随前台
    bool autoStartOnGame = true;
    int keepBufferFrames = 65536;  // 4096..262144
    float lowPercentile = 0.01f;   // 0.001..0.05
    float stutterFactor = 2.0f;    // 1.5..10.0
    StutterBaseline stutterBaseline = StutterBaseline::Mean;
    bool showPresentMode = true;
};

struct Config {
    int schemaVersion = 1;
    LayoutConfig layout;
    std::vector<MetricEntry> metrics;  // 顺序 = 展示顺序（单一事实来源）
    ColorsConfig colors;
    TextConfig text;
    WindowConfig window;
    UpdateConfig update;
    FpsConfig fps;
    GpuConfig gpu;
};

// ── 默认值 / 诊断 ─────────────────────────────────────────────────────────
Config MakeDefaultConfig();

struct Diagnostic {
    Severity sev = Severity::Info;
    std::string field;    // 配置路径，如 "layout.itemGapDp"
    std::string message;  // 人类可读
    std::string action;   // 我们做了什么（"已夹紧到 0" / "已丢弃" ...）
};

struct ValidationPolicy {
    bool clampNumbers = true;
    bool dropUnknownMetrics = true;
    bool appendMissingMetrics = true;
    bool preserveUnknownKeys = false;
};

// 返回 true 表示发生修复
bool ValidateAndRepair(Config& c, const ValidationPolicy& p, std::vector<Diagnostic>& diags);

// ── 路径 / 加载 / 保存 ────────────────────────────────────────────────────
struct ConfigPaths {
    std::filesystem::path file;
    bool portable = false;  // portable.flag 命中
    bool writable = true;   // false = 只读模式（无有效位置或文件不可写）
};

// 优先级：--config <path> > 环境变量 winperfmon_CONFIG > portable.flag 同目录
//         > %APPDATA%\winperfmon\config.json > 只读模式
ConfigPaths ResolveConfigPaths(int argc, const wchar_t* const* argv);

struct LoadResult {
    Config config;
    std::vector<Diagnostic> diagnostics;
    bool fileExisted = false;
    bool repaired = false;
    bool readonly = false;
    std::string error;  // 解析失败等原因（此时 config = 默认值，且绝不覆盖用户文件）
};

LoadResult LoadConfig(const ConfigPaths& paths);
bool SaveConfig(const Config& c, const ConfigPaths& paths, std::string* error);

// ── JSON 层（纯函数，可脱离 Windows API 单测）─────────────────────────────
std::string ConfigToJson(const Config& c);
LoadResult ConfigFromJson(std::string_view utf8, const ValidationPolicy& p = {});

// ── 枚举 ↔ 字符串（配置文件的稳定表示，改名即破坏兼容）──────────────────
std::string_view ToString(Orientation v);
std::string_view ToString(Align v);
std::string_view ToString(ThemeId v);
std::string_view ToString(TopmostPolicy v);
std::string_view ToString(PositionMode v);
std::string_view ToString(Corner v);
std::string_view ToString(FpsMode v);
std::string_view ToString(StutterBaseline v);
std::string_view ToString(OverlayBackend v);
bool ParseOrientation(std::string_view s, Orientation& out);
bool ParseAlign(std::string_view s, Align& out);
bool ParseThemeId(std::string_view s, ThemeId& out);
bool ParseTopmostPolicy(std::string_view s, TopmostPolicy& out);
bool ParsePositionMode(std::string_view s, PositionMode& out);
bool ParseCorner(std::string_view s, Corner& out);
bool ParseFpsMode(std::string_view s, FpsMode& out);
bool ParseStutterBaseline(std::string_view s, StutterBaseline& out);
bool ParseOverlayBackend(std::string_view s, OverlayBackend& out);

// ── 命令行/会话覆盖（不落盘）──────────────────────────────────────────────
bool ApplyCommandLineOverrides(Config& c, int argc, const wchar_t* const* argv,
                               std::vector<Diagnostic>& diags);

// ── 颜色工具（校验与 UI 共用）─────────────────────────────────────────────
bool ParseColor(std::string_view text, ColorARGB& out);   // "" = 随主题(v=0) / "#AARRGGBB" / "#RRGGBB"
std::string FormatColor(ColorARGB c);                     // 规范形式 "#AARRGGBB"
double ContrastRatio(ColorARGB a, ColorARGB b);           // WCAG 对比度

}  // namespace wpm::config
