#pragma once
// Minimal value-or-error type. std::expected is C++23 and not available in every Apple libc++ that
// Nexus supports, so the core uses this small equivalent.

#include <cassert>
#include <string>
#include <utility>
#include <variant>

namespace nexus {

struct Error {
    std::string code;    // stable machine-readable identifier, e.g. "io.not_found"
    std::string message; // human-readable description

    Error() = default;
    Error(std::string c, std::string m) : code(std::move(c)), message(std::move(m)) {}
};

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const { return storage_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() & { assert(ok()); return std::get<0>(storage_); }
    const T& value() const& { assert(ok()); return std::get<0>(storage_); }
    T&& value() && { assert(ok()); return std::get<0>(std::move(storage_)); }

    const Error& error() const { assert(!ok()); return std::get<1>(storage_); }

    T valueOr(T fallback) const { return ok() ? std::get<0>(storage_) : std::move(fallback); }

private:
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }
    const Error& error() const { assert(!ok_); return error_; }

private:
    Error error_;
    bool ok_ = true;
};

inline Result<void> success() { return {}; }

} // namespace nexus
