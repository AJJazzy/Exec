#include "nexus/common/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nexus::json {

namespace {
const std::string kEmptyString;
const Array kEmptyArray;
const Object kEmptyObject;
} // namespace

// ---------------------------------------------------------------------------------------------
// Object

Value& Object::operator[](std::string_view key) {
    for (auto& [k, v] : entries_) {
        if (k == key) return v;
    }
    entries_.emplace_back(std::string(key), Value());
    return entries_.back().second;
}

const Value* Object::find(std::string_view key) const {
    for (const auto& [k, v] : entries_) {
        if (k == key) return &v;
    }
    return nullptr;
}

Value* Object::find(std::string_view key) {
    for (auto& [k, v] : entries_) {
        if (k == key) return &v;
    }
    return nullptr;
}

bool Object::erase(std::string_view key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == key) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Value

Value::Value(Object o) : data_(Box<Object>(std::move(o))) {}

Value& Value::operator[](std::string_view key) { return mutableObject()[key]; }

bool Value::asBool(bool fallback) const { return isBool() ? std::get<bool>(data_) : fallback; }
double Value::asNumber(double fallback) const { return isNumber() ? std::get<double>(data_) : fallback; }

int64_t Value::asInt(int64_t fallback) const {
    if (!isNumber()) return fallback;
    double d = std::get<double>(data_);
    if (!std::isfinite(d) || d < -9.2e18 || d > 9.2e18) return fallback;
    return static_cast<int64_t>(d);
}

const std::string& Value::asString() const { return isString() ? std::get<std::string>(data_) : kEmptyString; }
const Array& Value::asArray() const { return isArray() ? std::get<Array>(data_) : kEmptyArray; }
const Object& Value::asObject() const { return isObject() ? std::get<Box<Object>>(data_).get() : kEmptyObject; }

Array& Value::mutableArray() {
    if (!isArray()) data_ = Array{};
    return std::get<Array>(data_);
}

Object& Value::mutableObject() {
    if (!isObject()) data_ = Box<Object>();
    return std::get<Box<Object>>(data_).get();
}

const Value* Value::get(std::string_view key) const { return isObject() ? asObject().find(key) : nullptr; }

std::string Value::getString(std::string_view key, std::string fallback) const {
    const Value* v = get(key);
    return v && v->isString() ? v->asString() : fallback;
}

double Value::getNumber(std::string_view key, double fallback) const {
    const Value* v = get(key);
    return v && v->isNumber() ? v->asNumber() : fallback;
}

int64_t Value::getInt(std::string_view key, int64_t fallback) const {
    const Value* v = get(key);
    return v ? v->asInt(fallback) : fallback;
}

bool Value::getBool(std::string_view key, bool fallback) const {
    const Value* v = get(key);
    return v && v->isBool() ? v->asBool() : fallback;
}

bool Value::operator==(const Value& other) const {
    if (type() != other.type()) return false;
    switch (type()) {
    case Type::Null: return true;
    case Type::Bool: return asBool() == other.asBool();
    case Type::Number: return asNumber() == other.asNumber();
    case Type::String: return asString() == other.asString();
    case Type::Array: {
        const Array& a = asArray();
        const Array& b = other.asArray();
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) return false;
        return true;
    }
    case Type::Object: {
        const Object& a = asObject();
        const Object& b = other.asObject();
        if (a.size() != b.size()) return false;
        for (const auto& [k, v] : a) {
            const Value* ov = b.find(k);
            if (!ov || *ov != v) return false;
        }
        return true;
    }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Parser

namespace {

class Parser {
public:
    Parser(std::string_view text, const ParseOptions& options) : text_(text), options_(options) {}

    Result<Value> run() {
        skipWhitespace();
        Value v;
        if (!parseValue(v, 0)) return makeError();
        skipWhitespace();
        if (pos_ != text_.size()) {
            error_ = "unexpected trailing characters";
            return makeError();
        }
        return v;
    }

private:
    Error makeError() const {
        return Error("json.parse", error_ + " at offset " + std::to_string(pos_));
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool consume(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    bool parseValue(Value& out, size_t depth) {
        if (depth > options_.maxDepth) {
            error_ = "maximum nesting depth exceeded";
            return false;
        }
        if (pos_ >= text_.size()) {
            error_ = "unexpected end of input";
            return false;
        }
        char c = text_[pos_];
        switch (c) {
        case '{': return parseObject(out, depth);
        case '[': return parseArray(out, depth);
        case '"': {
            std::string s;
            if (!parseString(s)) return false;
            out = Value(std::move(s));
            return true;
        }
        case 't':
            if (consume("true")) { out = Value(true); return true; }
            break;
        case 'f':
            if (consume("false")) { out = Value(false); return true; }
            break;
        case 'n':
            if (consume("null")) { out = Value(); return true; }
            break;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
            break;
        }
        error_ = "unexpected character";
        return false;
    }

    bool parseNumber(Value& out) {
        size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        auto digits = [&] {
            size_t s = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            return pos_ > s;
        };
        if (!digits()) {
            error_ = "invalid number";
            return false;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digits()) {
                error_ = "invalid number fraction";
                return false;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (!digits()) {
                error_ = "invalid number exponent";
                return false;
            }
        }
        std::string token(text_.substr(start, pos_ - start));
        out = Value(std::strtod(token.c_str(), nullptr));
        return true;
    }

    static void appendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseHex4(uint32_t& out) {
        if (pos_ + 4 > text_.size()) {
            error_ = "truncated unicode escape";
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else {
                error_ = "invalid unicode escape";
                return false;
            }
        }
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_; // opening quote
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                error_ = "control character in string";
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) break;
            char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!parseHex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low = 0;
                    if (!consume("\\u") || !parseHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        error_ = "invalid surrogate pair";
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    error_ = "unpaired low surrogate";
                    return false;
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                error_ = "invalid escape sequence";
                return false;
            }
        }
        error_ = "unterminated string";
        return false;
    }

    bool parseArray(Value& out, size_t depth) {
        ++pos_;
        Array arr;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = Value(std::move(arr));
            return true;
        }
        while (true) {
            skipWhitespace();
            Value item;
            if (!parseValue(item, depth + 1)) return false;
            arr.push_back(std::move(item));
            skipWhitespace();
            if (pos_ >= text_.size()) {
                error_ = "unterminated array";
                return false;
            }
            char c = text_[pos_++];
            if (c == ']') break;
            if (c != ',') {
                error_ = "expected ',' or ']'";
                return false;
            }
        }
        out = Value(std::move(arr));
        return true;
    }

    bool parseObject(Value& out, size_t depth) {
        ++pos_;
        Object obj;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = Value(std::move(obj));
            return true;
        }
        while (true) {
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                error_ = "expected object key";
                return false;
            }
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                error_ = "expected ':'";
                return false;
            }
            ++pos_;
            skipWhitespace();
            Value item;
            if (!parseValue(item, depth + 1)) return false;
            obj[key] = std::move(item);
            skipWhitespace();
            if (pos_ >= text_.size()) {
                error_ = "unterminated object";
                return false;
            }
            char c = text_[pos_++];
            if (c == '}') break;
            if (c != ',') {
                error_ = "expected ',' or '}'";
                return false;
            }
        }
        out = Value(std::move(obj));
        return true;
    }

    std::string_view text_;
    ParseOptions options_;
    size_t pos_ = 0;
    std::string error_;
};

void dumpNumber(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null"; // JSON has no representation for NaN/Inf
        return;
    }
    char buf[32];
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
    } else {
        std::snprintf(buf, sizeof(buf), "%.17g", d);
    }
    out += buf;
}

void dumpString(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void dumpValue(std::string& out, const Value& v, int indent, int level) {
    auto newline = [&](int lvl) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * lvl), ' ');
    };
    switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Number: dumpNumber(out, v.asNumber()); break;
    case Value::Type::String: dumpString(out, v.asString()); break;
    case Value::Type::Array: {
        const Array& a = v.asArray();
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += ',';
            newline(level + 1);
            dumpValue(out, a[i], indent, level + 1);
        }
        if (!a.empty()) newline(level);
        out += ']';
        break;
    }
    case Value::Type::Object: {
        const Object& o = v.asObject();
        out += '{';
        bool first = true;
        for (const auto& [k, item] : o) {
            if (!first) out += ',';
            first = false;
            newline(level + 1);
            dumpString(out, k);
            out += indent < 0 ? ":" : ": ";
            dumpValue(out, item, indent, level + 1);
        }
        if (!o.empty()) newline(level);
        out += '}';
        break;
    }
    }
}

} // namespace

Result<Value> parse(std::string_view text, const ParseOptions& options) { return Parser(text, options).run(); }

std::string dump(const Value& value, int indent) {
    std::string out;
    dumpValue(out, value, indent, 0);
    return out;
}

std::string quote(std::string_view text) {
    std::string out;
    dumpString(out, text);
    return out;
}

} // namespace nexus::json
