#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::str {

std::string_view trim(std::string_view s);
std::string toLower(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool containsIgnoreCase(std::string_view haystack, std::string_view needle);
std::vector<std::string_view> splitLines(std::string_view s); // handles \n and \r\n
std::vector<std::string> split(std::string_view s, char sep);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);

// Returns true when the bytes form valid UTF-8.
bool isValidUtf8(std::string_view s);
// Replaces invalid UTF-8 sequences with U+FFFD so the result is safe to show in the UI / JSON.
std::string sanitizeUtf8(std::string_view s);

std::string formatBytes(uint64_t bytes);
std::string formatDuration(double milliseconds);

// ISO-8601 UTC timestamp, e.g. 2026-10-30T10:54:00.123Z
std::string isoTimestamp(std::chrono::system_clock::time_point tp);
int64_t unixMillis(std::chrono::system_clock::time_point tp);
std::chrono::system_clock::time_point fromUnixMillis(int64_t ms);

// Cryptographically random hex token (uses the OS CSPRNG).
std::string randomHex(size_t bytes);

} // namespace nexus::str
