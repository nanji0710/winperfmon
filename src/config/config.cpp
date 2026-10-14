// SPDX-License-Identifier: MIT
#include "config/config.h"

#include "config/catalog.h"
#include "core/json.h"
#include "core/text.h"
#include "winperfmon/version.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace wpm::config {
namespace {

// ── 取值辅助：JSON 里的错类型一律回退默认并记诊断（不抛）────────────────
bool readBool(const json::Value& o, const char* key, bool def, const std::string& prefix,
              std::vector<Diagnostic>& d) {
    const json::Value* v = o.find(key);
    if (!v) return def;
    if (!v->isBool()) {
        d.push_back({Severity::Warning, prefix + key, "类型应为布尔值", "已回退默认值"});
        return def;
    }
    return v->asBool();
}

int readInt(const json::Value& o, const char* key, int def, int lo, int hi,
            const std::string& prefix, std::vector<Diagnostic>& d,
            const ValidationPolicy& p) {
    const json::Value* v = o.find(key);
    if (!v) return def;
    if (!v->isNumber()) {
        d.push_back({Severity::Warning, prefix + key, "类型应为数字", "已回退默认值"});
        return def;
    }
    int iv = v->asInt(def);
    if (p.clampNumbers && (iv < lo || iv > hi)) {
        const int clamped = std::clamp(iv, lo, hi);
        d.push_back({Severity::Warning, prefix + key,
                     "数值 " + std::to_string(iv) + " 超出范围 [" + std::to_string(lo) + ", " +
                         std::to_string(hi) + "]",
                     "已夹紧到 " + std::to_string(clamped)});
        iv = clamped;
    }
    return iv;
}

float readFloat(const json::Value& o, const char* key, float def, float lo, float hi,
                const std::string& prefix, std::vector<Diagnostic>& d,
                const ValidationPolicy& p) {
    const json::Value* v = o.find(key);
    if (!v) return def;
    if (!v->isNumber()) {
        d.push_back({Severity::Warning, prefix + key, "类型应为数字", "已回退默认值"});
        return def;
    }
    float fv = static_cast<float>(v->asNumber(def));
    if (!std::isfinite(fv)) {
        d.push_back({Severity::Warning, prefix + key, "数值不是有限数", "已回退默认值"});
        return def;
    }
    if (p.clampNumbers && (fv < lo || fv > hi)) {
        const float clamped = std::clamp(fv, lo, hi);
        d.push_back({Severity::Warning, prefix + key, "数值超出允许范围", "已夹紧"});
        fv = clamped;
    }
    return fv;
}

std::string readString(const json::Value& o, const char* key, const std::string& def,
                       size_t maxBytes, const std::string& prefix,
                       std::vector<Diagnostic>& d) {
    const json::Value* v = o.find(key);
    if (!v) return def;
    if (!v->isString()) {
        d.push_back({Severity::Warning, prefix + key, "类型应为字符串", "已回退默认值"});
        return def;
    }
    std::string s = v->asString();
    if (s.size() > maxBytes) {
        d.push_back({Severity::Warning, prefix + key, "字符串过长", "已截断"});
        s.resize(maxBytes);
    }
    return s;
}

ColorARGB readColor(const json::Value& o, const char* key, ColorARGB def,
                    const std::string& prefix, std::vector<Diagnostic>& d) {
    const json::Value* v = o.find(key);
    if (!v) return def;
    if (v->isNull()) return def;
    if (!v->isString()) {
        d.push_back({Severity::Warning, prefix + key, "颜色应为 \"#AARRGGBB\" 字符串",
                     "已回退默认值"});
        return def;
    }
    ColorARGB out{};
    const std::string s = v->asString();
    if (s.empty()) return def;  // 空串 = 随主题
    if (!ParseColor(s, out)) {
        d.push_back({Severity::Warning, prefix + key, "颜色格式非法：" + s, "已回退默认值"});
        return def;
    }
    return out;
}

template <typename T, typename ParseFn>
T readEnum(const json::Value& o, const char* key, T def, ParseFn parse, const std::string& prefix,
           std::vector<Diagnostic>& d) {
    const json::Value* v = o.find(key);
    if (!v || !v->isString()) return def;
    T out{};
    if (!parse(v->asString(), out)) {
        d.push_back({Severity::Warning, prefix + key,
                     "未知取值 \"" + v->asString() + "\"", "已回退默认值"});
        return def;
    }
    return out;
}

json::Value colorToJson(ColorARGB c) {
    return c.isAuto() ? json::Value("") : json::Value(FormatColor(c));
}

}  // namespace

// ── 颜色 ─────────────────────────────────────────────────────────────────
bool ParseColor(std::string_view s, ColorARGB& out) {
    std::string_view t = text::trim(s);
    // 空串 = 随主题（ColorARGB::v == 0），见 config-schema.md §3.6/§6
    if (t.empty()) {
        out.v = 0;
        return true;
    }
    if (t[0] != '#') return false;
    t.remove_prefix(1);
    if (t.size() != 6 && t.size() != 8) return false;
    uint32_t v = 0;
    for (const char ch : t) {
        int h;
        if (ch >= '0' && ch <= '9') h = ch - '0';
        else if (ch >= 'a' && ch <= 'f') h = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') h = ch - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<uint32_t>(h);
    }
    out.v = (t.size() == 6) ? (0xFF000000u | v) : v;
    return true;
}

std::string FormatColor(ColorARGB c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%08X", c.v);
    return buf;
}

double ContrastRatio(ColorARGB a, ColorARGB b) {
    auto lin = [](uint8_t c) {
        const double s = static_cast<double>(c) / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    auto lum = [&](ColorARGB c) {
        return 0.2126 * lin(c.r()) + 0.7152 * lin(c.g()) + 0.0722 * lin(c.b());
    };
    const double l1 = lum(a), l2 = lum(b);
    const double hi = std::max(l1, l2), lo = std::min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

// ── 枚举字符串 ───────────────────────────────────────────────────────────
std::string_view ToString(Orientation v) {
    return v == Orientation::Vertical ? "vertical" : "horizontal";
}
std::string_view ToString(Align v) {
    switch (v) {
        case Align::Start: return "start";
        case Align::Center: return "center";
        case Align::End: return "end";
    }
    return "start";
}
std::string_view ToString(ThemeId v) {
    switch (v) {
        case ThemeId::Dark: return "dark";
        case ThemeId::Light: return "light";
        case ThemeId::Custom: return "custom";
    }
    return "dark";
}
std::string_view ToString(TopmostPolicy v) {
    switch (v) {
        case TopmostPolicy::Always: return "always";
        case TopmostPolicy::GameOnly: return "gameOnly";
        case TopmostPolicy::Never: return "never";
    }
    return "always";
}
std::string_view ToString(PositionMode v) {
    return v == PositionMode::Free ? "free" : "preset";
}
std::string_view ToString(Corner v) {
    switch (v) {
        case Corner::TopLeft: return "topLeft";
        case Corner::TopRight: return "topRight";
        case Corner::BottomLeft: return "bottomLeft";
        case Corner::BottomRight: return "bottomRight";
    }
    return "topLeft";
}
std::string_view ToString(FpsMode v) {
    switch (v) {
        case FpsMode::Auto: return "auto";
        case FpsMode::Service: return "service";
        case FpsMode::Cli: return "cli";
        case FpsMode::Off: return "off";
    }
    return "auto";
}
std::string_view ToString(StutterBaseline v) {
    return v == StutterBaseline::Median ? "median" : "mean";
}
std::string_view ToString(OverlayBackend v) {
    switch (v) {
        case OverlayBackend::Auto: return "auto";
        case OverlayBackend::Native: return "native";
        case OverlayBackend::Rtss: return "rtss";
    }
    return "auto";
}

bool ParseOrientation(std::string_view s, Orientation& o) {
    if (text::iequals(s, "horizontal")) { o = Orientation::Horizontal; return true; }
    if (text::iequals(s, "vertical")) { o = Orientation::Vertical; return true; }
    return false;
}
bool ParseAlign(std::string_view s, Align& o) {
    if (text::iequals(s, "start")) { o = Align::Start; return true; }
    if (text::iequals(s, "center")) { o = Align::Center; return true; }
    if (text::iequals(s, "end")) { o = Align::End; return true; }
    return false;
}
bool ParseThemeId(std::string_view s, ThemeId& o) {
    if (text::iequals(s, "dark")) { o = ThemeId::Dark; return true; }
    if (text::iequals(s, "light")) { o = ThemeId::Light; return true; }
    if (text::iequals(s, "custom")) { o = ThemeId::Custom; return true; }
    return false;
}
bool ParseTopmostPolicy(std::string_view s, TopmostPolicy& o) {
    if (text::iequals(s, "always")) { o = TopmostPolicy::Always; return true; }
    if (text::iequals(s, "gameOnly")) { o = TopmostPolicy::GameOnly; return true; }
    if (text::iequals(s, "never")) { o = TopmostPolicy::Never; return true; }
    return false;
}
bool ParsePositionMode(std::string_view s, PositionMode& o) {
    if (text::iequals(s, "preset")) { o = PositionMode::Preset; return true; }
    if (text::iequals(s, "free")) { o = PositionMode::Free; return true; }
    return false;
}
bool ParseCorner(std::string_view s, Corner& o) {
    if (text::iequals(s, "topLeft")) { o = Corner::TopLeft; return true; }
    if (text::iequals(s, "topRight")) { o = Corner::TopRight; return true; }
    if (text::iequals(s, "bottomLeft")) { o = Corner::BottomLeft; return true; }
    if (text::iequals(s, "bottomRight")) { o = Corner::BottomRight; return true; }
    return false;
}
bool ParseFpsMode(std::string_view s, FpsMode& o) {
    if (text::iequals(s, "auto")) { o = FpsMode::Auto; return true; }
    if (text::iequals(s, "service")) { o = FpsMode::Service; return true; }
    if (text::iequals(s, "cli")) { o = FpsMode::Cli; return true; }
    if (text::iequals(s, "off")) { o = FpsMode::Off; return true; }
    return false;
}
bool ParseStutterBaseline(std::string_view s, StutterBaseline& o) {
    if (text::iequals(s, "mean")) { o = StutterBaseline::Mean; return true; }
    if (text::iequals(s, "median")) { o = StutterBaseline::Median; return true; }
    return false;
}
bool ParseOverlayBackend(std::string_view s, OverlayBackend& o) {
    if (text::iequals(s, "auto")) { o = OverlayBackend::Auto; return true; }
    if (text::iequals(s, "native")) { o = OverlayBackend::Native; return true; }
    if (text::iequals(s, "rtss")) { o = OverlayBackend::Rtss; return true; }
    return false;
}

// ── 默认配置 ─────────────────────────────────────────────────────────────
Config MakeDefaultConfig() {
    Config c;
    c.schemaVersion = 1;
    // metrics：目录全量生成，visible 取目录默认（不变量 1：数组即顺序）
    for (const auto& m : MetricCatalog()) {
        MetricEntry e;
        e.id = std::string(m.id);
        e.visible = m.defaultVisible;
        c.metrics.push_back(std::move(e));
    }
    return c;
}

// ── 校验即修复 ───────────────────────────────────────────────────────────
bool ValidateAndRepair(Config& c, const ValidationPolicy& p, std::vector<Diagnostic>& diags) {
    const size_t before = diags.size();
    auto warn = [&](std::string field, std::string msg, std::string action) {
        diags.push_back({Severity::Warning, std::move(field), std::move(msg), std::move(action)});
    };
    auto info = [&](std::string field, std::string msg, std::string action) {
        diags.push_back({Severity::Info, std::move(field), std::move(msg), std::move(action)});
    };
    auto clampInt = [&](int& v, int lo, int hi, const char* field) {
        if (!p.clampNumbers) return;
        if (v < lo || v > hi) {
            const int nv = std::clamp(v, lo, hi);
            warn(field, "数值 " + std::to_string(v) + " 超出范围", "已夹紧到 " + std::to_string(nv));
            v = nv;
        }
    };
    auto clampFloat = [&](float& v, float lo, float hi, const char* field) {
        if (!p.clampNumbers) return;
        if (!std::isfinite(v) || v < lo || v > hi) {
            const float nv = std::isfinite(v) ? std::clamp(v, lo, hi) : lo;
            warn(field, "数值超出范围", "已夹紧");
            v = nv;
        }
    };

    // ── layout ──
    clampInt(c.layout.itemGapDp, 0, 64, "layout.itemGapDp");
    clampInt(c.layout.groupGapDp, 0, 64, "layout.groupGapDp");
    clampInt(c.layout.maxWidthDp, 120, 4096, "layout.maxWidthDp");

    // ── colors：value/background 对比度过低给可读性警告 ──
    if (!c.colors.value.isAuto() && !c.colors.background.isAuto()) {
        const double ratio = ContrastRatio(c.colors.value, c.colors.background);
        if (ratio < 3.0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "对比度 %.1f:1 低于 3:1", ratio);
            warn("colors.value", buf, "已保留用户取值，建议调亮数值颜色");
        }
    }

    // ── text ──
    clampInt(c.text.baseSizeDp, 8, 48, "text.baseSizeDp");
    clampInt(c.text.minSizeDp, 6, 24, "text.minSizeDp");
    if (c.text.minSizeDp > c.text.baseSizeDp) {
        c.text.minSizeDp = c.text.baseSizeDp;
        warn("text.minSizeDp", "字号下限大于基准字号", "已下调到基准字号");
    }
    if (c.text.fontFamily.size() > 256) {
        c.text.fontFamily.resize(256);
        warn("text.fontFamily", "字体名过长", "已截断");
    }

    // ── window ──
    clampFloat(c.window.opacity, 0.2f, 1.0f, "window.opacity");
    clampFloat(c.window.scale, 0.5f, 3.0f, "window.scale");
    clampInt(c.window.marginDp, 0, 200, "window.marginDp");
    clampInt(c.window.rtssUpdateMs, 100, 2000, "window.overlay.rtssUpdateMs");

    // ── update ──
    clampInt(c.update.sensorIntervalMs, 200, 5000, "update.sensorIntervalMs");
    clampInt(c.update.overlayRefreshFps, 1, 10, "update.overlayRefreshFps");
    clampInt(c.update.panelRefreshFps, 1, 30, "update.panelRefreshFps");

    // ── fps ──
    clampInt(c.fps.keepBufferFrames, 4096, 262144, "fps.keepBufferFrames");
    clampFloat(c.fps.lowPercentile, 0.001f, 0.05f, "fps.lowPercentile");
    clampFloat(c.fps.stutterFactor, 1.5f, 10.0f, "fps.stutterFactor");
    if (c.fps.targetProcess.size() > 260) {
        c.fps.targetProcess.resize(260);
        warn("fps.targetProcess", "进程名过长", "已截断");
    }

    // ── gpu（config-schema.md §3.6.1）──
    if (c.gpu.selectedIndex < -1 || c.gpu.selectedIndex > 15) {
        warn("gpu.selectedIndex",
             "下标 " + std::to_string(c.gpu.selectedIndex) + " 超出 [-1, 15]",
             "已回退 -1（自动）");
        c.gpu.selectedIndex = -1;
    }

    // ── metrics：去重、未知、规范大小写、补齐、至少一项可见 ──
    std::vector<MetricEntry> kept;
    kept.reserve(c.metrics.size());
    std::vector<std::string> seen;
    for (auto& e : c.metrics) {
        std::string norm = text::toLowerAscii(text::trim(e.id));
        const MetricInfo* mi = FindMetric(norm);
        if (!mi) {
            // 别名解析（config-schema.md §8）
            const MetricInfo* aliased = ResolveMetricAlias(norm);
            if (aliased) {
                info("metrics[].id", "旧指标 id \"" + e.id + "\"", std::string("已迁移为 \"") +
                         std::string(aliased->id) + "\"");
                norm = std::string(aliased->id);
                mi = aliased;
            }
        }
        if (!mi) {
            if (p.dropUnknownMetrics) {
                warn("metrics[].id", "未知指标 id \"" + e.id + "\"", "已丢弃");
                continue;
            }
            kept.push_back(std::move(e));
            continue;
        }
        if (norm != e.id) {
            info("metrics[].id", "id 大小写/空白不规范 \"" + e.id + "\"",
                 "已改写为 \"" + norm + "\"");
            e.id = norm;
        }
        if (std::find(seen.begin(), seen.end(), e.id) != seen.end()) {
            warn("metrics[].id", "重复指标 id \"" + e.id + "\"", "已保留首次出现，丢弃重复项");
            continue;
        }
        seen.push_back(e.id);
        clampFloat(e.sizeScale, 0.5f, 2.0f, "metrics[].sizeScale");
        if (e.precision != -1) clampInt(e.precision, -1, 3, "metrics[].precision");
        if (e.label.size() > 24 * 4) {  // 24 字符上限，按字节粗判后按码点截断
            e.label = text::utf8Truncate(e.label, 24);
            warn("metrics[].label", "标签超过 24 个字符", "已截断");
        }
        kept.push_back(std::move(e));
    }
    c.metrics = std::move(kept);

    if (c.metrics.empty() && p.appendMissingMetrics) {
        info("metrics", "指标数组为空", "已按目录默认重建");
        c.metrics.clear();
        for (const auto& m : MetricCatalog()) {
            MetricEntry e;
            e.id = std::string(m.id);
            e.visible = m.defaultVisible;
            c.metrics.push_back(std::move(e));
        }
    } else if (p.appendMissingMetrics) {
        // 目录有而数组缺 → 追加末尾，默认隐藏（Info）
        size_t appended = 0;
        for (const auto& m : MetricCatalog()) {
            const bool present =
                std::any_of(c.metrics.begin(), c.metrics.end(),
                            [&](const MetricEntry& e) { return e.id == m.id; });
            if (!present) {
                MetricEntry e;
                e.id = std::string(m.id);
                e.visible = false;  // 新增项默认隐藏，避免升级后布局突然变长
                c.metrics.push_back(std::move(e));
                ++appended;
            }
        }
        if (appended > 0)
            info("metrics", "新增 " + std::to_string(appended) + " 项目录指标", "已追加到末尾并默认隐藏");
    }

    if (!c.metrics.empty() &&
        std::none_of(c.metrics.begin(), c.metrics.end(),
                     [](const MetricEntry& e) { return e.visible; })) {
        auto first = std::find_if(c.metrics.begin(), c.metrics.end(), [](const MetricEntry& e) {
            const MetricInfo* mi = FindMetric(e.id);
            return mi && mi->defaultVisible;
        });
        if (first == c.metrics.end()) first = c.metrics.begin();
        first->visible = true;
        warn("metrics", "所有指标都被隐藏", "已强制显示 \"" + first->id + "\"");
    }

    return diags.size() != before;
}

// ── 序列化 ───────────────────────────────────────────────────────────────
std::string ConfigToJson(const Config& c) {
    using json::Value;
    Value root = Value::makeObject();
    root.set("schemaVersion", Value(c.schemaVersion));

    Value meta = Value::makeObject();
    meta.set("writtenBy", Value(WPM_WRITTEN_BY));
    {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth,
                      st.wDay, st.wHour, st.wMinute, st.wSecond);
        meta.set("lastSavedUtc", Value(std::string(buf)));
    }
    root.set("meta", std::move(meta));

    Value layout = Value::makeObject();
    layout.set("orientation", Value(std::string(ToString(c.layout.orientation))));
    layout.set("groupByCategory", Value(c.layout.groupByCategory));
    layout.set("showLabels", Value(c.layout.showLabels));
    layout.set("showUnits", Value(c.layout.showUnits));
    layout.set("itemGapDp", Value(c.layout.itemGapDp));
    layout.set("groupGapDp", Value(c.layout.groupGapDp));
    layout.set("align", Value(std::string(ToString(c.layout.align))));
    layout.set("wrap", Value(c.layout.wrap));
    layout.set("maxWidthDp", Value(c.layout.maxWidthDp));
    layout.set("hideMissing", Value(c.layout.hideMissing));
    root.set("layout", std::move(layout));

    Value arr = Value::makeArray();
    for (const auto& e : c.metrics) {
        Value item = Value::makeObject();
        item.set("id", Value(e.id));
        item.set("visible", Value(e.visible));
        item.set("label", Value(e.label));
        item.set("color", colorToJson(e.color));
        // sizeScale 用 3 位小数，保证 1.0 这类默认值写出可读形式
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(e.sizeScale));
        item.set("sizeScale", Value(std::strtod(buf, nullptr)));
        item.set("precision", Value(e.precision));
        arr.asArray().push_back(std::move(item));
    }
    root.set("metrics", std::move(arr));

    Value colors = Value::makeObject();
    colors.set("themeId", Value(std::string(ToString(c.colors.theme))));
    colors.set("background", colorToJson(c.colors.background));
    colors.set("panelBorder", colorToJson(c.colors.panelBorder));
    colors.set("label", colorToJson(c.colors.label));
    colors.set("value", colorToJson(c.colors.value));
    colors.set("unit", colorToJson(c.colors.unit));
    colors.set("ok", colorToJson(c.colors.ok));
    colors.set("warn", colorToJson(c.colors.warn));
    colors.set("critical", colorToJson(c.colors.critical));
    colors.set("accent", colorToJson(c.colors.accent));
    root.set("colors", std::move(colors));

    Value text_ = Value::makeObject();
    text_.set("baseSizeDp", Value(c.text.baseSizeDp));
    text_.set("boldValue", Value(c.text.boldValue));
    text_.set("fontFamily", Value(c.text.fontFamily));
    text_.set("minSizeDp", Value(c.text.minSizeDp));
    root.set("text", std::move(text_));

    Value win = Value::makeObject();
    win.set("topmostPolicy", Value(std::string(ToString(c.window.topmost))));
    win.set("clickThrough", Value(c.window.clickThrough));
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(c.window.opacity));
        win.set("opacity", Value(std::strtod(buf, nullptr)));
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(c.window.scale));
        win.set("scale", Value(std::strtod(buf, nullptr)));
    }
    Value pos = Value::makeObject();
    pos.set("mode", Value(std::string(ToString(c.window.posMode))));
    pos.set("preset", Value(std::string(ToString(c.window.corner))));
    pos.set("x", Value(c.window.x));
    pos.set("y", Value(c.window.y));
    win.set("position", std::move(pos));
    win.set("marginDp", Value(c.window.marginDp));
    win.set("monitorId", Value(c.window.monitorId));
    win.set("hideInFullscreen", Value(c.window.hideInFullscreen));
    win.set("showTrayIcon", Value(c.window.showTrayIcon));
    win.set("startWithWindows", Value(c.window.startWithWindows));
    Value ov = Value::makeObject();
    ov.set("backend", Value(std::string(ToString(c.window.overlayBackend))));
    ov.set("rtssUpdateMs", Value(c.window.rtssUpdateMs));
    win.set("overlay", std::move(ov));
    root.set("window", std::move(win));

    Value upd = Value::makeObject();
    upd.set("sensorIntervalMs", Value(c.update.sensorIntervalMs));
    upd.set("overlayRefreshFps", Value(c.update.overlayRefreshFps));
    upd.set("panelRefreshFps", Value(c.update.panelRefreshFps));
    upd.set("pauseWhenHidden", Value(c.update.pauseWhenHidden));
    upd.set("lowPowerOnBattery", Value(c.update.lowPowerOnBattery));
    root.set("update", std::move(upd));

    Value fps = Value::makeObject();
    fps.set("mode", Value(std::string(ToString(c.fps.mode))));
    fps.set("targetProcess", Value(c.fps.targetProcess));
    fps.set("autoStartOnGame", Value(c.fps.autoStartOnGame));
    fps.set("keepBufferFrames", Value(c.fps.keepBufferFrames));
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(c.fps.lowPercentile));
        fps.set("lowPercentile", Value(std::strtod(buf, nullptr)));
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(c.fps.stutterFactor));
        fps.set("stutterFactor", Value(std::strtod(buf, nullptr)));
    }
    fps.set("stutterBaseline", Value(std::string(ToString(c.fps.stutterBaseline))));
    fps.set("showPresentMode", Value(c.fps.showPresentMode));
    root.set("fps", std::move(fps));

    Value gpu = Value::makeObject();
    gpu.set("selectedIndex", Value(c.gpu.selectedIndex));
    root.set("gpu", std::move(gpu));

    return json::stringify(root, 2) + "\n";
}

// ── 反序列化 ─────────────────────────────────────────────────────────────
LoadResult ConfigFromJson(std::string_view utf8, const ValidationPolicy& p) {
    LoadResult r;
    r.config = MakeDefaultConfig();

    json::Value root;
    json::ParseError perr;
    if (!json::parse(utf8, root, perr)) {
        r.error = "解析失败（第 " + std::to_string(perr.line) + " 行，偏移 " +
                  std::to_string(perr.offset) + "）：" + perr.message;
        r.diagnostics.push_back({Severity::Error, "", r.error, "已使用默认配置（未覆盖原文件）"});
        r.repaired = true;
        return r;
    }
    if (!root.isObject()) {
        r.error = "配置根节点必须是对象";
        r.diagnostics.push_back({Severity::Error, "", r.error, "已使用默认配置（未覆盖原文件）"});
        r.repaired = true;
        return r;
    }

    // 版本检查：高于本程序已知版本 → 只读，不覆盖（前向兼容，不变量 2/3）
    if (const json::Value* v = root.find("schemaVersion"); v && v->isNumber()) {
        const int sv = v->asInt(1);
        if (sv > 1) {
            r.readonly = true;
            r.diagnostics.push_back({Severity::Warning, "schemaVersion",
                                     "配置版本 " + std::to_string(sv) + " 高于本程序支持的 1",
                                     "已按只读模式加载，不会覆盖该文件"});
        }
        r.config.schemaVersion = sv;
    }

    Config c;
    if (const json::Value* l = root.find("layout"); l && l->isObject()) {
        c.layout.orientation =
            readEnum(*l, "orientation", c.layout.orientation, ParseOrientation, "layout.", r.diagnostics);
        c.layout.groupByCategory = readBool(*l, "groupByCategory", c.layout.groupByCategory, "layout.", r.diagnostics);
        c.layout.showLabels = readBool(*l, "showLabels", c.layout.showLabels, "layout.", r.diagnostics);
        c.layout.showUnits = readBool(*l, "showUnits", c.layout.showUnits, "layout.", r.diagnostics);
        c.layout.itemGapDp = readInt(*l, "itemGapDp", c.layout.itemGapDp, 0, 64, "layout.", r.diagnostics, p);
        c.layout.groupGapDp = readInt(*l, "groupGapDp", c.layout.groupGapDp, 0, 64, "layout.", r.diagnostics, p);
        c.layout.align = readEnum(*l, "align", c.layout.align, ParseAlign, "layout.", r.diagnostics);
        c.layout.wrap = readBool(*l, "wrap", c.layout.wrap, "layout.", r.diagnostics);
        c.layout.maxWidthDp = readInt(*l, "maxWidthDp", c.layout.maxWidthDp, 120, 4096, "layout.", r.diagnostics, p);
        c.layout.hideMissing = readBool(*l, "hideMissing", c.layout.hideMissing, "layout.", r.diagnostics);
    }

    if (const json::Value* m = root.find("metrics"); m && m->isArray()) {
        c.metrics.clear();
        size_t idx = 0;
        for (const auto& item : m->asArray()) {
            if (!item.isObject()) {
                r.diagnostics.push_back({Severity::Warning, "metrics[" + std::to_string(idx) + "]",
                                          "数组元素不是对象", "已丢弃"});
                ++idx;
                continue;
            }
            MetricEntry e;
            e.id = readString(item, "id", "", 256, "metrics[].", r.diagnostics);
            e.visible = readBool(item, "visible", true, "metrics[].", r.diagnostics);
            e.label = readString(item, "label", "", 256, "metrics[].", r.diagnostics);
            e.color = readColor(item, "color", ColorARGB{}, "metrics[].", r.diagnostics);
            e.sizeScale = readFloat(item, "sizeScale", 1.0f, 0.5f, 2.0f, "metrics[].", r.diagnostics, p);
            e.precision = readInt(item, "precision", -1, -1, 3, "metrics[].", r.diagnostics, p);
            c.metrics.push_back(std::move(e));
            ++idx;
        }
    }

    if (const json::Value* col = root.find("colors"); col && col->isObject()) {
        c.colors.theme = readEnum(*col, "themeId", c.colors.theme, ParseThemeId, "colors.", r.diagnostics);
        c.colors.background = readColor(*col, "background", c.colors.background, "colors.", r.diagnostics);
        c.colors.panelBorder = readColor(*col, "panelBorder", c.colors.panelBorder, "colors.", r.diagnostics);
        c.colors.label = readColor(*col, "label", c.colors.label, "colors.", r.diagnostics);
        c.colors.value = readColor(*col, "value", c.colors.value, "colors.", r.diagnostics);
        c.colors.unit = readColor(*col, "unit", c.colors.unit, "colors.", r.diagnostics);
        c.colors.ok = readColor(*col, "ok", c.colors.ok, "colors.", r.diagnostics);
        c.colors.warn = readColor(*col, "warn", c.colors.warn, "colors.", r.diagnostics);
        c.colors.critical = readColor(*col, "critical", c.colors.critical, "colors.", r.diagnostics);
        c.colors.accent = readColor(*col, "accent", c.colors.accent, "colors.", r.diagnostics);
    }

    if (const json::Value* t = root.find("text"); t && t->isObject()) {
        c.text.baseSizeDp = readInt(*t, "baseSizeDp", c.text.baseSizeDp, 8, 48, "text.", r.diagnostics, p);
        c.text.boldValue = readBool(*t, "boldValue", c.text.boldValue, "text.", r.diagnostics);
        c.text.fontFamily = readString(*t, "fontFamily", "", 256, "text.", r.diagnostics);
        c.text.minSizeDp = readInt(*t, "minSizeDp", c.text.minSizeDp, 6, 24, "text.", r.diagnostics, p);
    }

    if (const json::Value* w = root.find("window"); w && w->isObject()) {
        c.window.topmost = readEnum(*w, "topmostPolicy", c.window.topmost, ParseTopmostPolicy, "window.", r.diagnostics);
        c.window.clickThrough = readBool(*w, "clickThrough", c.window.clickThrough, "window.", r.diagnostics);
        c.window.opacity = readFloat(*w, "opacity", c.window.opacity, 0.2f, 1.0f, "window.", r.diagnostics, p);
        c.window.scale = readFloat(*w, "scale", c.window.scale, 0.5f, 3.0f, "window.", r.diagnostics, p);
        if (const json::Value* pos = w->find("position"); pos && pos->isObject()) {
            c.window.posMode = readEnum(*pos, "mode", c.window.posMode, ParsePositionMode, "window.position.", r.diagnostics);
            c.window.corner = readEnum(*pos, "preset", c.window.corner, ParseCorner, "window.position.", r.diagnostics);
            c.window.x = readInt(*pos, "x", c.window.x, -100000, 100000, "window.position.", r.diagnostics, p);
            c.window.y = readInt(*pos, "y", c.window.y, -100000, 100000, "window.position.", r.diagnostics, p);
        }
        c.window.marginDp = readInt(*w, "marginDp", c.window.marginDp, 0, 200, "window.", r.diagnostics, p);
        c.window.monitorId = readString(*w, "monitorId", "", 256, "window.", r.diagnostics);
        c.window.hideInFullscreen = readBool(*w, "hideInFullscreen", c.window.hideInFullscreen, "window.", r.diagnostics);
        c.window.showTrayIcon = readBool(*w, "showTrayIcon", c.window.showTrayIcon, "window.", r.diagnostics);
        c.window.startWithWindows = readBool(*w, "startWithWindows", c.window.startWithWindows, "window.", r.diagnostics);
        if (const json::Value* ov = w->find("overlay"); ov && ov->isObject()) {
            c.window.overlayBackend = readEnum(*ov, "backend", c.window.overlayBackend, ParseOverlayBackend, "window.overlay.", r.diagnostics);
            c.window.rtssUpdateMs = readInt(*ov, "rtssUpdateMs", c.window.rtssUpdateMs, 100, 2000, "window.overlay.", r.diagnostics, p);
        }
    }

    if (const json::Value* u = root.find("update"); u && u->isObject()) {
        c.update.sensorIntervalMs = readInt(*u, "sensorIntervalMs", c.update.sensorIntervalMs, 200, 5000, "update.", r.diagnostics, p);
        c.update.overlayRefreshFps = readInt(*u, "overlayRefreshFps", c.update.overlayRefreshFps, 1, 10, "update.", r.diagnostics, p);
        c.update.panelRefreshFps = readInt(*u, "panelRefreshFps", c.update.panelRefreshFps, 1, 30, "update.", r.diagnostics, p);
        c.update.pauseWhenHidden = readBool(*u, "pauseWhenHidden", c.update.pauseWhenHidden, "update.", r.diagnostics);
        c.update.lowPowerOnBattery = readBool(*u, "lowPowerOnBattery", c.update.lowPowerOnBattery, "update.", r.diagnostics);
    }

    if (const json::Value* f = root.find("fps"); f && f->isObject()) {
        c.fps.mode = readEnum(*f, "mode", c.fps.mode, ParseFpsMode, "fps.", r.diagnostics);
        c.fps.targetProcess = readString(*f, "targetProcess", "", 260, "fps.", r.diagnostics);
        c.fps.autoStartOnGame = readBool(*f, "autoStartOnGame", c.fps.autoStartOnGame, "fps.", r.diagnostics);
        c.fps.keepBufferFrames = readInt(*f, "keepBufferFrames", c.fps.keepBufferFrames, 4096, 262144, "fps.", r.diagnostics, p);
        c.fps.lowPercentile = readFloat(*f, "lowPercentile", c.fps.lowPercentile, 0.001f, 0.05f, "fps.", r.diagnostics, p);
        c.fps.stutterFactor = readFloat(*f, "stutterFactor", c.fps.stutterFactor, 1.5f, 10.0f, "fps.", r.diagnostics, p);
        c.fps.stutterBaseline = readEnum(*f, "stutterBaseline", c.fps.stutterBaseline, ParseStutterBaseline, "fps.", r.diagnostics);
        c.fps.showPresentMode = readBool(*f, "showPresentMode", c.fps.showPresentMode, "fps.", r.diagnostics);
    }

    if (const json::Value* g = root.find("gpu"); g && g->isObject()) {
        c.gpu.selectedIndex = readInt(*g, "selectedIndex", c.gpu.selectedIndex, -1, 15, "gpu.", r.diagnostics, p);
    }

    const bool validated = ValidateAndRepair(c, p, r.diagnostics);
    r.repaired = validated;
    // 纯 JSON 入口没有文件句柄：约定「解析成功 = 有一份可用的配置文本」
    r.fileExisted = true;
    r.config = std::move(c);
    return r;
}

// ── 原子写 ───────────────────────────────────────────────────────────────
namespace {

bool WriteFileAtomic(const std::filesystem::path& target, std::string_view utf8,
                     std::string* error) {
    const std::filesystem::path tmp = target.wstring() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = "无法创建临时文件（错误 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    const char* p = utf8.data();
    size_t remaining = utf8.size();
    while (remaining > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(remaining > 1u << 20 ? 1u << 20 : remaining);
        if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0) {
            if (error) *error = "写入失败（错误 " + std::to_string(GetLastError()) + "）";
            CloseHandle(h);
            DeleteFileW(tmp.c_str());
            return false;
        }
        p += written;
        remaining -= written;
    }
    // 先落盘再替换，避免掉电后出现半截文件
    if (!FlushFileBuffers(h)) {
        if (error) *error = "FlushFileBuffers 失败";
        CloseHandle(h);
        DeleteFileW(tmp.c_str());
        return false;
    }
    CloseHandle(h);

    // 旧文件轮转为 .bak（保留 1 代）
    const std::filesystem::path bak = target.wstring() + L".bak";
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        std::filesystem::copy_file(target, bak,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = "MoveFileExW 失败（错误 " + std::to_string(GetLastError()) + "）";
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace

bool SaveConfig(const Config& c, const ConfigPaths& paths, std::string* error) {
    if (!paths.writable) {
        if (error) *error = "配置为只读模式，未写入";
        return false;
    }
    std::error_code ec;
    const auto parent = paths.file.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) *error = "无法创建配置目录：" + ec.message();
            return false;
        }
    }
    return WriteFileAtomic(paths.file, ConfigToJson(c), error);
}

// ── 路径解析 ─────────────────────────────────────────────────────────────
namespace {

std::filesystem::path GetExePath() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return {};
    return std::filesystem::path(std::wstring(buf, n));
}

}  // namespace

ConfigPaths ResolveConfigPaths(int argc, const wchar_t* const* argv) {
    ConfigPaths out;
    // 1) --config <path>
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--config") == 0) {
            out.file = argv[i + 1];
            out.writable = true;
            return out;
        }
    }
    // 2) 环境变量 winperfmon_CONFIG
    wchar_t envBuf[32768]{};
    const DWORD envLen = GetEnvironmentVariableW(L"winperfmon_CONFIG", envBuf,
                                                 static_cast<DWORD>(std::size(envBuf)));
    if (envLen > 0 && envLen < std::size(envBuf)) {
        out.file = envBuf;
        out.writable = true;
        return out;
    }
    const std::filesystem::path exeDir = GetExePath().parent_path();
    std::error_code ec;
    // 3) 便携模式：exe 同目录存在 portable.flag
    if (!exeDir.empty() && std::filesystem::exists(exeDir / L"portable.flag", ec)) {
        out.file = exeDir / L"config.json";
        out.portable = true;
        out.writable = true;
        return out;
    }
    // 4) %APPDATA%\winperfmon\config.json
    wchar_t appData[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData,
                                            static_cast<DWORD>(std::size(appData)));
    if (n > 0 && n < std::size(appData)) {
        out.file = std::filesystem::path(appData) / L"winperfmon" / L"config.json";
        out.writable = true;
        return out;
    }
    // 5) 只读模式
    out.file.clear();
    out.writable = false;
    return out;
}

LoadResult LoadConfig(const ConfigPaths& paths) {
    LoadResult r;
    r.config = MakeDefaultConfig();
    r.readonly = !paths.writable;
    if (paths.file.empty()) {
        r.diagnostics.push_back({Severity::Info, "", "未找到可用的配置位置",
                                 "已使用默认配置（只读模式）"});
        return r;
    }
    std::error_code ec;
    if (!std::filesystem::exists(paths.file, ec)) {
        r.fileExisted = false;
        r.diagnostics.push_back({Severity::Info, "config", "首次运行，配置文件不存在",
                                 std::string("已使用默认配置，将写入 ") +
                                     text::toUtf8(paths.file.wstring())});
        return r;
    }
    r.fileExisted = true;

    HANDLE h = CreateFileW(paths.file.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.error = "无法读取配置文件（错误 " + std::to_string(GetLastError()) + "）";
        r.diagnostics.push_back({Severity::Error, "", r.error, "已使用默认配置（未覆盖原文件）"});
        return r;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > 256 * 1024) {
        CloseHandle(h);
        r.error = "配置文件超过 256 KB 上限";
        r.diagnostics.push_back({Severity::Error, "", r.error, "已使用默认配置（未覆盖原文件）"});
        return r;
    }
    std::string buf(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (size.QuadPart > 0) {
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) {
            CloseHandle(h);
            r.error = "读取配置文件失败";
            r.diagnostics.push_back({Severity::Error, "", r.error, "已使用默认配置（未覆盖原文件）"});
            return r;
        }
        buf.resize(read);
    }
    CloseHandle(h);

    LoadResult parsed = ConfigFromJson(buf);
    // 保留路径级信息
    parsed.fileExisted = true;
    if (r.readonly) parsed.readonly = true;
    if (parsed.error.empty() && r.readonly) {
        parsed.diagnostics.push_back({Severity::Warning, "", "配置位置不可写",
                                      "改动仅作用于本次运行，不会落盘"});
    }
    return parsed;
}

// ── 命令行覆盖（不落盘）──────────────────────────────────────────────────
bool ApplyCommandLineOverrides(Config& c, int argc, const wchar_t* const* argv,
                               std::vector<Diagnostic>& diags) {
    const size_t before = diags.size();
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        auto next = [&](std::wstring& out) {
            if (i + 1 < argc) {
                out = argv[++i];
                return true;
            }
            return false;
        };
        std::wstring val;
        if (a == L"--interval" && next(val)) {
            c.update.sensorIntervalMs = std::clamp(_wtoi(val.c_str()), 200, 5000);
            diags.push_back({Severity::Info, "update.sensorIntervalMs", "命令行覆盖",
                             text::toUtf8(val)});
        } else if (a == L"--vertical") {
            c.layout.orientation = Orientation::Vertical;
            diags.push_back({Severity::Info, "layout.orientation", "命令行覆盖", "vertical"});
        } else if (a == L"--horizontal") {
            c.layout.orientation = Orientation::Horizontal;
            diags.push_back({Severity::Info, "layout.orientation", "命令行覆盖", "horizontal"});
        } else if (a == L"--gpu" && next(val)) {
            c.gpu.selectedIndex = std::clamp(_wtoi(val.c_str()), -1, 15);
            diags.push_back({Severity::Info, "gpu.selectedIndex", "命令行覆盖",
                             std::to_string(c.gpu.selectedIndex)});
        }
    }
    return diags.size() != before;
}

}  // namespace wpm::config
