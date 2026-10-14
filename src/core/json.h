// SPDX-License-Identifier: MIT
// 自研极简 JSON 子集 —— 零第三方依赖（config-schema.md §0 不变量 6）
//
// 只实现配置读写所需的子集：
//   值类型：object / array / string / number(double) / bool / null
//   不支持：注释、尾随逗号、单引号、NaN/Infinity、\u 代理对之外的转义
// 设计取舍：
//   - 不用异常（/EHsc 关闭时也能编译）
//   - 解析失败通过 bool + Diagnostic 报告，绝不抛
//   - 对象保留插入顺序（序列化时输出稳定，便于 diff 与人工编辑）
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wpm::json {

class Value;
using Object = std::vector<std::pair<std::string, Value>>;
using Array = std::vector<Value>;

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    explicit Value(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Value(double d) : type_(Type::Number), num_(d) {}
    explicit Value(int i) : type_(Type::Number), num_(static_cast<double>(i)) {}
    explicit Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    explicit Value(const char* s) : type_(Type::String), str_(s) {}
    explicit Value(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    explicit Value(Object o) : type_(Type::Object), obj_(std::move(o)) {}

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    // 类型不符时返回默认值（校验即修复原则：不抛异常）
    [[nodiscard]] bool asBool(bool def = false) const noexcept;
    [[nodiscard]] double asNumber(double def = 0.0) const noexcept;
    [[nodiscard]] int asInt(int def = 0) const noexcept;
    [[nodiscard]] std::string asString(std::string_view def = {}) const;

    [[nodiscard]] const Array& asArray() const noexcept { return arr_; }
    [[nodiscard]] Array& asArray() noexcept { return arr_; }
    [[nodiscard]] const Object& asObject() const noexcept { return obj_; }
    [[nodiscard]] Object& asObject() noexcept { return obj_; }

    // 对象成员查找；不存在返回 nullptr
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    Value* find(std::string_view key) noexcept;

    // 对象写入（已存在则覆盖，保持原位置）
    void set(std::string key, Value v);

    [[nodiscard]] static Value makeObject() { return Value(Object{}); }
    [[nodiscard]] static Value makeArray() { return Value(Array{}); }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

struct ParseLimits {
    size_t maxBytes = 256 * 1024;  // config-schema.md §1：配置上限 256 KB
    int maxDepth = 16;             // 深度上限
    size_t maxStringBytes = 4096;  // 单字符串上限
};

struct ParseError {
    size_t offset = 0;      // 出错字节偏移
    int line = 1;           // 1-based 行号
    std::string message;    // 已本地化的中文消息
};

// 解析 UTF-8 JSON。成功返回 true；失败时 out 不变、err 填好。
bool parse(std::string_view text, Value& out, ParseError& err,
           const ParseLimits& limits = {});

// 序列化。indent=0 输出紧凑形式；默认 2 空格缩进（config-schema.md §1）
std::string stringify(const Value& v, int indent = 2);

// 转义为 JSON 字符串字面量（含引号）
std::string escape(std::string_view s);

}  // namespace wpm::json
