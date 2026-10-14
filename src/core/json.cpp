// SPDX-License-Identifier: MIT
#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wpm::json {
namespace {

constexpr bool isWs(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // 孤立代理
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        return false;
    }
    return true;
}

int hexVal(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

class Parser {
public:
    Parser(std::string_view s, const ParseLimits& lim, ParseError& err)
        : s_(s), lim_(lim), err_(err) {}

    bool run(Value& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (pos_ != s_.size()) return fail("文档末尾有多余内容");
        return true;
    }

private:
    std::string_view s_;
    const ParseLimits& lim_;
    ParseError& err_;
    size_t pos_ = 0;

    [[nodiscard]] bool eof() const noexcept { return pos_ >= s_.size(); }
    [[nodiscard]] char peek() const noexcept { return eof() ? '\0' : s_[pos_]; }

    void skipWs() noexcept {
        while (!eof() && isWs(s_[pos_])) ++pos_;
    }

    bool fail(std::string msg) {
        err_.offset = pos_;
        err_.line = 1;
        for (size_t i = 0; i < pos_ && i < s_.size(); ++i)
            if (s_[i] == '\n') ++err_.line;
        err_.message = std::move(msg);
        return false;
    }

    bool literal(std::string_view lit) {
        if (s_.size() - pos_ < lit.size()) return false;
        if (s_.compare(pos_, lit.size(), lit) != 0) return false;
        pos_ += lit.size();
        return true;
    }

    bool parseValue(Value& out, int depth) {
        if (depth > lim_.maxDepth) return fail("嵌套层数超过上限（16）");
        skipWs();
        if (eof()) return fail("内容意外结束");
        switch (peek()) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case 't':
                if (!literal("true")) return fail("非法字面量（期望 true/false/null）");
                out = Value(true);
                return true;
            case 'f':
                if (!literal("false")) return fail("非法字面量（期望 true/false/null）");
                out = Value(false);
                return true;
            case 'n':
                if (!literal("null")) return fail("非法字面量（期望 true/false/null）");
                out = Value();
                return true;
            default: return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth) {
        ++pos_;  // '{'
        Object obj;
        skipWs();
        if (peek() == '}') { ++pos_; out = Value(std::move(obj)); return true; }
        for (;;) {
            skipWs();
            if (peek() != '"') return fail("对象的键必须是字符串");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (peek() != ':') return fail("键之后缺少 ':'");
            ++pos_;
            Value v;
            if (!parseValue(v, depth + 1)) return false;
            // 重复键：后者覆盖前者（宽恕策略，不报错）
            bool replaced = false;
            for (auto& kv : obj) {
                if (kv.first == key) { kv.second = std::move(v); replaced = true; break; }
            }
            if (!replaced) obj.emplace_back(std::move(key), std::move(v));
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; break; }
            return fail("对象中缺少 ',' 或 '}'");
        }
        out = Value(std::move(obj));
        return true;
    }

    bool parseArray(Value& out, int depth) {
        ++pos_;  // '['
        Array arr;
        skipWs();
        if (peek() == ']') { ++pos_; out = Value(std::move(arr)); return true; }
        for (;;) {
            Value v;
            if (!parseValue(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; break; }
            return fail("数组中缺少 ',' 或 ']'");
        }
        out = Value(std::move(arr));
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // 开头引号
        out.clear();
        for (;;) {
            if (eof()) return fail("字符串未闭合");
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') { ++pos_; return true; }
            if (c < 0x20) return fail("字符串中出现未转义的控制字符");
            if (c == '\\') {
                ++pos_;
                if (eof()) return fail("转义序列不完整");
                const char e = s_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!readHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // UTF-16 代理对
                            if (s_.size() - pos_ < 6 || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                                return fail("高位代理后缺少低位代理");
                            pos_ += 2;
                            uint32_t lo = 0;
                            if (!readHex4(lo)) return false;
                            if (lo < 0xDC00 || lo > 0xDFFF) return fail("非法低位代理");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return fail("孤立的低位代理");
                        }
                        if (!appendUtf8(out, cp)) return fail("非法码点");
                        break;
                    }
                    default: return fail("未知的转义字符");
                }
                continue;
            }
            out.push_back(static_cast<char>(c));
            ++pos_;
            if (out.size() > lim_.maxStringBytes) return fail("单个字符串超过 4 KB 上限");
        }
    }

    bool readHex4(uint32_t& v) {
        if (s_.size() - pos_ < 4) return fail("\\u 转义不足 4 位");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hexVal(s_[pos_ + static_cast<size_t>(i)]);
            if (h < 0) return fail("\\u 转义含非十六进制字符");
            v = (v << 4) | static_cast<uint32_t>(h);
        }
        pos_ += 4;
        return true;
    }

    bool parseNumber(Value& out) {
        const size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (eof()) return fail("数字不完整");
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            return fail("非法字符（期望值）");
        }
        if (peek() == '.') {
            ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return fail("小数点后缺少数字");
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return fail("指数部分缺少数字");
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        const std::string_view tok = s_.substr(start, pos_ - start);
        // 手工转换以避免依赖 locale（strtod 受区域设置影响，小数点可能是逗号）
        const double d = parseFinite(tok);
        if (!std::isfinite(d)) return fail("数字超出可表示范围");
        out = Value(d);
        return true;
    }

    static double parseFinite(std::string_view tok) {
        std::string buf(tok);
        char* end = nullptr;
        const double d = std::strtod(buf.c_str(), &end);
        if (end == buf.c_str() || *end != '\0') return std::nan("");
        return d;
    }
};

void indentTo(std::string& out, int n) {
    out.append(static_cast<size_t>(n) * 2, ' ');
}

void stringifyImpl(const Value& v, std::string& out, int indent, int level) {
    const bool pretty = indent > 0;
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Type::Number: {
            const double d = v.asNumber();
            if (d == static_cast<double>(static_cast<long long>(d)) &&
                std::fabs(d) < 1e15) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
                out += buf;
            } else {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.6g", d);
                out += buf;
            }
            break;
        }
        case Type::String: out += escape(v.asString()); break;
        case Type::Array: {
            const auto& a = v.asArray();
            if (a.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < a.size(); ++i) {
                if (pretty) { out += '\n'; indentTo(out, level + 1); }
                stringifyImpl(a[i], out, indent, level + 1);
                if (i + 1 < a.size()) out += ',';
            }
            if (pretty) { out += '\n'; indentTo(out, level); }
            out += ']';
            break;
        }
        case Type::Object: {
            const auto& o = v.asObject();
            if (o.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < o.size(); ++i) {
                if (pretty) { out += '\n'; indentTo(out, level + 1); }
                out += escape(o[i].first);
                out += pretty ? ": " : ":";
                stringifyImpl(o[i].second, out, indent, level + 1);
                if (i + 1 < o.size()) out += ',';
            }
            if (pretty) { out += '\n'; indentTo(out, level); }
            out += '}';
            break;
        }
    }
}

}  // namespace

bool Value::asBool(bool def) const noexcept {
    return type_ == Type::Bool ? bool_ : def;
}
double Value::asNumber(double def) const noexcept {
    return type_ == Type::Number ? num_ : def;
}
int Value::asInt(int def) const noexcept {
    if (type_ != Type::Number) return def;
    const double r = num_ < 0 ? num_ - 0.5 : num_ + 0.5;
    if (r < -2147483648.0 || r > 2147483647.0) return def;
    return static_cast<int>(r);
}
std::string Value::asString(std::string_view def) const {
    return type_ == Type::String ? str_ : std::string(def);
}

const Value* Value::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) return nullptr;
    for (const auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
Value* Value::find(std::string_view key) noexcept {
    if (type_ != Type::Object) return nullptr;
    for (auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void Value::set(std::string key, Value v) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        obj_.clear();
    }
    for (auto& kv : obj_) {
        if (kv.first == key) { kv.second = std::move(v); return; }
    }
    obj_.emplace_back(std::move(key), std::move(v));
}

bool parse(std::string_view text, Value& out, ParseError& err, const ParseLimits& limits) {
    err = ParseError{};
    if (text.size() > limits.maxBytes) {
        err.message = "配置超过 256 KB 上限";
        return false;
    }
    // 去掉 UTF-8 BOM（写盘时我们从不写 BOM，但用户可能用记事本另存）
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Value tmp;
    Parser p(text, limits, err);
    if (!p.run(tmp)) return false;
    out = std::move(tmp);
    return true;
}

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out.push_back(ch);  // UTF-8 原样输出
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string stringify(const Value& v, int indent) {
    std::string out;
    stringifyImpl(v, out, indent, 0);
    return out;
}

}  // namespace wpm::json
