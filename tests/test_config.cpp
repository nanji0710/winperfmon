// SPDX-License-Identifier: MIT
// 配置契约单测：默认值、校验即修复、别名、往返、迁移（config-schema.md §0/§3/§5/§8）。
// 这些用例不碰 Windows API，可在 CI（windows-latest 或任意平台）上空跑。
#include "config/config.h"
#include "config/catalog.h"
#include "core/text.h"

#include "test.h"

#include <string>

using namespace wpm;
using namespace wpm::config;

namespace {

bool HasDiagField(const std::vector<Diagnostic>& d, const std::string& field) {
    for (const auto& x : d)
        if (x.field == field) return true;
    return false;
}

}  // namespace

WPM_TEST(config_default_is_valid_and_stable) {
    Config c = MakeDefaultConfig();
    std::vector<Diagnostic> d;
    const bool repaired = ValidateAndRepair(c, ValidationPolicy{}, d);
    WPM_CHECK(!repaired);  // 默认值本身必须已经合规，否则每次首启都会"修复"
    WPM_CHECK(d.empty());

    // 默认可见项应为文档 config-schema.md 4 表格 ✅ 列定义的 13 项
    int visible = 0;
    for (const auto& m : c.metrics)
        if (m.visible) ++visible;
    WPM_CHECK_EQ(visible, 13);
}

WPM_TEST(config_default_includes_whole_catalog) {
    Config c = MakeDefaultConfig();
    WPM_CHECK_EQ(c.metrics.size(), MetricCatalog().size());
    // 顺序必须与目录一致（首次生成按目录顺序）
    for (size_t i = 0; i < c.metrics.size(); ++i) {
        WPM_CHECK_EQ(c.metrics[i].id, std::string(MetricCatalog()[i].id));
    }
}

WPM_TEST(config_clamps_out_of_range) {
    Config c = MakeDefaultConfig();
    c.layout.itemGapDp = 9999;
    c.layout.maxWidthDp = 1;
    c.text.baseSizeDp = 200;
    c.window.opacity = 5.0f;
    c.update.sensorIntervalMs = 1;
    c.fps.keepBufferFrames = 10;
    c.fps.stutterFactor = 100.0f;

    std::vector<Diagnostic> d;
    WPM_CHECK(ValidateAndRepair(c, ValidationPolicy{}, d));
    WPM_CHECK_EQ(c.layout.itemGapDp, 64);
    WPM_CHECK_EQ(c.layout.maxWidthDp, 120);
    WPM_CHECK_EQ(c.text.baseSizeDp, 48);
    WPM_CHECK_NEAR(c.window.opacity, 1.0, 1e-6);
    WPM_CHECK_EQ(c.update.sensorIntervalMs, 200);
    WPM_CHECK_EQ(c.fps.keepBufferFrames, 4096);
    WPM_CHECK_NEAR(c.fps.stutterFactor, 10.0, 1e-6);
    WPM_CHECK(HasDiagField(d, "layout.itemGapDp"));
}

WPM_TEST(config_drops_unknown_and_appends_missing) {
    Config c = MakeDefaultConfig();
    c.metrics.clear();
    c.metrics.push_back(MetricEntry{.id = "not.a.metric", .visible = true});
    c.metrics.push_back(MetricEntry{.id = "cpu.usage", .visible = true});

    std::vector<Diagnostic> d;
    WPM_CHECK(ValidateAndRepair(c, ValidationPolicy{}, d));
    // 未知项被丢弃
    for (const auto& m : c.metrics) WPM_CHECK(m.id != "not.a.metric");
    // 目录里缺的项被追加回来
    WPM_CHECK_EQ(c.metrics.size(), MetricCatalog().size());
    WPM_CHECK(HasDiagField(d, "metrics"));
}

WPM_TEST(config_resolves_alias_and_canonical_case) {
    Config c = MakeDefaultConfig();
    c.metrics.clear();
    MetricEntry e;
    e.id = "CPU.FREQ";  // 别名 + 大小写都需规范
    e.visible = true;
    c.metrics.push_back(e);

    std::vector<Diagnostic> d;
    ValidateAndRepair(c, ValidationPolicy{}, d);
    bool found = false;
    for (const auto& m : c.metrics) {
        if (m.id == "cpu.freq.avg") found = true;
    }
    WPM_CHECK(found);
}

WPM_TEST(config_dedupes_keeping_first) {
    Config c = MakeDefaultConfig();
    c.metrics.clear();
    for (int i = 0; i < 3; ++i) {
        MetricEntry e;
        e.id = "cpu.usage";
        e.visible = (i == 0);
        c.metrics.push_back(e);
    }
    std::vector<Diagnostic> d;
    ValidateAndRepair(c, ValidationPolicy{}, d);
    int count = 0;
    for (const auto& m : c.metrics)
        if (m.id == "cpu.usage") ++count;
    WPM_CHECK_EQ(count, 1);
}

WPM_TEST(config_forces_at_least_one_visible) {
    Config c = MakeDefaultConfig();
    for (auto& m : c.metrics) m.visible = false;
    std::vector<Diagnostic> d;
    ValidateAndRepair(c, ValidationPolicy{}, d);
    int visible = 0;
    for (const auto& m : c.metrics)
        if (m.visible) ++visible;
    WPM_CHECK_EQ(visible, 1);
    WPM_CHECK(HasDiagField(d, "metrics"));
}

WPM_TEST(config_clamps_metric_entry_fields) {
    Config c = MakeDefaultConfig();
    c.metrics[0].sizeScale = 99.0f;
    c.metrics[0].precision = 99;
    c.metrics[0].label = std::string(100, 'x');

    std::vector<Diagnostic> d;
    ValidateAndRepair(c, ValidationPolicy{}, d);
    WPM_CHECK_NEAR(c.metrics[0].sizeScale, 2.0, 1e-6);
    WPM_CHECK_EQ(c.metrics[0].precision, 3);
    WPM_CHECK(text::utf8Length(c.metrics[0].label) <= 24);
}

WPM_TEST(config_color_parse_format) {
    ColorARGB c{};
    WPM_CHECK(ParseColor("#FF4CAF50", c));
    WPM_CHECK_EQ(c.v, 0xFF4CAF50u);
    // #RRGGBB 视为不透明
    WPM_CHECK(ParseColor("#101418", c));
    WPM_CHECK_EQ(c.v, 0xFF101418u);
    // 空串 = 随主题
    WPM_CHECK(ParseColor("", c));
    WPM_CHECK(c.isAuto());
    WPM_CHECK(!ParseColor("#GGGGGG", c));
    WPM_CHECK(!ParseColor("#12345", c));
    WPM_CHECK_EQ(FormatColor(ColorARGB{0xFF4CAF50u}), std::string("#FF4CAF50"));
}

WPM_TEST(config_json_roundtrip_preserves_all) {
    Config c = MakeDefaultConfig();
    c.layout.orientation = Orientation::Vertical;
    c.layout.itemGapDp = 21;
    c.colors.theme = ThemeId::Custom;
    c.window.topmost = TopmostPolicy::GameOnly;
    c.window.overlayBackend = OverlayBackend::Rtss;
    c.gpu.selectedIndex = 2;
    c.fps.stutterBaseline = StutterBaseline::Median;
    c.text.fontFamily = "思源黑体";
    c.metrics[3].visible = false;
    c.metrics[5].precision = 2;
    c.metrics[5].color = ColorARGB{0xFF00FF00u};
    c.metrics[7].label = "自定义名";

    const std::string json = ConfigToJson(c);
    LoadResult lr = ConfigFromJson(json);
    WPM_CHECK(lr.error.empty());
    WPM_CHECK(lr.fileExisted);

    WPM_CHECK(lr.config.layout.orientation == Orientation::Vertical);
    WPM_CHECK_EQ(lr.config.layout.itemGapDp, 21);
    WPM_CHECK(lr.config.colors.theme == ThemeId::Custom);
    WPM_CHECK(lr.config.window.topmost == TopmostPolicy::GameOnly);
    WPM_CHECK(lr.config.window.overlayBackend == OverlayBackend::Rtss);
    WPM_CHECK_EQ(lr.config.gpu.selectedIndex, 2);
    WPM_CHECK(lr.config.fps.stutterBaseline == StutterBaseline::Median);
    WPM_CHECK_EQ(lr.config.text.fontFamily, std::string("思源黑体"));
    WPM_CHECK(!lr.config.metrics[3].visible);
    WPM_CHECK_EQ(lr.config.metrics[5].precision, 2);
    WPM_CHECK_EQ(lr.config.metrics[5].color.v, 0xFF00FF00u);
    WPM_CHECK_EQ(lr.config.metrics[7].label, std::string("自定义名"));
}

WPM_TEST(config_json_broken_never_throws) {
    LoadResult lr = ConfigFromJson("{ this is not json ");
    WPM_CHECK(!lr.error.empty());          // 报告错误
    WPM_CHECK_EQ(lr.config.metrics.size(), MetricCatalog().size());  // 回落默认
    WPM_CHECK(!lr.fileExisted);
}

WPM_TEST(config_json_unknown_keys_ignored) {
    LoadResult lr = ConfigFromJson(R"({"schemaVersion":1,"futureField":{"x":1},"layout":{"unknown":5}})");
    WPM_CHECK(lr.error.empty());
    WPM_CHECK(lr.config.layout.itemGapDp == 8);  // 未知字段不影响默认值
}

WPM_TEST(config_json_wrong_types_repaired) {
    // "itemGapDp": "abc" 类型不符 → 回退默认并记诊断，而不是抛异常
    LoadResult lr = ConfigFromJson(R"({"layout":{"itemGapDp":"abc"}})");
    WPM_CHECK(lr.error.empty());
    WPM_CHECK_EQ(lr.config.layout.itemGapDp, 8);
}

WPM_TEST(config_enum_string_stability) {
    // 配置文件里的枚举字符串是稳定表示，改名即破坏兼容（config-schema.md §8）
    WPM_CHECK_EQ(std::string(ToString(Orientation::Horizontal)), std::string("horizontal"));
    WPM_CHECK_EQ(std::string(ToString(Orientation::Vertical)), std::string("vertical"));
    WPM_CHECK_EQ(std::string(ToString(TopmostPolicy::GameOnly)), std::string("gameOnly"));
    WPM_CHECK_EQ(std::string(ToString(ThemeId::Dark)), std::string("dark"));
    WPM_CHECK_EQ(std::string(ToString(StutterBaseline::Median)), std::string("median"));
    WPM_CHECK_EQ(std::string(ToString(OverlayBackend::Rtss)), std::string("rtss"));
    WPM_CHECK_EQ(std::string(ToString(FpsMode::Service)), std::string("service"));

    Orientation o{};
    WPM_CHECK(ParseOrientation("Vertical", o) && o == Orientation::Vertical);
    WPM_CHECK(!ParseOrientation("nope", o));
    Align a{};
    WPM_CHECK(ParseAlign("center", a) && a == Align::Center);
    FpsMode f{};
    WPM_CHECK(ParseFpsMode("CLI", f) && f == FpsMode::Cli);
}

WPM_TEST(catalog_lookup_is_case_insensitive) {
    WPM_CHECK(FindMetric("cpu.usage") != nullptr);
    WPM_CHECK(FindMetric("CPU.USAGE") != nullptr);
    WPM_CHECK(FindMetric("nope") == nullptr);
    WPM_CHECK(ResolveMetricAlias("cpu.freq") != nullptr);
    WPM_CHECK_EQ(std::string(ResolveMetricAlias("cpu.freq")->id), std::string("cpu.freq.avg"));
    WPM_CHECK_EQ(std::string(ResolveMetricAlias("gpu.temp")->id), std::string("gpu.temp.core"));
}

WPM_TEST(catalog_ids_unique_and_wellformed) {
    const auto cat = MetricCatalog();
    for (size_t i = 0; i < cat.size(); ++i) {
        WPM_CHECK(!cat[i].id.empty());
        // 全部小写，点分三段以内
        WPM_CHECK(text::toLowerAscii(cat[i].id) == cat[i].id);
        for (size_t j = i + 1; j < cat.size(); ++j) WPM_CHECK(cat[i].id != cat[j].id);
    }
}
