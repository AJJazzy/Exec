#include "nexus/runtime/types.hpp"

#include "nexus/common/strings.hpp"

#include <cctype>

namespace nexus {

json::Value ExecutionLimits::toJson() const {
    return json::Object{
        {"timeoutMs", static_cast<int64_t>(timeout.count())},
        {"memoryLimitBytes", memoryLimitBytes},
        {"maxOutputBytes", maxOutputBytes},
        {"maxThreads", maxThreads},
        {"minimumWaitSeconds", minimumWaitSeconds},
        {"killGracePeriodMs", static_cast<int64_t>(killGracePeriod.count())},
    };
}

ExecutionLimits ExecutionLimits::fromJson(const json::Value& v) {
    ExecutionLimits l;
    l.timeout = std::chrono::milliseconds(v.getInt("timeoutMs", l.timeout.count()));
    l.memoryLimitBytes = static_cast<uint64_t>(v.getInt("memoryLimitBytes", static_cast<int64_t>(l.memoryLimitBytes)));
    l.maxOutputBytes = static_cast<uint64_t>(v.getInt("maxOutputBytes", static_cast<int64_t>(l.maxOutputBytes)));
    l.maxThreads = static_cast<uint32_t>(v.getInt("maxThreads", l.maxThreads));
    l.minimumWaitSeconds = v.getNumber("minimumWaitSeconds", l.minimumWaitSeconds);
    l.killGracePeriod = std::chrono::milliseconds(v.getInt("killGracePeriodMs", l.killGracePeriod.count()));
    return l;
}

const char* toString(OutputStream s) {
    switch (s) {
    case OutputStream::Output: return "output";
    case OutputStream::Warning: return "warning";
    case OutputStream::Error: return "error";
    case OutputStream::Info: return "info";
    case OutputStream::System: return "system";
    }
    return "output";
}

OutputStream outputStreamFromString(std::string_view s) {
    if (s == "warning") return OutputStream::Warning;
    if (s == "error") return OutputStream::Error;
    if (s == "info") return OutputStream::Info;
    if (s == "system") return OutputStream::System;
    return OutputStream::Output;
}

json::Value OutputEvent::toJson() const {
    return json::Object{{"stream", nexus::toString(stream)}, {"text", text}, {"elapsedMs", elapsedMs}};
}

OutputEvent OutputEvent::fromJson(const json::Value& v) {
    OutputEvent e;
    e.stream = outputStreamFromString(v.getString("stream"));
    e.text = v.getString("text");
    e.elapsedMs = v.getNumber("elapsedMs");
    return e;
}

const char* toString(ExecutionStatus s) {
    switch (s) {
    case ExecutionStatus::Succeeded: return "succeeded";
    case ExecutionStatus::CompileError: return "compile-error";
    case ExecutionStatus::RuntimeError: return "runtime-error";
    case ExecutionStatus::Cancelled: return "cancelled";
    case ExecutionStatus::TimedOut: return "timed-out";
    case ExecutionStatus::MemoryLimit: return "memory-limit";
    case ExecutionStatus::Unavailable: return "unavailable";
    case ExecutionStatus::InternalError: return "internal-error";
    }
    return "internal-error";
}

ExecutionStatus executionStatusFromString(std::string_view s) {
    if (s == "succeeded") return ExecutionStatus::Succeeded;
    if (s == "compile-error") return ExecutionStatus::CompileError;
    if (s == "runtime-error") return ExecutionStatus::RuntimeError;
    if (s == "cancelled") return ExecutionStatus::Cancelled;
    if (s == "timed-out") return ExecutionStatus::TimedOut;
    if (s == "memory-limit") return ExecutionStatus::MemoryLimit;
    if (s == "unavailable") return ExecutionStatus::Unavailable;
    return ExecutionStatus::InternalError;
}

json::Value ScriptError::toJson() const {
    json::Array f;
    for (const auto& fr : frames) f.push_back(json::Object{{"source", fr.source}, {"line", fr.line}, {"function", fr.function}});
    return json::Object{{"message", message}, {"raw", raw}, {"line", line}, {"traceback", traceback}, {"frames", std::move(f)}};
}

ScriptError ScriptError::fromJson(const json::Value& v) {
    ScriptError e;
    e.message = v.getString("message");
    e.raw = v.getString("raw");
    e.line = static_cast<uint32_t>(v.getInt("line"));
    e.traceback = v.getString("traceback");
    if (const json::Value* f = v.get("frames")) {
        for (const auto& fr : f->asArray())
            e.frames.push_back({fr.getString("source"), static_cast<uint32_t>(fr.getInt("line")), fr.getString("function")});
    }
    return e;
}

json::Value ExecutionStats::toJson() const {
    return json::Object{
        {"wallMs", wallMs},
        {"cpuMs", cpuMs},
        {"compileMs", compileMs},
        {"peakMemoryBytes", peakMemoryBytes},
        {"bytecodeBytes", bytecodeBytes},
        {"interruptChecks", interruptChecks},
        {"outputBytes", outputBytes},
        {"droppedOutputBytes", droppedOutputBytes},
        {"threadsCreated", threadsCreated},
        {"backgroundErrors", backgroundErrors},
    };
}

ExecutionStats ExecutionStats::fromJson(const json::Value& v) {
    ExecutionStats s;
    s.wallMs = v.getNumber("wallMs");
    s.cpuMs = v.getNumber("cpuMs");
    s.compileMs = v.getNumber("compileMs");
    s.peakMemoryBytes = static_cast<uint64_t>(v.getInt("peakMemoryBytes"));
    s.bytecodeBytes = static_cast<uint64_t>(v.getInt("bytecodeBytes"));
    s.interruptChecks = static_cast<uint64_t>(v.getInt("interruptChecks"));
    s.outputBytes = static_cast<uint64_t>(v.getInt("outputBytes"));
    s.droppedOutputBytes = static_cast<uint64_t>(v.getInt("droppedOutputBytes"));
    s.threadsCreated = static_cast<uint32_t>(v.getInt("threadsCreated"));
    s.backgroundErrors = static_cast<uint32_t>(v.getInt("backgroundErrors"));
    return s;
}

json::Value ExecutionResult::toJson() const {
    json::Array diags;
    for (const auto& d : diagnostics) diags.push_back(d.toJson());
    json::Object o{
        {"status", nexus::toString(status)},
        {"diagnostics", std::move(diags)},
        {"stats", stats.toJson()},
        {"target", target},
        {"summary", summary},
    };
    if (error) o["error"] = error->toJson();
    return o;
}

ExecutionResult ExecutionResult::fromJson(const json::Value& v) {
    ExecutionResult r;
    r.status = executionStatusFromString(v.getString("status"));
    if (const json::Value* e = v.get("error"); e && e->isObject()) r.error = ScriptError::fromJson(*e);
    if (const json::Value* d = v.get("diagnostics"))
        for (const auto& item : d->asArray()) r.diagnostics.push_back(Diagnostic::fromJson(item));
    if (const json::Value* s = v.get("stats")) r.stats = ExecutionStats::fromJson(*s);
    r.target = v.getString("target");
    r.summary = v.getString("summary");
    return r;
}

std::pair<uint32_t, std::string> splitErrorLocation(std::string_view raw, std::string_view chunkName) {
    // Luau formats runtime errors as "<chunk>:<line>: <message>".
    if (!chunkName.empty() && str::startsWith(raw, chunkName) && raw.size() > chunkName.size() && raw[chunkName.size()] == ':') {
        size_t p = chunkName.size() + 1;
        size_t digitsStart = p;
        while (p < raw.size() && std::isdigit(static_cast<unsigned char>(raw[p]))) ++p;
        if (p > digitsStart && p < raw.size() && raw[p] == ':') {
            uint32_t line = static_cast<uint32_t>(std::stoul(std::string(raw.substr(digitsStart, p - digitsStart))));
            std::string_view rest = raw.substr(p + 1);
            if (!rest.empty() && rest[0] == ' ') rest.remove_prefix(1);
            return {line, std::string(rest)};
        }
    }
    return {0, std::string(raw)};
}

std::vector<StackFrame> parseTraceback(std::string_view traceback) {
    // lua_debugtrace lines look like "Script:12 function name" or "Script:12".
    std::vector<StackFrame> frames;
    for (std::string_view line : str::splitLines(traceback)) {
        line = str::trim(line);
        if (line.empty()) continue;
        size_t colon = line.rfind(':', line.find(' '));
        if (colon == std::string_view::npos) continue;
        size_t p = colon + 1;
        size_t ds = p;
        while (p < line.size() && std::isdigit(static_cast<unsigned char>(line[p]))) ++p;
        if (p == ds) continue;
        StackFrame f;
        f.source = std::string(line.substr(0, colon));
        f.line = static_cast<uint32_t>(std::stoul(std::string(line.substr(ds, p - ds))));
        std::string_view rest = str::trim(line.substr(p));
        if (str::startsWith(rest, "function ")) rest.remove_prefix(9);
        f.function = std::string(rest);
        frames.push_back(std::move(f));
    }
    return frames;
}

} // namespace nexus
