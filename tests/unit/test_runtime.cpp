// Unit tests for the standalone Luau runtime. All tests execute real Luau code in the official VM.
#include "nexus_test.hpp"

#include "nexus/runtime/luau_runtime.hpp"

#include <algorithm>
#include <thread>

using namespace nexus;

namespace {

struct Run {
    ExecutionResult result;
    std::vector<OutputEvent> output;
    std::string text(OutputStream s = OutputStream::Output) const {
        std::string t;
        for (const auto& e : output)
            if (e.stream == s) t += e.text + "\n";
        return t;
    }
};

Run run(const std::string& src, ExecutionLimits limits = {}, CancellationToken cancel = {}) {
    Run r;
    RuntimeOptions o;
    o.limits = limits;
    o.chunkName = "Test";
    r.result = LuauRuntime::run(src, o, [&](const OutputEvent& e) { r.output.push_back(e); }, cancel);
    return r;
}

ExecutionLimits shortLimits(int ms) {
    ExecutionLimits l;
    l.timeout = std::chrono::milliseconds(ms);
    return l;
}

} // namespace

TEST_CASE(runtime_print_and_warn_are_captured) {
    auto r = run("print('hello', 1, true, nil, 2.5)\nwarn('careful')\nprint(setmetatable({}, {__tostring=function() return 'custom' end}))");
    CHECK(r.result.succeeded());
    CHECK_EQ(r.text(), std::string("hello\t1\ttrue\tnil\t2.5\ncustom\n"));
    CHECK_EQ(r.text(OutputStream::Warning), std::string("careful\n"));
    CHECK(r.result.stats.peakMemoryBytes > 0);
    CHECK(r.result.stats.bytecodeBytes > 0);
}

TEST_CASE(runtime_reports_runtime_errors_with_line_and_traceback) {
    auto r = run("local function inner()\n  local t = nil\n  return t.field\nend\nlocal function outer() return inner() end\nouter()\n");
    CHECK(r.result.status == ExecutionStatus::RuntimeError);
    REQUIRE(r.result.error.has_value());
    CHECK_EQ(r.result.error->line, uint32_t(3));
    CHECK(r.result.error->message.find("attempt to index nil") != std::string::npos);
    CHECK(r.result.error->traceback.find("inner") != std::string::npos);
    CHECK(r.result.error->frames.size() >= 2);
    CHECK(!r.result.diagnostics.empty());
}

TEST_CASE(runtime_error_with_non_string_value) {
    auto r = run("error({code = 1})");
    CHECK(r.result.status == ExecutionStatus::RuntimeError);
    REQUIRE(r.result.error.has_value());
    CHECK(r.result.error->message.find("table") != std::string::npos);
}

TEST_CASE(runtime_compile_errors_are_not_executed) {
    auto r = run("print('should not run')\nlocal = 5");
    CHECK(r.result.status == ExecutionStatus::CompileError);
    CHECK(r.output.empty());
    CHECK(!r.result.diagnostics.empty());
}

TEST_CASE(runtime_infinite_loop_times_out) {
    auto start = std::chrono::steady_clock::now();
    auto r = run("while true do end", shortLimits(300));
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(r.result.status == ExecutionStatus::TimedOut);
    CHECK(elapsed < std::chrono::seconds(2));
    CHECK(elapsed >= std::chrono::milliseconds(290));
}

TEST_CASE(runtime_pcall_cannot_swallow_timeout) {
    auto r = run("while true do pcall(function() while true do end end) end", shortLimits(300));
    CHECK(r.result.status == ExecutionStatus::TimedOut);
    auto r2 = run("local function f() pcall(f) end while true do pcall(f) end", shortLimits(300));
    CHECK(r2.result.status == ExecutionStatus::TimedOut || r2.result.status == ExecutionStatus::RuntimeError);
}

TEST_CASE(runtime_memory_limit_is_enforced) {
    ExecutionLimits l;
    l.memoryLimitBytes = 16ull << 20;
    auto r = run("local t = {} for i = 1, 1e8 do t[i] = string.rep('x', 100) .. i end", l);
    CHECK(r.result.status == ExecutionStatus::MemoryLimit);
    CHECK(r.result.stats.peakMemoryBytes <= l.memoryLimitBytes);
    // A pcall that catches the memory error cannot keep the script alive.
    auto r2 = run("local ok = pcall(function() local s = string.rep('x', 64 * 1024 * 1024) end)\nprint('survived', ok)\nwhile true do end", l);
    CHECK(r2.result.status == ExecutionStatus::MemoryLimit);
    CHECK(r2.text().empty());
}

TEST_CASE(runtime_output_limit_is_enforced) {
    ExecutionLimits l;
    l.maxOutputBytes = 1000;
    auto r = run("for i = 1, 10000 do print(string.rep('y', 50)) end", l);
    CHECK(r.result.succeeded());
    CHECK(r.result.stats.outputBytes <= 1000);
    CHECK(r.result.stats.droppedOutputBytes > 0);
    CHECK(!r.text(OutputStream::System).empty());
}

TEST_CASE(runtime_cancellation_from_another_thread) {
    CancellationSource src;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        src.cancel();
    });
    auto start = std::chrono::steady_clock::now();
    auto r = run("local n = 0 while true do n = n + 1 end", shortLimits(10000), src.token());
    t.join();
    CHECK(r.result.status == ExecutionStatus::Cancelled);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE(runtime_cancellation_while_waiting) {
    CancellationSource src;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        src.cancel();
    });
    auto start = std::chrono::steady_clock::now();
    auto r = run("task.wait(30) print('unreachable')", shortLimits(60000), src.token());
    t.join();
    CHECK(r.result.status == ExecutionStatus::Cancelled);
    CHECK(r.text().empty());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE(runtime_task_scheduler_ordering) {
    auto r = run(R"(
local order = {}
local function log(x) table.insert(order, x) end
task.spawn(function(a) log("spawn " .. a) task.wait(0.05) log("spawn resumed") end, "arg")
task.defer(function() log("defer") end)
task.delay(0.02, function() log("delay") end)
log("main")
local dt = task.wait(0.1)
log("main resumed " .. tostring(dt >= 0.09))
print(table.concat(order, ","))
)");
    CHECK(r.result.succeeded());
    CHECK_EQ(r.text(), std::string("spawn arg,main,defer,delay,spawn resumed,main resumed true\n"));
    CHECK_EQ(r.result.stats.threadsCreated, uint32_t(3));
}

TEST_CASE(runtime_task_cancel_and_background_errors) {
    auto r = run(R"(
local t = task.delay(0.05, function() print("should not print") end)
task.cancel(t)
task.spawn(function() error("boom in task") end)
print("main done")
)");
    CHECK(r.result.succeeded());
    CHECK_EQ(r.text(), std::string("main done\n"));
    CHECK_EQ(r.result.stats.backgroundErrors, uint32_t(1));
    CHECK(r.text(OutputStream::Error).find("boom in task") != std::string::npos);
}

TEST_CASE(runtime_waits_for_scheduled_tasks_after_main) {
    auto r = run("task.delay(0.05, function() print('late') end)\nprint('first')");
    CHECK(r.result.succeeded());
    CHECK_EQ(r.text(), std::string("first\nlate\n"));
}

TEST_CASE(runtime_loadstring) {
    auto r = run("local f = loadstring('return 6 * 7')\nprint(f())\nlocal g, err = loadstring('return +')\nprint(g == nil, err ~= nil)");
    CHECK(r.result.succeeded());
    CHECK_EQ(r.text(), std::string("42\ntrue\ttrue\n"));
}

TEST_CASE(runtime_sandbox_protects_builtins) {
    auto r = run("math.floor = nil");
    CHECK(r.result.status == ExecutionStatus::RuntimeError);
    // _G follows Roblox semantics: writable, shared by the run's threads, separate from globals.
    auto r2 = run("x = 10\n_G.y = 5\ntask.spawn(function() print(x + _G.y, y) end)");
    CHECK(r2.result.succeeded());
    CHECK_EQ(r2.text(), std::string("15\tnil\n"));
    // Globals do not leak between runs.
    auto r3 = run("print(x, _G.y)");
    CHECK_EQ(r3.text(), std::string("nil\tnil\n"));
}

TEST_CASE(runtime_has_no_filesystem_or_process_access) {
    auto r = run("print(io, os.execute, os.exit, os.getenv, require, dofile, loadfile)");
    CHECK(r.result.succeeded());
    CHECK_EQ(r.text(), std::string("nil\tnil\tnil\tnil\tnil\tnil\tnil\n"));
}

TEST_CASE(runtime_roblox_globals_are_absent_not_faked) {
    auto r = run("print(game, workspace, Instance, Vector3, CFrame, getgenv)");
    CHECK(r.result.succeeded());
    CHECK_EQ(r.text(), std::string("nil\tnil\tnil\tnil\tnil\tnil\n"));
    auto r2 = run("local p = game:GetService('Players')");
    CHECK(r2.result.status == ExecutionStatus::RuntimeError);
}

TEST_CASE(runtime_repeated_execution_is_stable) {
    uint64_t firstPeak = 0;
    for (int i = 0; i < 200; ++i) {
        auto r = run("local t = {} for i = 1, 1000 do t[i] = i * 2 end print(#t)");
        REQUIRE(r.result.succeeded());
        if (i == 0) firstPeak = r.result.stats.peakMemoryBytes;
        CHECK(r.result.stats.peakMemoryBytes <= firstPeak + 4096); // no state carried between runs
    }
}

TEST_CASE(runtime_standalone_inventory_matches_vm) {
    CHECK(isStandaloneGlobal("print"));
    CHECK(isStandaloneGlobal("task"));
    CHECK(isStandaloneGlobal("buffer"));
    CHECK(!isStandaloneGlobal("game"));
    CHECK(!isStandaloneGlobal("getgenv"));
    const auto& members = standaloneLibraryMembers();
    REQUIRE(members.count("math"));
    CHECK(std::find(members.at("math").begin(), members.at("math").end(), "floor") != members.at("math").end());
    REQUIRE(members.count("task"));
    CHECK_EQ(members.at("task").size(), size_t(5));
}

TEST_CASE(runtime_result_json_roundtrip) {
    auto r = run("error('x')");
    auto back = ExecutionResult::fromJson(json::parse(json::dump(r.result.toJson())).value());
    CHECK(back.status == r.result.status);
    REQUIRE(back.error.has_value());
    CHECK_EQ(back.error->line, r.result.error->line);
    CHECK_EQ(back.summary, r.result.summary);
}
