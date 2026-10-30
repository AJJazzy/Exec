#pragma once
// Composition root: owns every Nexus service and wires them together. Shared by nexus-cli and the
// macOS application (through the Objective-C++ bridge), so both front ends behave identically.

#include "nexus/compatibility/analyzer.hpp"
#include "nexus/diagnostics/logger.hpp"
#include "nexus/execution/local_targets.hpp"
#include "nexus/library/draft_store.hpp"
#include "nexus/library/script_library.hpp"
#include "nexus/roblox/connection.hpp"
#include "nexus/sessions/session_manager.hpp"
#include "nexus/settings/settings.hpp"
#include "nexus/studio/studio_integration.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace nexus {

struct HostOptions {
    std::filesystem::path dataDirectory;   // empty = platform application-support directory
    std::string runnerPath;                // empty = locate nexus-runner next to the executable
    bool persistent = true;                // write logs/history/settings to disk
    std::shared_ptr<roblox::ClientEnvironment> environment; // null = the real system
};

class NexusHost {
public:
    explicit NexusHost(HostOptions options = {});
    ~NexusHost();
    NexusHost(const NexusHost&) = delete;
    NexusHost& operator=(const NexusHost&) = delete;

    Logger& logger() { return logger_; }
    SessionManager& sessions() { return *sessions_; }
    ScriptLibrary& library() { return *library_; }
    DraftStore& drafts() { return *drafts_; }
    TargetRegistry& targets() { return targets_; }
    roblox::PlayerMonitor& player() { return *player_; }
    studio::StudioIntegration& studio() { return *studio_; }
    roblox::VersionHistory& versions() { return *versions_; }

    Settings settings() const;
    Result<void> updateSettings(const Settings& settings);

    // Runs `source` on `targetId` (empty = default target from settings) with the configured limits.
    Result<uint64_t> run(const std::string& source, const std::string& scriptName, const std::string& targetId = {},
                         json::Object options = {});

    compat::CompatibilityReport analyze(const std::string& source) const;

    roblox::PlayerStatus refreshRoblox();
    studio::StudioStatus refreshStudio();
    studio::StudioStatus connectStudio();
    void disconnectStudio();

    // Environment, versions, targets and statistics — used by the Diagnostics view and exports.
    json::Value diagnosticsSnapshot() const;
    // Writes logs, execution history and the diagnostics snapshot into one JSON file.
    Result<void> exportDiagnostics(const std::filesystem::path& path) const;

    void saveHistory() const;
    const std::filesystem::path& dataDirectory() const { return dataDir_; }
    std::string runnerPath() const { return runnerPath_; }

private:
    void applySettings(const Settings& s);
    void loadHistory();

    HostOptions options_;
    std::filesystem::path dataDir_;
    std::string runnerPath_;
    Logger logger_;
    TargetRegistry targets_;
    std::shared_ptr<roblox::ClientEnvironment> env_;
    std::unique_ptr<roblox::VersionHistory> versions_;
    std::unique_ptr<SettingsStore> settingsStore_;
    mutable std::mutex settingsMutex_;
    Settings settings_;
    std::unique_ptr<ScriptLibrary> library_;
    std::unique_ptr<DraftStore> drafts_;
    std::unique_ptr<roblox::PlayerMonitor> player_;
    std::shared_ptr<studio::StudioIntegration> studio_;
    std::unique_ptr<SessionManager> sessions_;
    std::chrono::system_clock::time_point startedAt_;
};

} // namespace nexus
