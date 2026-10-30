// Unit tests for Roblox client discovery. Binaries and bundles are synthetic fixtures and process
// lists come from a MOCK environment; these tests validate parsing and state logic only. They do
// not demonstrate anything about live Roblox behaviour.
#include "fixtures.hpp"
#include "nexus_test.hpp"

#include "nexus/platform/subprocess.hpp"
#include "nexus/platform/system.hpp"
#include "nexus/roblox/connection.hpp"
#include "nexus/roblox/macho.hpp"
#include "nexus/roblox/plist.hpp"

#include <cstdlib>
#include <filesystem>
#include <unistd.h>

using namespace nexus;
using namespace nexus::roblox;
namespace fs = std::filesystem;

namespace {

const char* kEntitlements =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
    "<key>com.apple.security.cs.disable-executable-page-protection</key><true/>"
    "<key>com.apple.security.device.camera</key><true/></dict></plist>";

class MockEnvironment final : public ClientEnvironment {
public:
    std::string appsDir;
    std::vector<platform::ProcessEntry> procs;
    std::vector<std::string> candidateBundlePaths(const ProductDescriptor& p) override {
        std::vector<std::string> out;
        for (const char* n : p.bundleNames) out.push_back(appsDir + "/" + n);
        return out;
    }
    std::vector<platform::ProcessEntry> processes() override { return procs; }
    std::optional<bool> isTranslated(pid_t) override { return false; }
    platform::SignatureCheck verifySignature(const std::string&, const std::string&) override {
        platform::SignatureCheck c;
        c.error = "mock environment";
        return c;
    }
};

} // namespace

TEST_CASE(plist_parses_dicts_arrays_and_entities) {
    auto v = parseXmlPlist(R"(<?xml version="1.0"?><!DOCTYPE plist><plist version="1.0"><dict>
        <!-- comment --><key>Name</key><string>A &amp; B &lt;&#x263A;&gt;</string>
        <key>Count</key><integer>42</integer><key>Ratio</key><real>0.5</real>
        <key>Flag</key><false/><key>Empty</key><string/>
        <key>List</key><array><string>x</string><dict/></array>
        <key>Blob</key><data>AAEC
        Aw==</data></dict></plist>)");
    REQUIRE(v.ok());
    CHECK_EQ(v.value().getString("Name"), std::string("A & B <☺>"));
    CHECK_EQ(v.value().getInt("Count"), int64_t(42));
    CHECK_EQ(v.value().getNumber("Ratio"), 0.5);
    CHECK(!v.value().getBool("Flag", true));
    CHECK_EQ(v.value().get("List")->asArray().size(), size_t(2));
    CHECK_EQ(v.value().getString("Blob"), std::string("AAECAw=="));
    for (const char* bad : {"", "<plist>", "<plist><dict><string>x</string></dict></plist>", "<plist><foo/></plist>", "<plist><string>&bogus;</string></plist>"})
        CHECK(!parseXmlPlist(bad).ok());
}

TEST_CASE(macho_parses_thin_signed_binary) {
    nexus_test::SyntheticSlice s;
    s.entitlementsXml = kEntitlements;
    auto info = inspectMachOBytes(nexus_test::buildMachO(s));
    REQUIRE(info.ok());
    CHECK(!info.value().universal);
    REQUIRE(info.value().slices.size() == 1);
    const auto& slice = info.value().slices[0];
    CHECK_EQ(slice.architecture, std::string("arm64"));
    CHECK_EQ(slice.minimumOS, std::string("11.0"));
    CHECK_EQ(slice.sdk, std::string("26.2"));
    CHECK_EQ(slice.linkedLibraryCount, uint32_t(1));
    CHECK(slice.signature.present);
    CHECK_EQ(slice.signature.identifier, std::string("com.example.synthetic"));
    CHECK_EQ(slice.signature.teamId, std::string("TEAMID1234"));
    CHECK(slice.signature.hardenedRuntime);
    CHECK(slice.signature.entitlements.count("com.apple.security.device.camera"));
    auto posture = evaluateSecurityPosture(slice.signature);
    CHECK(posture.hardenedRuntime);
    CHECK(!posture.getTaskAllow);
    CHECK(!posture.allowDyldEnvironment);
    CHECK(posture.disableExecutablePageProtection);
}

TEST_CASE(macho_parses_universal_binary) {
    nexus_test::SyntheticSlice arm, x86;
    x86.cpuType = 0x01000007;
    x86.signature = false;
    auto info = inspectMachOBytes(nexus_test::buildUniversal({arm, x86}));
    REQUIRE(info.ok());
    CHECK(info.value().universal);
    CHECK(info.value().architectures() == (std::vector<std::string>{"arm64", "x86_64"}));
    CHECK(!info.value().slices[1].signature.present);
}

TEST_CASE(macho_rejects_malformed_input_without_crashing) {
    std::string good = nexus_test::buildMachO({});
    CHECK(!inspectMachOBytes("").ok());
    CHECK(!inspectMachOBytes("not a binary at all").ok());
    CHECK(!inspectMachOBytes(std::string("\xca\xfe\xba\xbe\x00\x00\x00\x2d", 8)).ok()); // Java class file
    // Every truncation and many single-byte corruptions must fail gracefully.
    for (size_t n = 0; n < good.size(); n += 7) (void)inspectMachOBytes(good.substr(0, n));
    for (size_t i = 0; i < good.size(); i += 3) {
        std::string bad = good;
        bad[i] = static_cast<char>(bad[i] ^ 0xff);
        (void)inspectMachOBytes(bad);
    }
    CHECK(true);
}

TEST_CASE(macho_inspects_real_roblox_binary_if_provided) {
    const char* path = std::getenv("NEXUS_TEST_ROBLOX_BINARY");
    if (!path || !*path) SKIP("set NEXUS_TEST_ROBLOX_BINARY to a RobloxPlayer executable to run this check");
    auto info = inspectMachO(path);
    REQUIRE(info.ok());
    REQUIRE(!info.value().slices.empty());
    CHECK_EQ(info.value().slices[0].signature.teamId, std::string(kRobloxTeamId));
    CHECK(info.value().slices[0].signature.hardenedRuntime);
}

TEST_CASE(connection_state_machine_enforces_rules) {
    ConnectionStateMachine player(false);
    CHECK(player.transition(ConnectionState::Discovering, "scan"));
    CHECK(player.transition(ConnectionState::ClientFound, "found"));
    CHECK(player.transition(ConnectionState::CheckingCompatibility, "check"));
    CHECK(!player.transition(ConnectionState::IntegrationAvailable, "must be rejected"));
    CHECK(!player.transition(ConnectionState::Connected, "must be rejected"));
    CHECK(player.transition(ConnectionState::Unsupported, "no interface"));
    CHECK(player.state() == ConnectionState::Unsupported);
    CHECK(!player.transition(ConnectionState::Connected, "still rejected"));
    CHECK(player.transition(ConnectionState::Disconnected, "closed"));

    ConnectionStateMachine studio(true);
    CHECK(!studio.transition(ConnectionState::Connected, "cannot skip discovery"));
    CHECK(studio.transition(ConnectionState::Discovering, ""));
    CHECK(studio.transition(ConnectionState::ClientFound, ""));
    CHECK(studio.transition(ConnectionState::CheckingCompatibility, ""));
    CHECK(studio.transition(ConnectionState::IntegrationAvailable, ""));
    CHECK(studio.transition(ConnectionState::Connected, ""));
    CHECK_EQ(studio.history().size(), size_t(5));
}

TEST_CASE(versions_compare_and_history_detects_updates) {
    CHECK(compareVersions("0.745.0.7451137", "0.744.0.7440912") > 0);
    CHECK(compareVersions("0.745.0", "0.745.0.0") == 0);
    CHECK(compareVersions("1.2", "1.10") < 0);
    std::string file = nexus_test::tempDir() + "/versions.json";
    {
        VersionHistory h(file);
        CHECK(!h.record(Product::Player, "0.744.0.1").has_value());
        CHECK(!h.record(Product::Player, "0.744.0.1").has_value());
    }
    VersionHistory reopened(file);
    auto prev = reopened.record(Product::Player, "0.745.0.2");
    REQUIRE(prev.has_value());
    CHECK_EQ(*prev, std::string("0.744.0.1"));
    auto remote = parseClientVersionResponse(R"({"version":"0.745.0.7451137","clientVersionUpload":"version-b09c93298b5d4cd1","bootstrapperVersion":""})");
    REQUIRE(remote.ok());
    CHECK_EQ(remote.value().version, std::string("0.745.0.7451137"));
    CHECK(!parseClientVersionResponse("<html>").ok());
}

TEST_CASE(player_monitor_MOCK_detection_never_reports_attachment) {
    std::printf("    [MOCK] synthetic bundle + simulated process list\n");
    Logger logger;
    auto env = std::make_shared<MockEnvironment>();
    env->appsDir = nexus_test::tempDir() + "/apps-player";
    fs::create_directories(env->appsDir);
    VersionHistory versions;
    PlayerMonitor monitor(logger, *env, versions);

    auto st = monitor.refresh();
    CHECK(st.state == ConnectionState::Disconnected);
    CHECK(!st.installed.has_value());

    nexus_test::SyntheticSlice slice;
    slice.teamId = kRobloxTeamId;
    slice.identifier = "com.roblox.RobloxPlayer";
    std::string bundle = nexus_test::makeBundle(env->appsDir, "Roblox.app", "com.roblox.RobloxPlayer", "0.745.0.7451137", "RobloxPlayer",
                                                nexus_test::buildMachO(slice));
    st = monitor.refresh();
    CHECK(st.state == ConnectionState::Unsupported);
    REQUIRE(st.installed.has_value());
    CHECK_EQ(st.installed->version, std::string("0.745.0.7451137"));
    CHECK(st.running.empty());
    CHECK(!st.unsupportedReasons.empty());

    env->procs.push_back({4242, bundle + "/Contents/MacOS/RobloxPlayer"});
    uint64_t genBefore = st.generation;
    st = monitor.refresh();
    REQUIRE(st.running.size() == 1);
    CHECK_EQ(st.running[0].pid, pid_t(4242));
    CHECK(st.state == ConnectionState::Unsupported); // running != attached
    CHECK(st.generation != genBefore);

    env->procs = {{5151, bundle + "/Contents/MacOS/RobloxPlayer"}}; // restarted with a new pid
    uint64_t genRunning = st.generation;
    st = monitor.refresh();
    CHECK(st.generation != genRunning);
    for (const auto& t : st.history) CHECK(t.to != ConnectionState::Connected && t.to != ConnectionState::IntegrationAvailable);

    PlayerTarget target(monitor);
    CHECK(!target.availability().available);
    auto r = target.execute(ExecutionRequest{}, nullptr, {});
    CHECK(r.status == ExecutionStatus::Unavailable);
}

TEST_CASE(bundle_inspection_rejects_wrong_products) {
    MockEnvironment env;
    std::string dir = nexus_test::tempDir() + "/apps-wrong";
    std::string b = nexus_test::makeBundle(dir, "Roblox.app", "com.example.NotRoblox", "1.0", "RobloxPlayer", nexus_test::buildMachO({}));
    CHECK(!inspectBundle(b, Product::Player, env).ok());
    CHECK(!inspectBundle(dir + "/Missing.app", Product::Player, env).ok());
}

TEST_CASE(platform_subprocess_roundtrip_and_kill) {
    std::string err;
    auto p = platform::Subprocess::spawn({"/bin/cat", {}, {}, false, "", true}, &err);
    REQUIRE(p.has_value());
    REQUIRE(p->writeStdin("hello\n"));
    std::string out;
    while (out.find('\n') == std::string::npos && platform::readAvailable(p->stdoutFd(), out)) {
    }
    CHECK_EQ(out, std::string("hello\n"));
    p->closeStdin();
    auto status = p->waitFor(std::chrono::seconds(2));
    REQUIRE(status.has_value());
    CHECK(status->exited && status->code == 0);

    auto sleeper = platform::Subprocess::spawn({"/bin/sleep", {"30"}, {}, false, "", true}, &err);
    REQUIRE(sleeper.has_value());
    pid_t pid = sleeper->pid();
    CHECK(platform::processExists(pid));
    sleeper->kill();
    auto killed = sleeper->waitFor(std::chrono::seconds(2));
    REQUIRE(killed.has_value());
    CHECK(killed->signaled);
    CHECK(!platform::processExists(pid)); // reaped, no zombie
    CHECK(!platform::Subprocess::spawn({"/nonexistent/binary", {}, {}, false, "", true}, &err).has_value());
}

TEST_CASE(platform_process_list_and_paths) {
    auto procs = platform::listProcesses();
    bool foundSelf = false;
    for (const auto& p : procs)
        if (p.pid == getpid()) foundSelf = true;
    CHECK(foundSelf);
    CHECK(!platform::executableDirectory().empty());
    CHECK(!platform::operatingSystemDescription().empty());
    CHECK(platform::applicationSupportDirectory("Nexus").find("Nexus") != std::string::npos);
}
