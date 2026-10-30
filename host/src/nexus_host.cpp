#include "nexus/host/nexus_host.hpp"

#include "nexus/common/strings.hpp"
#include "nexus/compiler/compiler.hpp"
#include "nexus/platform/system.hpp"

#ifndef NEXUS_VERSION
#define NEXUS_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace nexus {

NexusHost::NexusHost(HostOptions options) : options_(std::move(options)), startedAt_(std::chrono::system_clock::now()) {
    dataDir_ = options_.dataDirectory.empty() ? fs::path(platform::applicationSupportDirectory("Nexus")) : options_.dataDirectory;
    std::error_code ec;
    fs::create_directories(dataDir_, ec);
    if (options_.persistent) {
        if (auto r = logger_.enableFileSink(dataDir_ / "Logs" / "nexus.log"); !r.ok()) logger_.warning("host", r.error().message);
    }
    logger_.log(LogLevel::Info, "host", "Nexus " NEXUS_VERSION " starting",
                {{"os", platform::operatingSystemDescription()}, {"luau", luauVersion()}, {"data", dataDir_.string()}});

    runnerPath_ = options_.runnerPath.empty() ? findRunnerExecutable() : options_.runnerPath;
    env_ = options_.environment ? options_.environment : std::make_shared<roblox::SystemClientEnvironment>();
    versions_ = std::make_unique<roblox::VersionHistory>(options_.persistent ? dataDir_ / "roblox-versions.json" : fs::path());
    settingsStore_ = std::make_unique<SettingsStore>(dataDir_ / "settings.json");
    settings_ = options_.persistent ? settingsStore_->load() : Settings{};

    library_ = std::make_unique<ScriptLibrary>(dataDir_ / "Library");
    if (auto r = library_->initialize(); !r.ok()) logger_.error("library", r.error().message);
    drafts_ = std::make_unique<DraftStore>(dataDir_ / "Drafts");

    player_ = std::make_unique<roblox::PlayerMonitor>(logger_, *env_, *versions_);
    studio_ = std::make_shared<studio::StudioIntegration>(logger_, *env_, *versions_);

    targets_.add(std::make_shared<IsolatedTarget>(runnerPath_, settings_.runnerSandbox));
    targets_.add(std::make_shared<InProcessTarget>());
    targets_.add(studio_);
    targets_.add(std::make_shared<roblox::PlayerTarget>(*player_));

    sessions_ = std::make_unique<SessionManager>(logger_, targets_, static_cast<size_t>(settings_.consoleCapacity));
    studio_->setDisconnectHandler([this](const std::string& reason) { sessions_->targetDisconnected(studio::StudioIntegration::kId, reason); });
    sessions_->subscribe([this](const SessionEvent& e) {
        if (e.kind == SessionEventKind::Finished) saveHistory();
    });

    applySettings(settings_);
    loadHistory();
    if (runnerPath_.empty()) logger_.warning("host", "nexus-runner was not found; the isolated target is unavailable");
}

NexusHost::~NexusHost() {
    sessions_->shutdown();
    saveHistory();
    studio_->disconnect("Nexus is closing");
    logger_.info("host", "Nexus shut down cleanly");
}

Settings NexusHost::settings() const {
    std::lock_guard lock(settingsMutex_);
    return settings_;
}

void NexusHost::applySettings(const Settings& s) {
    if (auto iso = std::dynamic_pointer_cast<IsolatedTarget>(targets_.find(IsolatedTarget::kId))) iso->setSandboxEnabled(s.runnerSandbox);
    studio::StudioOptions so = studio_->options();
    so.requireRobloxSignature = s.requireRobloxSignature;
    so.dataModel = s.studioDataModel;
    studio_->setOptions(so);
}

Result<void> NexusHost::updateSettings(const Settings& settings) {
    Settings validated = Settings::fromJson(settings.toJson()); // re-validate / clamp
    {
        std::lock_guard lock(settingsMutex_);
        settings_ = validated;
    }
    applySettings(validated);
    logger_.info("settings", "Settings updated");
    if (!options_.persistent) return success();
    return settingsStore_->save(validated);
}

Result<uint64_t> NexusHost::run(const std::string& source, const std::string& scriptName, const std::string& targetId, json::Object options) {
    Settings s = settings();
    ExecutionRequest request;
    request.source = source;
    request.scriptName = scriptName.empty() ? "Script" : scriptName;
    request.limits = s.limits();
    if (!options.contains("dataModel")) options["dataModel"] = s.studioDataModel;
    request.options = std::move(options);
    return sessions_->start(std::move(request), targetId.empty() ? s.defaultTarget : targetId);
}

compat::CompatibilityReport NexusHost::analyze(const std::string& source) const { return compat::analyzeCompatibility(source); }

roblox::PlayerStatus NexusHost::refreshRoblox() { return player_->refresh(); }

studio::StudioStatus NexusHost::refreshStudio() { return studio_->refresh(); }

studio::StudioStatus NexusHost::connectStudio() {
    Settings s = settings();
    if (!s.studioIntegrationEnabled) {
        logger_.info("studio", "Studio connection requested; integration consent recorded");
        s.studioIntegrationEnabled = true;
        (void)updateSettings(s);
    }
    return studio_->connect();
}

void NexusHost::disconnectStudio() {
    studio_->disconnect();
    sessions_->targetDisconnected(studio::StudioIntegration::kId, "Disconnected by user");
}

json::Value NexusHost::diagnosticsSnapshot() const {
    json::Array targets;
    for (const auto& t : targets_.list()) {
        json::Value info = t->info().toJson();
        TargetAvailability a = t->availability();
        info["available"] = a.available;
        info["availability"] = a.reason;
        targets.push_back(std::move(info));
    }
    auto history = sessions_->history();
    uint64_t ok = 0, failed = 0;
    double totalMs = 0;
    for (const auto& h : history) {
        if (h.result.succeeded()) ++ok;
        else ++failed;
        totalMs += h.result.stats.wallMs;
    }
    json::Object logCounts;
    for (const auto& [level, count] : logger_.countsByLevel()) logCounts[toString(level)] = count;
    double uptime = std::chrono::duration<double>(std::chrono::system_clock::now() - startedAt_).count();
    return json::Object{
        {"nexusVersion", NEXUS_VERSION},
        {"luauVersion", luauVersion()},
        {"os", platform::operatingSystemDescription()},
        {"architecture", platform::machineArchitecture()},
        {"dataDirectory", dataDir_.string()},
        {"runnerPath", runnerPath_},
        {"uptimeSeconds", uptime},
        {"targets", std::move(targets)},
        {"settings", settings().toJson()},
        {"robloxPlayer", player_->status().toJson()},
        {"robloxStudio", studio_->status().toJson()},
        {"versionHistory", versions_->toJson()},
        {"executions", json::Object{{"total", static_cast<uint64_t>(history.size())}, {"succeeded", ok}, {"failed", failed},
                                    {"averageWallMs", history.empty() ? 0.0 : totalMs / static_cast<double>(history.size())}}},
        {"logCounts", std::move(logCounts)},
        {"session", sessions_->current().toJson()},
    };
}

Result<void> NexusHost::exportDiagnostics(const fs::path& path) const {
    json::Array history, logs;
    for (const auto& h : sessions_->history()) history.push_back(h.toJson());
    for (const auto& e : const_cast<Logger&>(logger_).entries()) logs.push_back(e.toJson());
    json::Value doc = json::Object{
        {"format", "nexus-diagnostics-v1"},
        {"exportedAt", str::isoTimestamp(std::chrono::system_clock::now())},
        {"snapshot", diagnosticsSnapshot()},
        {"history", std::move(history)},
        {"logs", std::move(logs)},
    };
    return writeFileAtomic(path, json::dump(doc, 2) + "\n");
}

void NexusHost::saveHistory() const {
    if (!options_.persistent) return;
    json::Array arr;
    for (const auto& h : sessions_->history()) arr.push_back(h.toJson());
    (void)writeFileAtomic(dataDir_ / "history.json", json::dump(json::Object{{"format", "nexus-history-v1"}, {"records", std::move(arr)}}) + "\n");
}

void NexusHost::loadHistory() {
    if (!options_.persistent) return;
    auto data = readFile(dataDir_ / "history.json", 32 << 20);
    if (!data.ok()) return;
    auto parsed = json::parse(data.value());
    if (!parsed.ok()) {
        logger_.warning("host", "Ignoring unreadable history.json: " + parsed.error().message);
        return;
    }
    std::vector<ExecutionRecord> records;
    if (const json::Value* r = parsed.value().get("records"))
        for (const auto& item : r->asArray()) records.push_back(ExecutionRecord::fromJson(item));
    sessions_->setHistory(std::move(records));
}

} // namespace nexus
