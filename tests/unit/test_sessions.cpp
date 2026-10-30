// Unit tests for the session manager. Execution uses the real in-process Luau target; the
// "MOCK" disconnect test uses a simulated target to exercise stale-session handling.
#include "nexus_test.hpp"

#include "nexus/execution/local_targets.hpp"
#include "nexus/sessions/session_manager.hpp"

#include <atomic>
#include <mutex>
#include <thread>

using namespace nexus;

namespace {

struct Fixture {
    Logger logger;
    TargetRegistry targets;
    std::unique_ptr<SessionManager> sessions;
    std::mutex mutex;
    std::vector<SessionState> states;

    explicit Fixture(size_t consoleCapacity = 1000) {
        targets.add(std::make_shared<InProcessTarget>());
        sessions = std::make_unique<SessionManager>(logger, targets, consoleCapacity);
        sessions->subscribe([this](const SessionEvent& e) {
            if (e.kind == SessionEventKind::StateChanged) {
                std::lock_guard lock(mutex);
                states.push_back(e.snapshot.state);
            }
        });
    }

    ExecutionRequest request(const std::string& src, int timeoutMs = 5000) {
        ExecutionRequest r;
        r.source = src;
        r.scriptName = "T";
        r.limits.timeout = std::chrono::milliseconds(timeoutMs);
        return r;
    }
};

// A target whose connection can be dropped mid-run (simulated; not a real integration).
class MockFlakyTarget final : public ExecutionTarget {
public:
    std::atomic<uint64_t> gen{1};
    TargetInfo info() const override {
        TargetInfo i;
        i.id = "mock-flaky";
        i.displayName = "Mock flaky target";
        return i;
    }
    TargetAvailability availability() const override { return {true, "mock"}; }
    ExecutionResult execute(const ExecutionRequest&, const OutputCallback&, const CancellationToken& cancel) override {
        cancel.waitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(10));
        ExecutionResult r;
        r.status = ExecutionStatus::Cancelled;
        r.summary = "mock cancelled";
        return r;
    }
    uint64_t generation() const override { return gen.load(); }
};

} // namespace

TEST_CASE(session_runs_through_state_machine) {
    Fixture f;
    auto id = f.sessions->start(f.request("print('a') print('b')"), InProcessTarget::kId);
    REQUIRE(id.ok());
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(5)));
    auto snap = f.sessions->current();
    CHECK(snap.state == SessionState::Stopped);
    REQUIRE(snap.result.has_value());
    CHECK(snap.result->succeeded());
    std::lock_guard lock(f.mutex);
    std::vector<SessionState> expected = {SessionState::Initializing, SessionState::Compiling, SessionState::Ready,
                                          SessionState::Running, SessionState::Stopped};
    CHECK(f.states == expected);
    auto console = f.sessions->console();
    REQUIRE(console.size() >= 2);
    CHECK_EQ(console[0].second.text, std::string("a"));
    CHECK_EQ(console[1].second.text, std::string("b"));
}

TEST_CASE(session_rejects_concurrent_runs_and_stops) {
    Fixture f;
    REQUIRE(f.sessions->start(f.request("while true do end", 20000), InProcessTarget::kId).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto second = f.sessions->start(f.request("print(1)"), InProcessTarget::kId);
    CHECK(!second.ok());
    if (!second.ok()) CHECK_EQ(second.error().code, std::string("session.busy"));
    CHECK(f.sessions->busy());
    auto stopAt = std::chrono::steady_clock::now();
    CHECK(f.sessions->stop());
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(3)));
    CHECK(std::chrono::steady_clock::now() - stopAt < std::chrono::seconds(1));
    auto snap = f.sessions->current();
    CHECK(snap.state == SessionState::Stopped);
    CHECK(snap.result->status == ExecutionStatus::Cancelled);
    CHECK(!f.sessions->stop()); // nothing running
}

TEST_CASE(session_compile_errors_fail_without_running) {
    Fixture f;
    REQUIRE(f.sessions->start(f.request("print('x'\n"), InProcessTarget::kId).ok());
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(5)));
    auto snap = f.sessions->current();
    CHECK(snap.state == SessionState::Failed);
    CHECK(snap.result->status == ExecutionStatus::CompileError);
    std::lock_guard lock(f.mutex);
    CHECK(std::find(f.states.begin(), f.states.end(), SessionState::Running) == f.states.end());
}

TEST_CASE(session_runtime_error_and_unknown_target_fail) {
    Fixture f;
    REQUIRE(f.sessions->start(f.request("error('nope')"), InProcessTarget::kId).ok());
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(5)));
    CHECK(f.sessions->current().state == SessionState::Failed);
    REQUIRE(f.sessions->start(f.request("print(1)"), "does-not-exist").ok());
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(5)));
    CHECK(f.sessions->current().state == SessionState::Failed);
    CHECK(f.sessions->current().result->status == ExecutionStatus::Unavailable);
}

TEST_CASE(session_restart_and_history) {
    Fixture f;
    CHECK(!f.sessions->restart().ok()); // nothing to restart
    REQUIRE(f.sessions->start(f.request("print('run')"), InProcessTarget::kId).ok());
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(5)));
    auto again = f.sessions->restart();
    REQUIRE(again.ok());
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(5)));
    auto history = f.sessions->history();
    REQUIRE(history.size() == 2);
    CHECK(history[1].sessionId == again.value());
    auto back = ExecutionRecord::fromJson(json::parse(json::dump(history[0].toJson())).value());
    CHECK_EQ(back.sessionId, history[0].sessionId);
    CHECK(back.finalState == SessionState::Stopped);
}

TEST_CASE(session_console_is_bounded) {
    Fixture f(100);
    REQUIRE(f.sessions->start(f.request("for i = 1, 5000 do print(i) end"), InProcessTarget::kId).ok());
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(10)));
    auto console = f.sessions->console();
    CHECK_EQ(console.size(), size_t(100));
    CHECK(f.sessions->consoleSequence() > 5000);
    f.sessions->clearConsole();
    CHECK(f.sessions->console().empty());
}

TEST_CASE(session_MOCK_target_disconnect_marks_session_stale) {
    std::printf("    [MOCK] simulated target; validates session bookkeeping only\n");
    Fixture f;
    auto mock = std::make_shared<MockFlakyTarget>();
    f.targets.add(mock);
    REQUIRE(f.sessions->start(f.request("print(1)"), "mock-flaky").ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ++mock->gen;
    f.sessions->targetDisconnected("mock-flaky", "simulated connection loss");
    REQUIRE(f.sessions->waitForIdle(std::chrono::seconds(3)));
    auto snap = f.sessions->current();
    CHECK(snap.state == SessionState::Disconnected);
    CHECK(snap.result->summary.find("lost") != std::string::npos);
}

TEST_CASE(session_shutdown_stops_running_script) {
    Fixture f;
    REQUIRE(f.sessions->start(f.request("while true do end", 60000), InProcessTarget::kId).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto start = std::chrono::steady_clock::now();
    f.sessions->shutdown();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    CHECK(!f.sessions->busy());
    CHECK(!f.sessions->start(f.request("print(1)"), InProcessTarget::kId).ok()); // refused after shutdown
}
