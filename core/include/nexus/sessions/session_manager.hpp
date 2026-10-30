#pragma once
// Centralised execution session management.
//
// State machine (one active session at a time; starting another while one is active is refused):
//
//   IDLE -> INITIALIZING -> COMPILING -> READY -> RUNNING -> STOPPED      (finished / stopped by user)
//                 |             |                    |   \-> FAILED       (compile/runtime/limit error)
//                 |             |                    \-> STOPPING -> STOPPED
//                 \-------------+-> FAILED (target unavailable, syntax errors)
//   RUNNING -> DISCONNECTED when the target's connection is lost or replaced mid-run.
//
// The manager owns the worker thread of the active session and joins it on shutdown, so no
// execution outlives the manager.

#include "nexus/common/cancellation.hpp"
#include "nexus/common/result.hpp"
#include "nexus/common/ring_buffer.hpp"
#include "nexus/diagnostics/logger.hpp"
#include "nexus/execution/target.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace nexus {

enum class SessionState { Idle, Initializing, Compiling, Ready, Running, Stopping, Stopped, Failed, Disconnected };
const char* toString(SessionState s);
bool isActive(SessionState s); // Initializing .. Stopping

struct ConsoleLine {
    uint64_t sessionId = 0;
    OutputStream stream = OutputStream::Output;
    std::string text;
    std::chrono::system_clock::time_point time;
    double elapsedMs = 0;
};

struct ExecutionRecord {
    uint64_t sessionId = 0;
    std::string scriptName;
    std::string targetId;
    std::chrono::system_clock::time_point startedAt;
    uint64_t sourceBytes = 0;
    SessionState finalState = SessionState::Idle;
    ExecutionResult result;

    json::Value toJson() const;
    static ExecutionRecord fromJson(const json::Value& v);
};

struct SessionSnapshot {
    uint64_t id = 0;
    SessionState state = SessionState::Idle;
    std::string scriptName;
    std::string targetId;
    std::chrono::system_clock::time_point startedAt;
    bool cancelRequested = false;
    std::optional<ExecutionResult> result;
    std::string message; // human-readable status

    json::Value toJson() const;
};

enum class SessionEventKind { StateChanged, Output, Finished };

struct SessionEvent {
    SessionEventKind kind = SessionEventKind::StateChanged;
    SessionSnapshot snapshot;
    std::optional<ConsoleLine> line;
    uint64_t consoleSequence = 0;
};

class SessionManager {
public:
    using Listener = std::function<void(const SessionEvent&)>;

    SessionManager(Logger& logger, TargetRegistry& targets, size_t consoleCapacity = 20000, size_t historyCapacity = 200);
    ~SessionManager();
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // Starts executing `request` on `targetId`. Fails with "session.busy" while another session is
    // active.
    Result<uint64_t> start(ExecutionRequest request, const std::string& targetId);
    // Requests cancellation of the active session. Returns false if nothing is running.
    bool stop();
    // Stops the active session (if any), waits for it to finish and starts the last request again.
    Result<uint64_t> restart(std::chrono::milliseconds stopTimeout = std::chrono::seconds(5));

    // Marks sessions running on `targetId` as stale: they are cancelled and finish as DISCONNECTED.
    void targetDisconnected(const std::string& targetId, const std::string& reason);

    SessionSnapshot current() const;
    bool busy() const;
    bool waitForIdle(std::chrono::milliseconds timeout) const;

    std::vector<ExecutionRecord> history() const;
    void setHistory(std::vector<ExecutionRecord> records);
    void clearHistory();

    std::vector<std::pair<uint64_t, ConsoleLine>> console(uint64_t afterSequence = 0, size_t limit = SIZE_MAX) const;
    uint64_t consoleSequence() const;
    void clearConsole();
    void appendConsole(OutputStream stream, std::string text); // host messages (not from a script)

    uint64_t subscribe(Listener listener);
    void unsubscribe(uint64_t token);

    // Cancels the active session and joins its worker (waiting at most `grace`, then detaching
    // from the result; isolated runners are killed by their own deadline logic).
    void shutdown(std::chrono::milliseconds grace = std::chrono::seconds(3));

private:
    struct Active {
        uint64_t id = 0;
        ExecutionRequest request;
        std::string targetId;
        CancellationSource cancel;
        std::chrono::system_clock::time_point startedAt;
        bool disconnected = false;
        std::string disconnectReason;
    };

    void runSession(std::shared_ptr<Active> active);
    SessionSnapshot setState(SessionState state, std::string message, std::optional<ExecutionResult> result = std::nullopt);
    void emit(const SessionEvent& event);
    uint64_t pushConsole(ConsoleLine line);

    Logger& logger_;
    TargetRegistry& targets_;

    mutable std::mutex mutex_;
    mutable std::condition_variable idleCv_;
    SessionSnapshot snapshot_;
    std::shared_ptr<Active> active_;
    std::thread worker_;
    uint64_t nextId_ = 1;
    std::optional<std::pair<ExecutionRequest, std::string>> lastRequest_;
    bool shuttingDown_ = false;

    RingBuffer<ConsoleLine> console_;
    RingBuffer<ExecutionRecord> history_;

    std::mutex listenerMutex_;
    std::map<uint64_t, Listener> listeners_;
    uint64_t nextListener_ = 1;
};

} // namespace nexus
