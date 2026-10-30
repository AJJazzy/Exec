#pragma once
// Minimal test harness (no third-party dependency). Each test executable registers cases with
// TEST_CASE and links tests/support/test_main.cpp. Tests that rely on simulated components carry
// "MOCK" in their name and print a notice, so their results are never mistaken for evidence of
// compatibility with real Roblox software.

#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nexus_test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
void recordFailure(const char* file, int line, const std::string& message);
void skip(const std::string& reason);

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct RequireFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Skipped : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class T>
std::string show(const T& value) {
    if constexpr (requires(std::ostream& os, const T& v) { os << v; }) {
        std::ostringstream os;
        os << value;
        return os.str();
    } else {
        return "<value of non-printable type>";
    }
}

template <class A, class B>
std::string describe(const A& a, const B& b) {
    return "\n      left:  " + show(a) + "\n      right: " + show(b);
}

// Scratch directory unique to the running test executable (removed at exit).
std::string tempDir();
// Directory containing the test executables (and nexus-runner / nexus-cli).
std::string binDir();

} // namespace nexus_test

#define NEXUS_CONCAT2(a, b) a##b
#define NEXUS_CONCAT(a, b) NEXUS_CONCAT2(a, b)

#define TEST_CASE(name)                                                                    \
    static void NEXUS_CONCAT(test_, name)();                                               \
    static nexus_test::Registrar NEXUS_CONCAT(reg_, name)(#name, NEXUS_CONCAT(test_, name)); \
    static void NEXUS_CONCAT(test_, name)()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) nexus_test::recordFailure(__FILE__, __LINE__, "CHECK(" #cond ")");     \
    } while (0)

#define REQUIRE(cond)                                                                      \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            nexus_test::recordFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");            \
            throw nexus_test::RequireFailed(#cond);                                        \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        const auto nexus_a_ = (a); /* copies: operands may refer into temporaries */       \
        const auto nexus_b_ = (b);                                                         \
        if (!(nexus_a_ == nexus_b_))                                                       \
            nexus_test::recordFailure(__FILE__, __LINE__,                                  \
                                      "CHECK_EQ(" #a ", " #b ")" + nexus_test::describe(nexus_a_, nexus_b_)); \
    } while (0)

#define SKIP(reason) nexus_test::skip(reason)
