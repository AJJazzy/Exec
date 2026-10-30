#include "nexus/sessions/session_manager.hpp"

#include "nexus/common/strings.hpp"
#include "nexus/compiler/compiler.hpp"

namespace nexus {

const char* toString(SessionState s) {
    switch (s) {
    case SessionState::Idle: return "IDLE";
    case SessionState::Initializing: return "INITIALIZING";
    case SessionState::Compiling: return "COMPILING";
    case SessionState::Ready: return "READY";
    case SessionState::Running: return "RUNNING";
    case SessionState::Stopping: return "STOPPING";
    case SessionState::Stopped: return "STOPPED";
    case SessionState::Failed: return "FAILED";
    case SessionState::Disconnected: return "DISCONNECTED";
    }
    return "IDLE";
}

namespace {
SessionState sessionStateFromString(std::string_view s) {
    for (SessionState st : {SessionState::Idle, SessionState::Initializing, SessionState::Compiling, SessionState::Ready,
                            SessionState::Running, SessionState::Stopping, SessionState::Stopped, SessionState::Failed,
                            SessionState::Disconnected})
        if (s == toString(st)) return st;
    return SessionState::Idle;
}
} // namespace

bool isActive(SessionState s) {
    return s == SessionState::Initializing || s == SessionState::Compiling || s == SessionState::Ready ||
           s == SessionState::Running || s == SessionState::Stopping;
}

json::Value ExecutionRecord::toJson() const {
    return json::Object{
        {"sessionId", sessionId},
        {"scriptName", scriptName},
        {"targetId", targetId},
        {"startedAt", str::isoTimestamp(startedAt)},
        {"startedAtMs", str::unixMillis(startedAt)},
        {"sourceBytes", sourceBytes},
        {"finalState", toString(finalState)},
        {"result", result.toJson()},
    };
}

ExecutionRecord ExecutionRecord::fromJson(const json::Value& v) {
    ExecutionRecord r;
    r.sessionId = static_cast<uint64_t>(v.getInt("sessionId"));
    r.scriptName = v.getString("scriptName");
    r.targetId = v.getString("targetId");
    r.startedAt = str::fromUnixMillis(v.getInt("startedAtMs"));
    r.sourceBytes = static_cast<uint64_t>(v.getInt("sourceBytes"));
    r.finalState = sessionStateFromString(v.getString("finalState"));
    if (const json::Value* res = v.get("result")) r.result = ExecutionResult::fromJson(*res);
    return r;
}

json::Value SessionSnapshot::toJson() const {
    json::Object o{
        {"id", id},
        {"state", toString(state)},
        {"scriptName", scriptName},
        {"targetId", targetId},
        {"startedAt", str::isoTimestamp(startedAt)},
        {"cancelRequested", cancelRequested},
        {"message", message},
    };
    if (result) o["result"] = result->toJson();
    return o;
}

SessionManager::SessionManager(Logger& logger, TargetRegistry& targets, size_t consoleCapacity, size_t historyCapacity)
    : logger_(logger), targets_(targets), console_(consoleCapacity), history_(historyCapacity) {
    snapshot_.message = "Idle";
}

SessionManager::~SessionManager() { shutdown(); }

Result<uint64_t> SessionManager::start(ExecutionRequest request, const std::string& targetId) {
    std::shared_ptr<Active> active;
    std::thread previous;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) return Error("session.shutdown", "Nexus is shutting down");
        if (isActive(snapshot_.state))
            return Error("session.busy", "A script is already running (session " + std::to_string(snapshot_.id) +
                                             "). Stop it before starting another one.");
        previous = std::move(worker_); // finished session; joined below, outside the lock

        active = std::make_shared<Active>();
        active->id = nextId_++;
        active->request = request;
        active->targetId = targetId;
        active->startedAt = std::chrono::system_clock::now();
        active_ = active;
        lastRequest_ = std::make_pair(std::move(request), targetId);

        snapshot_ = SessionSnapshot{};
        snapshot_.id = active->id;
        snapshot_.scriptName = active->request.scriptName;
        snapshot_.targetId = targetId;
        snapshot_.startedAt = active->startedAt;
        snapshot_.state = SessionState::Initializing;
        snapshot_.message = "Initializing";
    }
    // The previous worker may still be delivering its final event; it never blocks on us.
    if (previous.joinable()) previous.join();
    emit(SessionEvent{SessionEventKind::StateChanged, current(), std::nullopt, 0});
    logger_.log(LogLevel::Info, "session", "Session " + std::to_string(active->id) + " started", {{"target", targetId}, {"script", active->request.scriptName}});

    std::lock_guard lock(mutex_);
    worker_ = std::thread([this, active] { runSession(active); });
    return active->id;
}

void SessionManager::runSession(std::shared_ptr<Active> active) {
    auto finish = [&](SessionState state, ExecutionResult result) {
        ExecutionRecord record;
        record.sessionId = active->id;
        record.scriptName = active->request.scriptName;
        record.targetId = active->targetId;
        record.startedAt = active->startedAt;
        record.sourceBytes = active->request.source.size();
        record.finalState = state;
        record.result = result;
        {
            std::lock_guard lock(mutex_);
            history_.push(record);
        }
        std::string summary = result.summary.empty() ? std::string(toString(result.status)) : result.summary;
        pushConsole(ConsoleLine{active->id, result.succeeded() ? OutputStream::System : OutputStream::Error, summary,
                                std::chrono::system_clock::now(), result.stats.wallMs});
        SessionSnapshot finalSnapshot = setState(state, summary, result);
        logger_.log(result.succeeded() ? LogLevel::Info : LogLevel::Warning, "session",
                    "Session " + std::to_string(active->id) + " finished: " + summary,
                    {{"status", toString(result.status)}, {"state", toString(state)}, {"wallMs", std::to_string(result.stats.wallMs)},
                     {"peakMemory", std::to_string(result.stats.peakMemoryBytes)}});
        emit(SessionEvent{SessionEventKind::Finished, finalSnapshot, std::nullopt, 0});
        idleCv_.notify_all();
    };

    std::shared_ptr<ExecutionTarget> target = targets_.find(active->targetId);
    if (!target) {
        finish(SessionState::Failed, makeUnavailableResult(active->targetId, "unknown execution target '" + active->targetId + "'"));
        return;
    }
    TargetAvailability avail = target->availability();
    if (!avail.available) {
        finish(SessionState::Failed, makeUnavailableResult(active->targetId, avail.reason));
        return;
    }
    if (active->cancel.requested()) {
        ExecutionResult r = makeUnavailableResult(active->targetId, "stopped before execution started");
        r.status = ExecutionStatus::Cancelled;
        r.summary = "Stopped before execution started";
        finish(SessionState::Stopped, r);
        return;
    }

    // Every target gets a local syntax check first, so syntax errors are reported identically and
    // never sent anywhere.
    setState(SessionState::Compiling, "Compiling");
    CompileOutput compiled = compileSource(active->request.source);
    if (!compiled.ok) {
        ExecutionResult r;
        r.status = ExecutionStatus::CompileError;
        r.target = active->targetId;
        r.diagnostics = compiled.diagnostics;
        r.stats.compileMs = compiled.parseMilliseconds + compiled.compileMilliseconds;
        if (!compiled.diagnostics.empty()) {
            const Diagnostic& d = compiled.diagnostics.front();
            r.error = ScriptError{d.message, formatDiagnostic(d, active->request.scriptName), d.range.line, {}, {}};
            for (const auto& diag : compiled.diagnostics)
                pushConsole(ConsoleLine{active->id, OutputStream::Error, formatDiagnostic(diag, active->request.scriptName),
                                        std::chrono::system_clock::now(), 0});
        }
        r.summary = "Compilation failed with " + std::to_string(compiled.diagnostics.size()) + " error(s)";
        finish(SessionState::Failed, r);
        return;
    }

    setState(SessionState::Ready, "Ready");
    uint64_t generation = target->generation();
    setState(SessionState::Running, "Running on " + target->info().displayName);

    OutputCallback output = [this, id = active->id](const OutputEvent& e) {
        pushConsole(ConsoleLine{id, e.stream, e.text, std::chrono::system_clock::now(), e.elapsedMs});
    };

    ExecutionResult result;
    try {
        result = target->execute(active->request, output, active->cancel.token());
    } catch (const std::exception& e) {
        result.status = ExecutionStatus::InternalError;
        result.target = active->targetId;
        result.summary = std::string("Execution target failed: ") + e.what();
    }

    bool disconnected;
    std::string reason;
    {
        std::lock_guard lock(mutex_);
        disconnected = active->disconnected || target->generation() != generation;
        reason = active->disconnectReason;
    }
    SessionState state;
    if (disconnected) {
        state = SessionState::Disconnected;
        if (result.succeeded() || result.status == ExecutionStatus::Cancelled) {
            result.summary = "Connection to " + target->info().displayName + " was lost during execution" + (reason.empty() ? "" : ": " + reason);
        }
    } else if (result.succeeded() || result.status == ExecutionStatus::Cancelled) {
        state = SessionState::Stopped;
    } else {
        state = SessionState::Failed;
    }
    finish(state, std::move(result));
}

bool SessionManager::stop() {
    std::shared_ptr<Active> active;
    {
        std::lock_guard lock(mutex_);
        if (!isActive(snapshot_.state) || !active_) return false;
        active = active_;
        if (snapshot_.state != SessionState::Stopping) {
            snapshot_.state = SessionState::Stopping;
            snapshot_.cancelRequested = true;
            snapshot_.message = "Stopping";
        }
    }
    logger_.info("session", "Stop requested for session " + std::to_string(active->id));
    emit(SessionEvent{SessionEventKind::StateChanged, current(), std::nullopt, 0});
    active->cancel.cancel();
    return true;
}

Result<uint64_t> SessionManager::restart(std::chrono::milliseconds stopTimeout) {
    std::optional<std::pair<ExecutionRequest, std::string>> last;
    {
        std::lock_guard lock(mutex_);
        last = lastRequest_;
    }
    if (!last) return Error("session.no_previous", "Nothing to restart yet");
    stop();
    if (!waitForIdle(stopTimeout)) return Error("session.stop_timeout", "The running session did not stop in time");
    return start(last->first, last->second);
}

void SessionManager::targetDisconnected(const std::string& targetId, const std::string& reason) {
    std::shared_ptr<Active> active;
    {
        std::lock_guard lock(mutex_);
        if (!active_ || active_->targetId != targetId || !isActive(snapshot_.state)) return;
        active_->disconnected = true;
        active_->disconnectReason = reason;
        active = active_;
    }
    logger_.warning("session", "Target " + targetId + " disconnected during session " + std::to_string(active->id) + ": " + reason);
    active->cancel.cancel();
}

SessionSnapshot SessionManager::setState(SessionState state, std::string message, std::optional<ExecutionResult> result) {
    SessionSnapshot snapshot;
    {
        std::lock_guard lock(mutex_);
        // A stop request keeps the visible state at STOPPING until the run actually ends.
        if (snapshot_.state == SessionState::Stopping && isActive(state)) return snapshot_;
        snapshot_.state = state;
        snapshot_.message = std::move(message);
        if (result) snapshot_.result = std::move(result);
        snapshot = snapshot_;
    }
    emit(SessionEvent{SessionEventKind::StateChanged, snapshot, std::nullopt, 0});
    return snapshot;
}

SessionSnapshot SessionManager::current() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

bool SessionManager::busy() const {
    std::lock_guard lock(mutex_);
    return isActive(snapshot_.state);
}

bool SessionManager::waitForIdle(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return idleCv_.wait_for(lock, timeout, [&] { return !isActive(snapshot_.state); });
}

std::vector<ExecutionRecord> SessionManager::history() const {
    std::lock_guard lock(mutex_);
    return history_.values();
}

void SessionManager::setHistory(std::vector<ExecutionRecord> records) {
    std::lock_guard lock(mutex_);
    history_.clear();
    for (auto& r : records) history_.push(std::move(r));
    for (const auto& r : history_.values()) nextId_ = std::max(nextId_, r.sessionId + 1);
}

void SessionManager::clearHistory() {
    std::lock_guard lock(mutex_);
    history_.clear();
}

std::vector<std::pair<uint64_t, ConsoleLine>> SessionManager::console(uint64_t afterSequence, size_t limit) const {
    std::lock_guard lock(mutex_);
    return console_.since(afterSequence, limit);
}

uint64_t SessionManager::consoleSequence() const {
    std::lock_guard lock(mutex_);
    return console_.lastSequence();
}

void SessionManager::clearConsole() {
    std::lock_guard lock(mutex_);
    console_.clear();
}

void SessionManager::appendConsole(OutputStream stream, std::string text) {
    pushConsole(ConsoleLine{0, stream, std::move(text), std::chrono::system_clock::now(), 0});
}

uint64_t SessionManager::pushConsole(ConsoleLine line) {
    uint64_t seq;
    {
        std::lock_guard lock(mutex_);
        seq = console_.push(line);
    }
    SessionEvent e;
    e.kind = SessionEventKind::Output;
    e.line = std::move(line);
    e.consoleSequence = seq;
    {
        std::lock_guard lock(mutex_);
        e.snapshot = snapshot_;
    }
    emit(e);
    return seq;
}

uint64_t SessionManager::subscribe(Listener listener) {
    std::lock_guard lock(listenerMutex_);
    uint64_t token = nextListener_++;
    listeners_[token] = std::move(listener);
    return token;
}

void SessionManager::unsubscribe(uint64_t token) {
    std::lock_guard lock(listenerMutex_);
    listeners_.erase(token);
}

void SessionManager::emit(const SessionEvent& event) {
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(listenerMutex_);
        for (const auto& [_, l] : listeners_) listeners.push_back(l);
    }
    for (const auto& l : listeners) l(event);
}

void SessionManager::shutdown(std::chrono::milliseconds grace) {
    {
        std::lock_guard lock(mutex_);
        shuttingDown_ = true;
    }
    stop();
    waitForIdle(grace);
    std::thread worker;
    {
        std::lock_guard lock(mutex_);
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
    std::lock_guard lock(listenerMutex_);
    listeners_.clear();
}

} // namespace nexus
