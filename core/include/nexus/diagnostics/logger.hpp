#pragma once
// Structured, thread-safe application log. Entries are kept in a bounded ring buffer (for the
// Diagnostics view) and optionally appended to a JSON-lines file with size-based rotation.
// Nothing is ever sent over the network.

#include "nexus/common/json.hpp"
#include "nexus/common/result.hpp"
#include "nexus/common/ring_buffer.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace nexus {

enum class LogLevel { Trace, Debug, Info, Warning, Error };

const char* toString(LogLevel level);

struct LogEntry {
    uint64_t sequence = 0;
    std::chrono::system_clock::time_point time;
    LogLevel level = LogLevel::Info;
    std::string category; // e.g. "session", "roblox", "studio", "library"
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;

    json::Value toJson() const;
};

class Logger {
public:
    using Listener = std::function<void(const LogEntry&)>;

    explicit Logger(size_t capacity = 5000);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, std::string category, std::string message,
             std::vector<std::pair<std::string, std::string>> fields = {});

    using Fields = std::vector<std::pair<std::string, std::string>>;
    void trace(std::string category, std::string message, Fields fields = {}) { log(LogLevel::Trace, std::move(category), std::move(message), std::move(fields)); }
    void debug(std::string category, std::string message, Fields fields = {}) { log(LogLevel::Debug, std::move(category), std::move(message), std::move(fields)); }
    void info(std::string category, std::string message, Fields fields = {}) { log(LogLevel::Info, std::move(category), std::move(message), std::move(fields)); }
    void warning(std::string category, std::string message, Fields fields = {}) { log(LogLevel::Warning, std::move(category), std::move(message), std::move(fields)); }
    void error(std::string category, std::string message, Fields fields = {}) { log(LogLevel::Error, std::move(category), std::move(message), std::move(fields)); }

    void setMinimumLevel(LogLevel level);

    // Enables persistent logging to `path` (JSON lines). The file is rotated to `path.1` when it
    // exceeds maxBytes.
    Result<void> enableFileSink(const std::filesystem::path& path, uint64_t maxBytes = 4 * 1024 * 1024);
    void disableFileSink();

    std::vector<LogEntry> entries(uint64_t afterSequence = 0, size_t limit = SIZE_MAX) const;
    uint64_t lastSequence() const;
    std::map<LogLevel, uint64_t> countsByLevel() const;

    // Listener tokens allow unsubscribing; listeners are invoked outside the internal lock.
    uint64_t addListener(Listener listener);
    void removeListener(uint64_t token);

    Result<void> exportJson(const std::filesystem::path& path) const;
    Result<void> exportText(const std::filesystem::path& path) const;
    void clear();

private:
    void writeToFile(const LogEntry& entry);

    mutable std::mutex mutex_;
    RingBuffer<LogEntry> buffer_;
    LogLevel minimum_ = LogLevel::Debug;
    std::map<LogLevel, uint64_t> counts_;
    std::map<uint64_t, Listener> listeners_;
    uint64_t nextListener_ = 1;

    std::filesystem::path filePath_;
    std::FILE* file_ = nullptr;
    uint64_t fileBytes_ = 0;
    uint64_t fileMaxBytes_ = 0;
};

} // namespace nexus
