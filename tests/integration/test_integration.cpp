// Integration tests. These spawn the real nexus-runner and nexus-cli executables. Studio tests use
// the MOCK MCP server (tests/support/mock_mcp_server.cpp) and are named accordingly.
#include "fixtures.hpp"
#include "nexus_test.hpp"

#include "nexus/execution/local_targets.hpp"
#include "nexus/host/nexus_host.hpp"
#include "nexus/library/script_library.hpp"
#include "nexus/platform/subprocess.hpp"
#include "nexus/platform/system.hpp"
#include "nexus/studio/studio_integration.hpp"

#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <thread>

using namespace nexus;
namespace fs = std::filesystem;

namespace {

std::string runner() { return nexus_test::binDir() + "/nexus-runner"; }
std::string cli() { return nexus_test::binDir() + "/nexus-cli"; }
std::string mockServer() { return nexus_test::binDir() + "/nexus-mock-mcp-server"; }

struct Captured {
    ExecutionResult result;
    std::vector<OutputEvent> output;
};

Captured runIsolated(const std::string& src, int timeoutMs = 5000, CancellationToken cancel = {}) {
    IsolatedTarget target(runner());
    ExecutionRequest req;
    req.source = src;
    req.scriptName = "Iso";
    req.limits.timeout = std::chrono::milliseconds(timeoutMs);
    req.limits.killGracePeriod = std::chrono::milliseconds(300);
    Captured c;
    c.result = target.execute(req, [&](const OutputEvent& e) { c.output.push_back(e); }, cancel);
    return c;
}

struct CliResult {
    int exitCode = -1;
    std::string out;
    std::string err;
};

CliResult runCli(const std::vector<std::string>& args, const std::vector<std::string>& env = {}) {
    platform::SpawnOptions o;
    o.executable = cli();
    o.arguments = args;
    o.environment = env;
    std::string error;
    auto p = platform::Subprocess::spawn(o, &error);
    CliResult r;
    if (!p) {
        r.err = error;
        return r;
    }
    p->closeStdin();
    bool outOpen = true, errOpen = true;
    while (outOpen || errOpen) {
        pollfd fds[2] = {{outOpen ? p->stdoutFd() : -1, POLLIN, 0}, {errOpen ? p->stderrFd() : -1, POLLIN, 0}};
        if (poll(fds, 2, 30000) <= 0) break;
        if (fds[0].revents) outOpen = platform::readAvailable(p->stdoutFd(), r.out);
        if (fds[1].revents) errOpen = platform::readAvailable(p->stderrFd(), r.err);
    }
    auto st = p->waitFor(std::chrono::seconds(30));
    r.exitCode = st && st->exited ? st->code : -1;
    return r;
}

std::string writeScript(const std::string& name, const std::string& content) {
    std::string path = nexus_test::tempDir() + "/" + name;
    (void)writeFileAtomic(path, content);
    return path;
}

} // namespace

// ---- Isolated runner (real process) ----------------------------------------------------------

TEST_CASE(isolated_runner_executes_script) {
    auto c = runIsolated("print('from runner')\ntask.wait(0.02)\nprint(1 + 2)");
    CHECK(c.result.succeeded());
    CHECK_EQ(c.result.target, std::string("luau-isolated"));
    std::string text;
    for (const auto& e : c.output)
        if (e.stream == OutputStream::Output) text += e.text + "\n";
    CHECK_EQ(text, std::string("from runner\n3\n"));
    bool sawReady = false;
    for (const auto& e : c.output)
        if (e.stream == OutputStream::Info && e.text.find("Isolated runner started") != std::string::npos) sawReady = true;
    CHECK(sawReady);
}

TEST_CASE(isolated_runner_reports_errors_and_limits) {
    auto err = runIsolated("local x = nil\nprint(x.y)");
    CHECK(err.result.status == ExecutionStatus::RuntimeError);
    REQUIRE(err.result.error.has_value());
    CHECK_EQ(err.result.error->line, uint32_t(2));
    auto syntax = runIsolated("print(");
    CHECK(syntax.result.status == ExecutionStatus::CompileError);
    auto timeout = runIsolated("while true do end", 400);
    CHECK(timeout.result.status == ExecutionStatus::TimedOut);
}

TEST_CASE(isolated_runner_stop_is_prompt) {
    CancellationSource src;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        src.cancel();
    });
    auto start = std::chrono::steady_clock::now();
    auto c = runIsolated("while true do end", 60000, src.token());
    t.join();
    CHECK(c.result.status == ExecutionStatus::Cancelled);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE(isolated_runner_kills_script_stuck_in_native_code) {
    // A pathological pattern keeps the VM inside one native call, where the interrupt cannot fire.
    // The host must still end the run (SIGKILL backstop) shortly after timeout + grace period.
    auto start = std::chrono::steady_clock::now();
    auto c = runIsolated("local s = string.rep('a', 200000)\nprint(s:find('.-.-.-.-.-.-.-.-.-.-b'))", 500);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(c.result.status == ExecutionStatus::TimedOut);
    CHECK(elapsed < std::chrono::seconds(6));
}

TEST_CASE(isolated_runner_repeated_runs_leave_no_processes) {
    for (int i = 0; i < 25; ++i) {
        auto c = runIsolated("print(" + std::to_string(i) + ")");
        REQUIRE(c.result.succeeded());
    }
    int stray = 0;
    for (const auto& p : platform::listProcesses())
        if (p.executablePath == fs::weakly_canonical(runner()).string()) ++stray;
    CHECK_EQ(stray, 0);
}

TEST_CASE(isolated_runner_sandbox_is_measured_not_assumed) {
    platform::SpawnOptions o;
    o.executable = runner();
    o.arguments = {"--probe-sandbox"};
    std::string err;
    auto p = platform::Subprocess::spawn(o, &err);
    REQUIRE(p.has_value());
    std::string out;
    while (platform::readAvailable(p->stdoutFd(), out)) {
    }
    p->wait();
    auto v = json::parse(out);
    REQUIRE(v.ok());
#ifdef __APPLE__
    CHECK(v.value().getBool("sandboxApplied"));
    CHECK(v.value().getBool("fileWriteBlocked"));
    CHECK(v.value().getBool("networkBlocked"));
#else
    CHECK(!v.value().getBool("sandboxApplied")); // honest: no OS sandbox on this platform
#endif
    std::printf("    sandbox probe: %s\n", out.c_str());
}

TEST_CASE(isolated_target_reports_missing_runner) {
    IsolatedTarget target("/nonexistent/nexus-runner");
    CHECK(!target.availability().available);
    auto r = target.execute(ExecutionRequest{}, nullptr, {});
    CHECK(r.status == ExecutionStatus::Unavailable);
}

// ---- Host + CLI ------------------------------------------------------------------------------

TEST_CASE(cli_run_check_analyze_format) {
    std::string data = nexus_test::tempDir() + "/cli-data";
    std::string ok = writeScript("ok.luau", "print('cli ok')");
    auto r = runCli({"run", ok, "--data-dir", data});
    CHECK_EQ(r.exitCode, 0);
    CHECK(r.out.find("cli ok") != std::string::npos);

    auto bad = runCli({"run", writeScript("bad.luau", "error('cli failure')"), "--data-dir", data});
    CHECK_EQ(bad.exitCode, 1);
    CHECK(bad.err.find("cli failure") != std::string::npos);

    auto slow = runCli({"run", writeScript("loop.luau", "while true do end"), "--timeout", "300", "--data-dir", data});
    CHECK_EQ(slow.exitCode, 124);

    auto syntax = runCli({"check", writeScript("syn.luau", "local = 1")});
    CHECK_EQ(syntax.exitCode, 1);

    auto analyze = runCli({"analyze", writeScript("rbx.luau", "print(game:GetService('Players'))"), "--json"});
    CHECK_EQ(analyze.exitCode, 0);
    auto report = json::parse(analyze.out);
    REQUIRE(report.ok());
    CHECK_EQ(report.value().get("services")->asArray().front().asString(), std::string("Players"));

    std::string fmt = writeScript("fmt.luau", "if x then\nprint(1)\nend\n");
    CHECK_EQ(runCli({"format", fmt, "--write"}).exitCode, 0);
    CHECK_EQ(readFile(fmt).value(), std::string("if x then\n    print(1)\nend\n"));

    // History was persisted by the host.
    CHECK(fs::exists(data + "/history.json"));
    auto player = runCli({"run", ok, "--target", "roblox-player", "--data-dir", data});
    CHECK_EQ(player.exitCode, 3); // unavailable, never "success"
}

TEST_CASE(cli_inspect_synthetic_binary) {
    nexus_test::SyntheticSlice s;
    std::string path = writeScript("synthetic.bin", nexus_test::buildMachO(s));
    auto r = runCli({"inspect", path, "--json"});
    CHECK_EQ(r.exitCode, 0);
    auto v = json::parse(r.out);
    REQUIRE(v.ok());
    CHECK(v.value().get("securityPosture")->getBool("hardenedRuntime"));
}

TEST_CASE(cli_detects_simulated_running_player_without_attaching) {
    std::printf("    [SIMULATED CLIENT] a copy of /bin/sleep inside a fixture bundle named RobloxPlayer\n");
    std::string apps = nexus_test::tempDir() + "/Applications";
    fs::create_directories(apps);
    auto sleepBinary = readFile("/bin/sleep", 64ull << 20);
    REQUIRE(sleepBinary.ok());
    std::string bundle = nexus_test::makeBundle(apps, "Roblox.app", "com.roblox.RobloxPlayer", "0.745.0.7451137", "RobloxPlayer", sleepBinary.value());
    std::string err;
    auto proc = platform::Subprocess::spawn({bundle + "/Contents/MacOS/RobloxPlayer", {"30"}, {}, false, "", true}, &err);
    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<std::string> env = {"NEXUS_ROBLOX_SEARCH_PATHS=" + apps};
    auto r = runCli({"roblox", "--json", "--ephemeral"}, env);
    CHECK_EQ(r.exitCode, 0);
    auto v = json::parse(r.out);
    REQUIRE(v.ok());
    const json::Value* player = v.value().get("player");
    REQUIRE(player);
    CHECK_EQ(player->getString("state"), std::string("UNSUPPORTED"));
    CHECK(!player->getBool("liveExecutionAvailable", true));
    REQUIRE(!player->get("running")->asArray().empty());
    CHECK_EQ(player->get("running")->asArray()[0].getInt("pid"), int64_t(proc->pid()));
    CHECK_EQ(player->get("installed")->getString("version"), std::string("0.745.0.7451137"));
    // The simulated client is untouched by detection.
    CHECK(platform::processExists(proc->pid()));
    CHECK(!proc->tryWait().has_value());
    proc->kill();
    proc->wait();

    auto after = runCli({"roblox", "--json", "--ephemeral"}, env);
    auto v2 = json::parse(after.out);
    REQUIRE(v2.ok());
    CHECK(v2.value().get("player")->get("running")->asArray().empty());
}

TEST_CASE(host_end_to_end_with_library_and_diagnostics) {
    HostOptions o;
    o.dataDirectory = nexus_test::tempDir() + "/host";
    o.runnerPath = runner();
    {
        NexusHost host(o);
        auto saved = host.library().createScript("", "Example", "print('library script')");
        REQUIRE(saved.ok());
        auto src = host.library().read(saved.value());
        REQUIRE(src.ok());
        REQUIRE(host.run(src.value(), "Example").ok());
        REQUIRE(host.sessions().waitForIdle(std::chrono::seconds(10)));
        CHECK(host.sessions().current().result->succeeded());
        auto report = host.analyze("print(workspace)");
        CHECK(!report.findings.empty());
        std::string exportPath = nexus_test::tempDir() + "/diag.json";
        REQUIRE(host.exportDiagnostics(exportPath).ok());
        auto diag = json::parse(readFile(exportPath).value());
        REQUIRE(diag.ok());
        CHECK_EQ(diag.value().getString("format"), std::string("nexus-diagnostics-v1"));
        CHECK(!diag.value().get("logs")->asArray().empty());
    }
    NexusHost reopened(o); // history restored
    CHECK(!reopened.sessions().history().empty());
}

// ---- Studio integration against the MOCK MCP server ------------------------------------------

namespace {
struct StudioHarness {
    Logger logger;
    roblox::SystemClientEnvironment env;
    roblox::VersionHistory versions;
    std::shared_ptr<studio::StudioIntegration> studio;

    explicit StudioHarness(const std::string& mode) {
        setenv("MOCK_MCP_MODE", mode.c_str(), 1);
        studio = std::make_shared<studio::StudioIntegration>(logger, env, versions);
        studio::StudioOptions o;
        o.serverOverride = mockServer();
        o.requireRobloxSignature = true; // overrides skip signature checks by design (test-only path)
        studio->setOptions(o);
    }
};

ExecutionRequest studioRequest(const std::string& src, int timeoutMs = 5000) {
    ExecutionRequest r;
    r.source = src;
    r.limits.timeout = std::chrono::milliseconds(timeoutMs);
    r.options["dataModel"] = "server";
    return r;
}
} // namespace

TEST_CASE(studio_MOCK_connects_maps_schema_and_executes) {
    std::printf("    [MOCK] MCP server imitating Studio's documented tool names; not real Roblox Studio\n");
    StudioHarness h("normal");
    auto refreshed = h.studio->refresh();
    CHECK(refreshed.state == roblox::ConnectionState::IntegrationAvailable);
    CHECK(!h.studio->availability().available); // not connected until the user connects
    auto st = h.studio->connect();
    if (st.state != roblox::ConnectionState::Connected) std::printf("    status: %s — %s\n", roblox::toString(st.state), st.message.c_str());
    REQUIRE(st.state == roblox::ConnectionState::Connected);
    CHECK_EQ(st.serverName, std::string("MockStudioMCP"));
    CHECK_EQ(st.mapping.tool, std::string("execute_luau"));
    CHECK_EQ(st.mapping.codeParam, std::string("code"));
    CHECK_EQ(st.mapping.dataModelParam, std::string("datamodel_type"));
    CHECK(st.mapping.studioIdRequired);
    REQUIRE(st.studios.size() == 1);
    CHECK_EQ(st.selectedStudioId, std::string("studio-1"));

    std::vector<OutputEvent> out;
    auto r = h.studio->execute(studioRequest("print('hi')"), [&](const OutputEvent& e) { out.push_back(e); }, {});
    CHECK(r.succeeded());
    bool echoed = false;
    for (const auto& e : out)
        if (e.text.find("MOCK received 11 bytes for Server on studio-1") != std::string::npos) echoed = true; // enum value normalised
    CHECK(echoed);
    auto failing = h.studio->execute(studioRequest("error('x')"), nullptr, {});
    CHECK(failing.status == ExecutionStatus::RuntimeError);
    h.studio->disconnect();
    CHECK(!h.studio->availability().available);
}

TEST_CASE(studio_MOCK_schema_changes_are_reported_unsupported) {
    StudioHarness noexec("no-exec");
    auto a = noexec.studio->connect();
    CHECK(a.state == roblox::ConnectionState::Unsupported);
    CHECK(a.message.find("execute_luau") != std::string::npos);
    StudioHarness changed("changed-schema");
    auto b = changed.studio->connect();
    CHECK(b.state == roblox::ConnectionState::Unsupported);
    CHECK(b.message.find("sandbox_token") != std::string::npos);
    StudioHarness none("no-studios");
    auto c = none.studio->connect();
    CHECK(c.state == roblox::ConnectionState::IntegrationAvailable); // running, but no Studio instance
    CHECK(!none.studio->availability().available);
}

TEST_CASE(studio_MOCK_cancel_timeout_and_crash) {
    StudioHarness slow("slow");
    REQUIRE(slow.studio->connect().state == roblox::ConnectionState::Connected);
    auto timedOut = slow.studio->execute(studioRequest("print(1)", 300), nullptr, {});
    CHECK(timedOut.status == ExecutionStatus::TimedOut);
    CHECK(timedOut.summary.find("may still be running") != std::string::npos); // no false claim of termination
    CancellationSource src;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        src.cancel();
    });
    auto cancelled = slow.studio->execute(studioRequest("print(1)", 10000), nullptr, src.token());
    t.join();
    CHECK(cancelled.status == ExecutionStatus::Cancelled);
    CHECK(cancelled.summary.find("does not guarantee") != std::string::npos);

    StudioHarness crash("crash-on-exec");
    REQUIRE(crash.studio->connect().state == roblox::ConnectionState::Connected);
    std::atomic<bool> notified{false};
    crash.studio->setDisconnectHandler([&](const std::string&) { notified = true; });
    uint64_t gen = crash.studio->generation();
    auto r = crash.studio->execute(studioRequest("print(1)"), nullptr, {});
    CHECK(r.status == ExecutionStatus::InternalError);
    for (int i = 0; i < 100 && !notified; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(notified.load());
    CHECK(crash.studio->generation() != gen); // sessions on the old connection become stale
    CHECK(crash.studio->status().state == roblox::ConnectionState::Error);
}

TEST_CASE(studio_mapping_rules) {
    using studio::McpTool;
    McpTool legacy{"run_code", "", json::parse(R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})").value()};
    auto m = studio::mapExecutionTool({legacy});
    CHECK(m.usable());
    CHECK_EQ(m.codeParam, std::string("command"));
    CHECK(!studio::mapExecutionTool({}).usable());
    McpTool odd{"execute_luau", "", json::parse(R"({"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}},"required":["a","b"]})").value()};
    CHECK(!studio::mapExecutionTool({odd}).usable()); // ambiguous: refuse rather than guess
    studio::McpToolResult listed;
    listed.text = R"({"studios":[{"id":"s1","name":"Place A"},{"studioId":"s2"}]})";
    CHECK_EQ(studio::parseStudioList(listed).size(), size_t(2));
    listed.text = "not json";
    CHECK(studio::parseStudioList(listed).empty());
}
