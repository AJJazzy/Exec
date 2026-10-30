#pragma once
// Connection state machines for Roblox clients.
//
// States: DISCONNECTED, DISCOVERING, CLIENT_FOUND, CHECKING_COMPATIBILITY, INTEGRATION_AVAILABLE,
// CONNECTED, UNSUPPORTED, ERROR. Transitions are validated; a machine created without integration
// support (the Roblox Player) rejects INTEGRATION_AVAILABLE and CONNECTED outright, so a detected
// process can never be presented as an attached execution environment.

#include "nexus/common/json.hpp"
#include "nexus/diagnostics/logger.hpp"
#include "nexus/execution/target.hpp"
#include "nexus/roblox/client.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nexus::roblox {

enum class ConnectionState { Disconnected, Discovering, ClientFound, CheckingCompatibility, IntegrationAvailable, Connected, Unsupported, Error };
const char* toString(ConnectionState s);

struct StateTransition {
    ConnectionState from = ConnectionState::Disconnected;
    ConnectionState to = ConnectionState::Disconnected;
    std::chrono::system_clock::time_point time;
    std::string reason;

    json::Value toJson() const;
};

class ConnectionStateMachine {
public:
    explicit ConnectionStateMachine(bool integrationSupported, size_t historyLimit = 64);

    ConnectionState state() const { return state_; }
    // Applies the transition if it is legal; returns false (and leaves the state unchanged) otherwise.
    bool transition(ConnectionState to, std::string reason);
    static bool isLegal(ConnectionState from, ConnectionState to, bool integrationSupported);
    const std::vector<StateTransition>& history() const { return history_; }
    const std::string& lastReason() const { return lastReason_; }

private:
    bool integrationSupported_;
    size_t historyLimit_;
    ConnectionState state_ = ConnectionState::Disconnected;
    std::vector<StateTransition> history_;
    std::string lastReason_;
};

// Remembers which client versions were seen, so updates can be reported.
class VersionHistory {
public:
    explicit VersionHistory(std::filesystem::path file = {});
    // Records `version` for `product`; returns the previously recorded version if it changed.
    std::optional<std::string> record(Product product, const std::string& version);
    std::optional<std::string> lastVersion(Product product) const;
    json::Value toJson() const;

private:
    void load();
    void save() const;
    std::filesystem::path file_;
    json::Value data_ = json::Object{};
};

struct PlayerStatus {
    ConnectionState state = ConnectionState::Disconnected;
    std::optional<InstalledClient> installed;
    std::vector<RunningClient> running;
    std::string summary;
    std::vector<std::string> unsupportedReasons;
    std::vector<std::string> notes;
    std::optional<std::string> previousVersion; // set when the installed version changed
    uint64_t generation = 0;                    // changes when the set of running processes changes
    std::vector<StateTransition> history;
    std::chrono::system_clock::time_point checkedAt;

    json::Value toJson() const;
};

// Detects the Roblox Player and evaluates whether any authorized integration is available.
// The answer for live execution is always "unsupported" (see docs/feasibility.md); the monitor
// exists to report detection, version and architecture accurately, never to attach.
class PlayerMonitor {
public:
    PlayerMonitor(Logger& logger, ClientEnvironment& env, VersionHistory& versions);

    PlayerStatus refresh();
    PlayerStatus status() const;

    // The reasons live execution is unavailable, with the facts measured on this machine.
    static std::vector<std::string> unsupportedReasons(const std::optional<InstalledClient>& installed);

private:
    Logger& logger_;
    ClientEnvironment& env_;
    VersionHistory& versions_;
    mutable std::mutex mutex_;
    ConnectionStateMachine machine_{false};
    PlayerStatus status_;
    std::vector<pid_t> lastPids_;
};

// Execution target for the live Roblox Player. It is registered so the UI can show it, but it can
// never execute: availability() is always false and execute() always returns Unavailable.
class PlayerTarget final : public ExecutionTarget {
public:
    static constexpr const char* kId = "roblox-player";
    explicit PlayerTarget(const PlayerMonitor& monitor) : monitor_(monitor) {}

    TargetInfo info() const override;
    TargetAvailability availability() const override;
    ExecutionResult execute(const ExecutionRequest& request, const OutputCallback& output, const CancellationToken& cancel) override;

private:
    const PlayerMonitor& monitor_;
};

// Parses the JSON returned by Roblox's public client-version endpoint
// (https://clientsettingscdn.roblox.com/v2/client-version/MacPlayer).
struct RemoteVersion {
    std::string version;
    std::string clientVersionUpload;
};
Result<RemoteVersion> parseClientVersionResponse(const std::string& body);

} // namespace nexus::roblox
