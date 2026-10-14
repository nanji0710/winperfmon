// SPDX-License-Identifier: MIT
// 自研 JSON 子集单测（config-schema.md §0 不变量 6）
#include "core/json.h"

#include "test.h"

using namespace wpm;

WPM_TEST(json_parse_scalars) {
    json::Value v;
    json::ParseError e;
    WPM_CHECK(json::parse("42", v, e));
    WPM_CHECK(v.isNumber());
    WPM_CHECK_NEAR(v.asNumber(), 42.0, 1e-9);

    WPM_CHECK(json::parse("true", v, e) && v.asBool());
    WPM_CHECK(json::parse("\"abc\"", v, e) && v.asString() == "abc");
    WPM_CHECK(json::parse("null", v, e) && v.isNull());
}

WPM_TEST(json_parse_object_and_find) {
    json::Value v;
    json::ParseError e;
    WPM_CHECK(json::parse(R"({"a":1,"b":{"c":"x"},"d":[1,2,3]})", v, e));
    WPM_CHECK(v.isObject());
    const json::Value* a = v.find("a");
    WPM_CHECK(a != nullptr);
    WPM_CHECK_NEAR(a->asNumber(), 1.0, 1e-9);
    WPM_CHECK(v.find("nope") == nullptr);
    const json::Value* b = v.find("b");
    WPM_CHECK(b != nullptr && b->isObject() && b->find("c")->asString() == "x");
    const json::Value* d = v.find("d");
    WPM_CHECK(d != nullptr && d->isArray() && d->asArray().size() == 3);
}

WPM_TEST(json_rejects_invalid) {
    json::Value v;
    json::ParseError e;
    // 尾随逗号：本子集明确不支持
    WPM_CHECK(!json::parse(R"({"a":1,})", v, e));
    WPM_CHECK(!e.message.empty());
    // 注释同理
    WPM_CHECK(!json::parse("{\"a\":1 /* c */}", v, e));
    // 未闭合
    WPM_CHECK(!json::parse("{\"a\":", v, e));
    WPM_CHECK(!json::parse("", v, e));
}

WPM_TEST(json_roundtrip) {
    json::Value v;
    json::ParseError e;
    WPM_CHECK(json::parse(R"({"z":1,"a":"汉字","n":[1,2]})", v, e));
    const std::string text = json::stringify(v, 2);
    json::Value v2;
    json::ParseError e2;
    WPM_CHECK(json::parse(text, v2, e2));
    WPM_CHECK_NEAR(v2.find("z")->asNumber(), 1.0, 1e-9);
    WPM_CHECK(v2.find("a")->asString() == "汉字");
    WPM_CHECK(v2.find("n")->asArray().size() == 2);
}

WPM_TEST(json_limits) {
    json::Value v;
    json::ParseError e;
    // 超过深度上限应失败而非崩溃
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "[";
    for (int i = 0; i < 40; ++i) deep += "]";
    WPM_CHECK(!json::parse(deep, v, e));

    // 超过字节上限同样失败
    std::string big = "\"" + std::string(200 * 1024, 'x') + "\"";
    WPM_CHECK(!json::parse(big, v, e));
}

WPM_TEST(json_escape_unicode) {
    const std::string s = json::escape("a\"b\\c\n汉字");
    WPM_CHECK(s.find("\\\"") != std::string::npos);
    WPM_CHECK(s.find("\\n") != std::string::npos);
    WPM_CHECK(s.find("汉字") != std::string::npos);  // UTF-8 直出，不转 \u
}
