#include "nexus/roblox/connection.hpp"

#include "nexus/common/strings.hpp"
#include "nexus/library/script_library.hpp"

#include <algorithm>

namespace nexus::roblox {

const char* toString(ConnectionState s) {
    switch (s) {
    case ConnectionState::Disconnected: return "DISCONNECTED";
    case ConnectionState::Discovering: return "DISCOVERING";
    case ConnectionState::ClientFound: return "CLIENT_FOUND";
    case ConnectionState::CheckingCompatibility: return "CHECKING_COMPATIBILITY";
    case ConnectionState::IntegrationAvailable: return "INTEGRATION_AVAILABLE";
    case ConnectionState::Connected: return "CONNECTED";
    case ConnectionState::Unsupported: return "UNSUPPORTED";
    case ConnectionState::Error: return "ERROR";
    }
    return "DISCONNECTED";
}

json::Value StateTransition::toJson() const {
    return json::Object{{"from", toString(from)}, {"to", toString(to)}, {"time", str::isoTimestamp(time)}, {"reason", reason}};
}

ConnectionStateMachine::ConnectionStateMachine(bool integrationSupported, size_t historyLimit)
    : integrationSupported_(integrationSupported), historyLimit_(historyLimit) {}

bool ConnectionStateMachine::isLegal(ConnectionState from, ConnectionState to, bool integrationSupported) {
    using S = ConnectionState;
    if (!integrationSupported && (to == S::IntegrationAvailable || to == S::Connected)) return false;
    if (to == S::Disconnected) return true; // always possible (client closed / user disconnect)
    switch (from) {
    case S::Disconnected: return to == S::Discovering;
    case S::Discovering: return to == S::ClientFound || to == S::Error;
    case S::ClientFound: return to == S::CheckingCompatibility || to == S::Discovering;
    case S::CheckingCompatibility:
        return to == S::IntegrationAvailable || to == S::Unsupported || to == S::Error || to == S::Discovering;
    case S::IntegrationAvailable:
        return to == S::Connected || to == S::Discovering || to == S::CheckingCompatibility || to == S::Error;
    case S::Connected: return to == S::IntegrationAvailable || to == S::Discovering || to == S::Error;
    case S::Unsupported: return to == S::Discovering;
    case S::Error: return to == S::Discovering;
    }
    return false;
}

bool ConnectionStateMachine::transition(ConnectionState to, std::string reason) {
    if (to == state_) {
        lastReason_ = std::move(reason);
        return true;
    }
    if (!isLegal(state_, to, integrationSupported_)) return false;
    history_.push_back({state_, to, std::chrono::system_clock::now(), reason});
    if (history_.size() > historyLimit_) history_.erase(history_.begin());
    state_ = to;
    lastReason_ = std::move(reason);
    return true;
}

// ---------------------------------------------------------------------------------------------
// VersionHistory

VersionHistory::VersionHistory(std::filesystem::path file) : file_(std::move(file)) { load(); }

void VersionHistory::load() {
    if (file_.empty()) return;
    auto data = readFile(file_, 1 << 20);
    if (!data.ok()) return;
    auto parsed = json::parse(data.value());
    if (parsed.ok() && parsed.value().isObject()) data_ = parsed.value();
}

void VersionHistory::save() const {
    if (file_.empty()) return;
    (void)writeFileAtomic(file_, json::dump(data_, 2) + "\n");
}

std::optional<std::string> VersionHistory::record(Product product, const std::string& version) {
    if (version.empty()) return std::nullopt;
    json::Value& entry = data_[toString(product)];
    std::string previous = entry.getString("lastVersion");
    std::string now = str::isoTimestamp(std::chrono::system_clock::now());
    if (previous == version) {
        entry["lastSeen"] = now;
        save();
        return std::nullopt;
    }
    if (!previous.empty()) entry["previousVersion"] = previous;
    entry["lastVersion"] = version;
    entry["firstSeen"] = now;
    entry["lastSeen"] = now;
    save();
    if (previous.empty()) return std::nullopt;
    return previous;
}

std::optional<std::string> VersionHistory::lastVersion(Product product) const {
    const json::Value* entry = data_.get(toString(product));
    if (!entry) return std::nullopt;
    std::string v = entry->getString("lastVersion");
    if (v.empty()) return std::nullopt;
    return v;
}

json::Value VersionHistory::toJson() const { return data_; }

// ---------------------------------------------------------------------------------------------
// PlayerMonitor

json::Value PlayerStatus::toJson() const {
    json::Array run, reasons, n, hist;
    for (const auto& r : running) run.push_back(r.toJson());
    for (const auto& r : unsupportedReasons) reasons.push_back(r);
    for (const auto& x : notes) n.push_back(x);
    for (const auto& h : history) hist.push_back(h.toJson());
    json::Object o{
        {"state", toString(state)},
        {"summary", summary},
        {"running", std::move(run)},
        {"unsupportedReasons", std::move(reasons)},
        {"notes", std::move(n)},
        {"generation", generation},
        {"history", std::move(hist)},
        {"checkedAt", str::isoTimestamp(checkedAt)},
        {"liveExecutionAvailable", false},
    };
    if (installed) o["installed"] = installed->toJson();
    if (previousVersion) o["previousVersion"] = *previousVersion;
    return o;
}

PlayerMonitor::PlayerMonitor(Logger& logger, ClientEnvironment& env, VersionHistory& versions)
    : logger_(logger), env_(env), versions_(versions) {
    status_.summary = "Not checked yet";
}

std::vector<std::string> PlayerMonitor::unsupportedReasons(const std::optional<InstalledClient>& installed) {
    std::vector<std::string> reasons = {
        "Roblox provides no supported or documented interface for executing third-party code inside the live Roblox Player.",
        "Running code in the Player would require modifying or injecting into the Roblox process, which the Roblox Terms of Use "
        "prohibit and which Nexus deliberately does not implement.",
    };
    if (installed && installed->binary && !installed->binary->slices.empty()) {
        const auto posture = evaluateSecurityPosture(installed->binary->slices.front().signature);
        for (const auto& e : posture.explanations) reasons.push_back("Measured on this installation: " + e);
    }
    reasons.push_back("Supported alternative: run Roblox engine code in Roblox Studio through its official MCP server (Edit, Server or "
                      "Client data model), or run plain Luau in the Nexus standalone runtime.");
    return reasons;
}

PlayerStatus PlayerMonitor::refresh() {
    std::lock_guard lock(mutex_);
    PlayerStatus st;
    st.checkedAt = std::chrono::system_clock::now();
    machine_.transition(ConnectionState::Discovering, "Scanning for the Roblox Player");

    std::vector<std::string> notes;
    st.installed = findInstalled(Product::Player, env_, &notes);
    st.running = findRunning(Product::Player, env_);
    st.notes = notes;

    std::vector<pid_t> pids;
    for (const auto& r : st.running) pids.push_back(r.pid);
    st.generation = status_.generation;
    if (pids != lastPids_) {
        if (!lastPids_.empty() && !pids.empty()) logger_.info("roblox", "Roblox Player restarted (process set changed)");
        else if (!lastPids_.empty()) logger_.info("roblox", "Roblox Player closed");
        else if (!pids.empty()) logger_.info("roblox", "Roblox Player started (pid " + std::to_string(pids.front()) + ")");
        ++st.generation;
        lastPids_ = pids;
    }

    if (!st.installed && st.running.empty()) {
        machine_.transition(ConnectionState::Disconnected, "Roblox Player is not installed");
        st.summary = "Roblox Player is not installed in /Applications or ~/Applications";
        st.unsupportedReasons = unsupportedReasons(std::nullopt); // live execution is unsupported either way
    } else {
        std::string where = st.running.empty() ? "installed, not running" : "running (pid " + std::to_string(st.running.front().pid) + ")";
        machine_.transition(ConnectionState::ClientFound, "Roblox Player " + where);
        machine_.transition(ConnectionState::CheckingCompatibility, "Evaluating integration options");
        if (st.installed) {
            if (auto previous = versions_.record(Product::Player, st.installed->version)) {
                st.previousVersion = previous;
                logger_.info("roblox", "Roblox Player updated from " + *previous + " to " + st.installed->version +
                                           "; re-evaluated compatibility (live execution remains unsupported)");
            }
        }
        st.unsupportedReasons = unsupportedReasons(st.installed);
        machine_.transition(ConnectionState::Unsupported, "No authorized execution interface for the live Roblox Player");
        std::string version = st.installed ? st.installed->version : std::string("unknown version");
        st.summary = "Roblox Player " + version + " " + where + ". Live script execution: not supported.";
    }
    st.state = machine_.state();
    st.history = machine_.history();
    status_ = st;
    return st;
}

PlayerStatus PlayerMonitor::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

// ---------------------------------------------------------------------------------------------
// PlayerTarget

TargetInfo PlayerTarget::info() const {
    TargetInfo i;
    i.id = kId;
    i.displayName = "Roblox Player (live)";
    i.description = "The live Roblox game client. Detected and inspected read-only; Nexus cannot execute scripts in it "
                    "because no authorized interface exists.";
    i.kind = TargetKind::RobloxPlayer;
    return i; // all capabilities false
}

TargetAvailability PlayerTarget::availability() const {
    PlayerStatus st = monitor_.status();
    std::string detected = st.running.empty() ? "" : " (a Roblox Player process is running, but detection is not attachment)";
    return {false, "Live execution in the Roblox Player is not supported: no authorized execution interface exists" + detected};
}

ExecutionResult PlayerTarget::execute(const ExecutionRequest&, const OutputCallback&, const CancellationToken&) {
    return makeUnavailableResult(kId, availability().reason);
}

Result<RemoteVersion> parseClientVersionResponse(const std::string& body) {
    auto v = json::parse(body);
    if (!v.ok()) return Error("roblox.version", "unexpected response: " + v.error().message);
    RemoteVersion r;
    r.version = v.value().getString("version");
    r.clientVersionUpload = v.value().getString("clientVersionUpload");
    if (r.version.empty()) return Error("roblox.version", "response does not contain a version");
    return r;
}

} // namespace nexus::roblox
