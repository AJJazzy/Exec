#pragma once

#include "nexus/common/json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nexus {

enum class Severity { Error, Warning, Info, Hint };

const char* toString(Severity s);
Severity severityFromString(std::string_view s);

// 1-based lines; columns are 1-based byte offsets within the line (UTF-8). A zero line means
// "no source location".
struct SourceRange {
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t endLine = 0;
    uint32_t endColumn = 0;

    bool valid() const { return line > 0; }
};

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string code;    // e.g. "syntax", "runtime", "compat.unknown-global"
    std::string message;
    SourceRange range;
    std::string origin;  // subsystem that produced it: "parser", "compiler", "runtime", "analyzer"

    json::Value toJson() const;
    static Diagnostic fromJson(const json::Value& v);
};

std::string formatDiagnostic(const Diagnostic& d, std::string_view chunkName = {});

json::Value toJson(const SourceRange& r);
SourceRange sourceRangeFromJson(const json::Value& v);

} // namespace nexus
