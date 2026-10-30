#pragma once
// Small JSON DOM used for settings, persisted state, the runner protocol, the MCP protocol and
// exported reports. Kept in-tree (instead of adding a third-party dependency) because the needed
// surface is small and auditable; it is covered by tests/unit/test_json.cpp.

#include "nexus/common/result.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nexus::json {

class Value;
class Object;
using Array = std::vector<Value>;

// Heap box with value semantics; lets Value hold an Object before Object is a complete type.
template <class T>
class Box {
public:
    Box() : ptr_(std::make_unique<T>()) {}
    Box(T value) : ptr_(std::make_unique<T>(std::move(value))) {}
    Box(const Box& other) : ptr_(std::make_unique<T>(*other.ptr_)) {}
    Box(Box&&) noexcept = default;
    Box& operator=(const Box& other) {
        if (this != &other) ptr_ = std::make_unique<T>(*other.ptr_);
        return *this;
    }
    Box& operator=(Box&&) noexcept = default;
    ~Box() = default;
    T& get() { return *ptr_; }
    const T& get() const { return *ptr_; }

private:
    std::unique_ptr<T> ptr_;
};

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : data_(b) {}
    Value(int v) : data_(static_cast<double>(v)) {}
    Value(unsigned v) : data_(static_cast<double>(v)) {}
    Value(long v) : data_(static_cast<double>(v)) {}
    Value(unsigned long v) : data_(static_cast<double>(v)) {}
    Value(long long v) : data_(static_cast<double>(v)) {}
    Value(unsigned long long v) : data_(static_cast<double>(v)) {}
    Value(double v) : data_(v) {}
    Value(const char* s) : data_(std::string(s)) {}
    Value(std::string s) : data_(std::move(s)) {}
    Value(std::string_view s) : data_(std::string(s)) {}
    Value(Array a) : data_(std::move(a)) {}
    Value(Object o);

    Type type() const { return static_cast<Type>(data_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    int64_t asInt(int64_t fallback = 0) const;
    const std::string& asString() const; // empty string when not a string
    const Array& asArray() const;        // empty array when not an array
    const Object& asObject() const;      // empty object when not an object
    Array& mutableArray();               // converts to an empty array if needed
    Object& mutableObject();             // converts to an empty object if needed

    // Object helpers; return nullptr / fallback when absent or of a different type.
    const Value* get(std::string_view key) const;
    std::string getString(std::string_view key, std::string fallback = {}) const;
    double getNumber(std::string_view key, double fallback = 0.0) const;
    int64_t getInt(std::string_view key, int64_t fallback = 0) const;
    bool getBool(std::string_view key, bool fallback = false) const;

    Value& operator[](std::string_view key);

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

private:
    std::variant<std::monostate, bool, double, std::string, Array, Box<Object>> data_;
};

// Insertion-ordered object; lookups are linear, which is fine for the small objects Nexus uses.
class Object {
public:
    Object() = default;
    Object(std::initializer_list<std::pair<std::string, Value>> init) : entries_(init) {}

    Value& operator[](std::string_view key);
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    bool erase(std::string_view key);
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    using Entries = std::vector<std::pair<std::string, Value>>;
    Entries::const_iterator begin() const { return entries_.begin(); }
    Entries::const_iterator end() const { return entries_.end(); }
    Entries::iterator begin() { return entries_.begin(); }
    Entries::iterator end() { return entries_.end(); }

private:
    Entries entries_;
};

struct ParseOptions {
    size_t maxDepth = 256;
};

Result<Value> parse(std::string_view text, const ParseOptions& options = {});

// indent < 0 produces compact single-line output (used for line-delimited protocols).
std::string dump(const Value& value, int indent = -1);

// Escapes a string as a JSON string literal including the surrounding quotes.
std::string quote(std::string_view text);

} // namespace nexus::json
