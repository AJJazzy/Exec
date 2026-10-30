#include "nexus/diagnostics/logger.hpp"

#include "nexus/common/strings.hpp"

#include <fstream>

namespace nexus {

const char* toString(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    }
    return "info";
}

json::Value LogEntry::toJson() const {
    json::Object f;
    for (const auto& [k, v] : fields) f[k] = v;
    return json::Object{
        {"seq", sequence},
        {"time", str::isoTimestamp(time)},
        {"level", nexus::toString(level)},
        {"category", category},
        {"message", message},
        {"fields", std::move(f)},
    };
}

Logger::Logger(size_t capacity) : buffer_(capacity) {}

Logger::~Logger() { disableFileSink(); }

void Logger::log(LogLevel level, std::string category, std::string message,
                 std::vector<std::pair<std::string, std::string>> fields) {
    std::vector<Listener> listeners;
    LogEntry entry;
    {
        std::lock_guard lock(mutex_);
        if (level < minimum_) return;
        entry.time = std::chrono::system_clock::now();
        entry.level = level;
        entry.category = std::move(category);
        entry.message = str::sanitizeUtf8(message);
        entry.fields = std::move(fields);
        entry.sequence = buffer_.lastSequence() + 1;
        buffer_.push(entry);
        ++counts_[level];
        if (file_) writeToFile(entry);
        listeners.reserve(listeners_.size());
        for (const auto& [_, l] : listeners_) listeners.push_back(l);
    }
    for (const auto& l : listeners) l(entry);
}

void Logger::setMinimumLevel(LogLevel level) {
    std::lock_guard lock(mutex_);
    minimum_ = level;
}

Result<void> Logger::enableFileSink(const std::filesystem::path& path, uint64_t maxBytes) {
    std::lock_guard lock(mutex_);
    if (file_) std::fclose(file_);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    file_ = std::fopen(path.c_str(), "ab");
    if (!file_) return Error("log.open", "cannot open log file " + path.string());
    filePath_ = path;
    fileMaxBytes_ = maxBytes;
    fileBytes_ = std::filesystem::file_size(path, ec);
    if (ec) fileBytes_ = 0;
    return success();
}

void Logger::disableFileSink() {
    std::lock_guard lock(mutex_);
    if (file_) std::fclose(file_);
    file_ = nullptr;
}

void Logger::writeToFile(const LogEntry& entry) {
    std::string line = json::dump(entry.toJson()) + "\n";
    if (fileMaxBytes_ && fileBytes_ + line.size() > fileMaxBytes_) {
        std::fclose(file_);
        std::error_code ec;
        std::filesystem::path rotated = filePath_;
        rotated += ".1";
        std::filesystem::rename(filePath_, rotated, ec);
        file_ = std::fopen(filePath_.c_str(), "wb");
        fileBytes_ = 0;
        if (!file_) return;
    }
    std::fwrite(line.data(), 1, line.size(), file_);
    std::fflush(file_);
    fileBytes_ += line.size();
}

std::vector<LogEntry> Logger::entries(uint64_t afterSequence, size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<LogEntry> out;
    for (auto& [seq, e] : buffer_.since(afterSequence, limit)) out.push_back(std::move(e));
    return out;
}

uint64_t Logger::lastSequence() const {
    std::lock_guard lock(mutex_);
    return buffer_.lastSequence();
}

std::map<LogLevel, uint64_t> Logger::countsByLevel() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

uint64_t Logger::addListener(Listener listener) {
    std::lock_guard lock(mutex_);
    uint64_t token = nextListener_++;
    listeners_[token] = std::move(listener);
    return token;
}

void Logger::removeListener(uint64_t token) {
    std::lock_guard lock(mutex_);
    listeners_.erase(token);
}

Result<void> Logger::exportJson(const std::filesystem::path& path) const {
    json::Array arr;
    for (const auto& e : entries()) arr.push_back(e.toJson());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Error("log.export", "cannot write " + path.string());
    out << json::dump(json::Object{{"format", "nexus-log-v1"}, {"entries", std::move(arr)}}, 2) << "\n";
    return out.good() ? success() : Result<void>(Error("log.export", "write failed for " + path.string()));
}

Result<void> Logger::exportText(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Error("log.export", "cannot write " + path.string());
    for (const auto& e : entries()) {
        out << str::isoTimestamp(e.time) << " [" << toString(e.level) << "] " << e.category << ": " << e.message;
        for (const auto& [k, v] : e.fields) out << " " << k << "=" << v;
        out << "\n";
    }
    return out.good() ? success() : Result<void>(Error("log.export", "write failed for " + path.string()));
}

void Logger::clear() {
    std::lock_guard lock(mutex_);
    buffer_.clear();
    counts_.clear();
}

} // namespace nexus
