#include "nexus/common/strings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <stdexcept>

#include <sys/random.h>
#include <unistd.h>

namespace nexus::str {

std::string_view trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

std::vector<std::string_view> splitLines(std::string_view s) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            size_t end = i;
            if (end > start && s[end - 1] == '\r') --end;
            lines.push_back(s.substr(start, end - start));
            start = i + 1;
        }
    }
    if (start < s.size()) lines.push_back(s.substr(start));
    return lines;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(s.substr(start));
            break;
        }
        parts.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

namespace {
// Returns the length of the valid UTF-8 sequence starting at s[i], or 0 when invalid.
size_t utf8SequenceLength(std::string_view s, size_t i) {
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = byte(i);
    if (c < 0x80) return 1;
    size_t len = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return 0;
    if (i + len > s.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
        unsigned char cc = byte(i + k);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp > 0x10FFFF) return 0;
    return len;
}
} // namespace

bool isValidUtf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        size_t len = utf8SequenceLength(s, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

std::string sanitizeUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t len = utf8SequenceLength(s, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    return out;
}

std::string formatBytes(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string formatDuration(double ms) {
    char buf[32];
    if (ms < 1.0) std::snprintf(buf, sizeof(buf), "%.0f µs", ms * 1000.0);
    else if (ms < 1000.0) std::snprintf(buf, sizeof(buf), "%.1f ms", ms);
    else if (ms < 60000.0) std::snprintf(buf, sizeof(buf), "%.2f s", ms / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%dm %02ds", static_cast<int>(ms / 60000.0), static_cast<int>(ms / 1000.0) % 60);
    return buf;
}

int64_t unixMillis(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point fromUnixMillis(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(ms)));
}

std::string isoTimestamp(std::chrono::system_clock::time_point tp) {
    int64_t ms = unixMillis(tp);
    std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

std::string randomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    size_t done = 0;
    while (done < bytes) {
        size_t chunk = std::min<size_t>(256, bytes - done); // getentropy limit
        if (getentropy(raw.data() + done, chunk) != 0) throw std::runtime_error("getentropy failed");
        done += chunk;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (unsigned char c : raw) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

} // namespace nexus::str
