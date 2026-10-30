#include "nexus/diagnostics/diagnostic.hpp"

namespace nexus {

const char* toString(Severity s) {
    switch (s) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Info: return "info";
    case Severity::Hint: return "hint";
    }
    return "error";
}

Severity severityFromString(std::string_view s) {
    if (s == "warning") return Severity::Warning;
    if (s == "info") return Severity::Info;
    if (s == "hint") return Severity::Hint;
    return Severity::Error;
}

json::Value toJson(const SourceRange& r) {
    return json::Object{{"line", r.line}, {"column", r.column}, {"endLine", r.endLine}, {"endColumn", r.endColumn}};
}

SourceRange sourceRangeFromJson(const json::Value& v) {
    SourceRange r;
    r.line = static_cast<uint32_t>(v.getInt("line"));
    r.column = static_cast<uint32_t>(v.getInt("column"));
    r.endLine = static_cast<uint32_t>(v.getInt("endLine"));
    r.endColumn = static_cast<uint32_t>(v.getInt("endColumn"));
    return r;
}

json::Value Diagnostic::toJson() const {
    return json::Object{
        {"severity", nexus::toString(severity)},
        {"code", code},
        {"message", message},
        {"range", nexus::toJson(range)},
        {"origin", origin},
    };
}

Diagnostic Diagnostic::fromJson(const json::Value& v) {
    Diagnostic d;
    d.severity = severityFromString(v.getString("severity"));
    d.code = v.getString("code");
    d.message = v.getString("message");
    if (const json::Value* r = v.get("range")) d.range = sourceRangeFromJson(*r);
    d.origin = v.getString("origin");
    return d;
}

std::string formatDiagnostic(const Diagnostic& d, std::string_view chunkName) {
    std::string out;
    if (!chunkName.empty()) out += chunkName;
    if (d.range.valid()) {
        out += (out.empty() ? "" : ":") + std::to_string(d.range.line);
        if (d.range.column) out += ":" + std::to_string(d.range.column);
    }
    if (!out.empty()) out += ": ";
    out += toString(d.severity);
    out += ": ";
    out += d.message;
    return out;
}

} // namespace nexus
