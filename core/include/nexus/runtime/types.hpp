#pragma once
// Value types shared by every execution target (in-process, isolated runner, Roblox Studio).

#include "nexus/common/json.hpp"
#include "nexus/diagnostics/diagnostic.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nexus {

struct ExecutionLimits {
    std::chrono::milliseconds timeout{30000};   // wall-clock limit for the whole run (incl. task.wait)
    uint64_t memoryLimitBytes = 256ull << 20;   // Luau heap limit enforced by the allocator
    uint64_t maxOutputBytes = 4ull << 20;       // further output is dropped (and reported)
    uint32_t maxThreads = 10000;                // live coroutines managed by the task scheduler
    double minimumWaitSeconds = 1.0 / 60.0;     // task.wait() resolution (one 60 Hz frame, as in Roblox)
    std::chrono::milliseconds killGracePeriod{750}; // isolated runner: SIGKILL delay after cancel/timeout

    json::Value toJson() const;
    static ExecutionLimits fromJson(const json::Value& v);
};

enum class OutputStream { Output, Warning, Error, Info, System };
const char* toString(OutputStream s);
OutputStream outputStreamFromString(std::string_view s);

struct OutputEvent {
    OutputStream stream = OutputStream::Output;
    std::string text;
    double elapsedMs = 0; // relative to execution start

    json::Value toJson() const;
    static OutputEvent fromJson(const json::Value& v);
};

using OutputCallback = std::function<void(const OutputEvent&)>;

enum class ExecutionStatus {
    Succeeded,
    CompileError,
    RuntimeError,
    Cancelled,
    TimedOut,
    MemoryLimit,
    Unavailable,   // the target cannot execute (not connected / unsupported)
    InternalError, // failure of Nexus or of the target itself (runner crashed, protocol error, ...)
};
const char* toString(ExecutionStatus s);
ExecutionStatus executionStatusFromString(std::string_view s);

struct StackFrame {
    std::string source;
    uint32_t line = 0;
    std::string function;
};

struct ScriptError {
    std::string message;   // message without the "chunk:line:" prefix
    std::string raw;       // message exactly as raised by Luau
    uint32_t line = 0;     // 1-based line when known
    std::string traceback; // Luau debug trace when available
    std::vector<StackFrame> frames;

    json::Value toJson() const;
    static ScriptError fromJson(const json::Value& v);
};

struct ExecutionStats {
    double wallMs = 0;
    double cpuMs = 0;
    double compileMs = 0;
    uint64_t peakMemoryBytes = 0;
    uint64_t bytecodeBytes = 0;
    uint64_t interruptChecks = 0; // VM safepoints reached (proxy for executed loop iterations/calls)
    uint64_t outputBytes = 0;
    uint64_t droppedOutputBytes = 0;
    uint32_t threadsCreated = 0;
    uint32_t backgroundErrors = 0; // errors raised in task.spawn/defer/delay threads

    json::Value toJson() const;
    static ExecutionStats fromJson(const json::Value& v);
};

struct ExecutionResult {
    ExecutionStatus status = ExecutionStatus::InternalError;
    std::optional<ScriptError> error;
    std::vector<Diagnostic> diagnostics;
    ExecutionStats stats;
    std::string target;  // execution target id
    std::string summary; // one-line human readable outcome

    bool succeeded() const { return status == ExecutionStatus::Succeeded; }

    json::Value toJson() const;
    static ExecutionResult fromJson(const json::Value& v);
};

// Splits "Script:12: message" into (12, "message"). Returns line 0 when there is no prefix.
std::pair<uint32_t, std::string> splitErrorLocation(std::string_view raw, std::string_view chunkName);
std::vector<StackFrame> parseTraceback(std::string_view traceback);

} // namespace nexus
