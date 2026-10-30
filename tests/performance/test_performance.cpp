// Performance measurements. Each test prints what it measured; assertions use generous bounds so
// they catch regressions by orders of magnitude without being flaky on shared CI machines.
#include "nexus_test.hpp"

#include "nexus/common/ring_buffer.hpp"
#include "nexus/compatibility/analyzer.hpp"
#include "nexus/compiler/compiler.hpp"
#include "nexus/editor/editor_services.hpp"
#include "nexus/execution/local_targets.hpp"
#include "nexus/runtime/luau_runtime.hpp"
#include "nexus/sessions/session_manager.hpp"

using namespace nexus;
using Clock = std::chrono::steady_clock;

namespace {
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::string largeScript(int functions) {
    std::string s = "local M = {}\n"; // functions live in a table: Luau allows at most 200 live locals
    for (int i = 0; i < functions; ++i) {
        s += "function M.f" + std::to_string(i) + "(a, b)\n";
        s += "    local t = {}\n    for i = 1, 10 do\n        if i % 2 == 0 then t[#t + 1] = a * i else t[#t + 1] = b + i end\n    end\n";
        s += "    return #t, \"string \" .. tostring(a) -- comment\nend\n";
    }
    return s;
}
} // namespace

TEST_CASE(perf_compile_throughput) {
    std::string src = largeScript(500); // ~4000 lines
    auto start = Clock::now();
    const int n = 10;
    for (int i = 0; i < n; ++i) REQUIRE(compileSource(src).ok);
    double per = msSince(start) / n;
    std::printf("    compile %zu bytes: %.2f ms per compile\n", src.size(), per);
    CHECK(per < 2000);
}

TEST_CASE(perf_execution_speed) {
    RuntimeOptions o;
    o.limits.timeout = std::chrono::seconds(60);
    auto start = Clock::now();
    auto r = LuauRuntime::run("local function fib(n) if n < 2 then return n end return fib(n-1) + fib(n-2) end\nprint(fib(27))", o,
                              [](const OutputEvent&) {}, {});
    double ms = msSince(start);
    REQUIRE(r.succeeded());
    std::printf("    fib(27) in-process: %.1f ms (interrupt checks: %llu)\n", ms, static_cast<unsigned long long>(r.stats.interruptChecks));
    CHECK(ms < 20000);
}

TEST_CASE(perf_inprocess_startup_latency) {
    RuntimeOptions o;
    const int n = 200;
    auto start = Clock::now();
    for (int i = 0; i < n; ++i) REQUIRE(LuauRuntime::run("print(1)", o, [](const OutputEvent&) {}, {}).succeeded());
    double per = msSince(start) / n;
    std::printf("    in-process run (VM create + compile + run + close): %.3f ms\n", per);
    CHECK(per < 100);
}

TEST_CASE(perf_isolated_runner_startup_latency) {
    IsolatedTarget target(nexus_test::binDir() + "/nexus-runner");
    ExecutionRequest req;
    req.source = "print(1)";
    const int n = 20;
    auto start = Clock::now();
    for (int i = 0; i < n; ++i) REQUIRE(target.execute(req, [](const OutputEvent&) {}, {}).succeeded());
    double per = msSince(start) / n;
    std::printf("    isolated run (spawn + sandbox + run + exit): %.2f ms\n", per);
    CHECK(per < 1500);
}

TEST_CASE(perf_editor_services_on_large_file) {
    std::string src = largeScript(1250); // ~10k lines
    auto start = Clock::now();
    auto toks = editor::highlight(src);
    double hl = msSince(start);
    start = Clock::now();
    auto fmt = editor::format(src);
    double fm = msSince(start);
    start = Clock::now();
    auto report = compat::analyzeCompatibility(src);
    double an = msSince(start);
    start = Clock::now();
    auto diags = checkSyntax(src);
    double sy = msSince(start);
    REQUIRE(fmt.ok);
    std::printf("    %zu lines: highlight %.1f ms (%zu tokens), format %.1f ms, analyze %.1f ms, syntax %.1f ms\n",
                static_cast<size_t>(std::count(src.begin(), src.end(), '\n')), hl, toks.size(), fm, an, sy);
    CHECK(diags.empty());
    CHECK(hl < 3000 && fm < 3000 && an < 3000);
}

TEST_CASE(perf_console_buffer_is_bounded_under_flood) {
    Logger logger;
    TargetRegistry targets;
    targets.add(std::make_shared<InProcessTarget>());
    SessionManager sessions(logger, targets, 5000);
    ExecutionRequest req;
    req.source = "for i = 1, 200000 do print(i) end";
    req.limits.timeout = std::chrono::seconds(60);
    req.limits.maxOutputBytes = 64ull << 20;
    auto start = Clock::now();
    REQUIRE(sessions.start(req, InProcessTarget::kId).ok());
    REQUIRE(sessions.waitForIdle(std::chrono::seconds(60)));
    double ms = msSince(start);
    std::printf("    200k printed lines through the session pipeline: %.1f ms; console kept %zu lines\n", ms, sessions.console().size());
    CHECK_EQ(sessions.console().size(), size_t(5000));
    CHECK(ms < 30000);
}
