#pragma once
// Static compatibility analysis of a Luau script, performed on the official Luau AST.
//
// The analyzer answers "where can this script run, and if not, why?" for each Nexus target:
//   * standalone Luau runtime   (luau-isolated / luau-inprocess)
//   * Roblox Studio integration (roblox-studio)
//   * live Roblox Player        (always unavailable: no authorized execution interface exists)
// It is static and therefore conservative: it cannot see code produced at runtime (loadstring) or
// globals injected by other scripts, and it reports such cases as "unknown" rather than guessing.

#include "nexus/compatibility/catalog.hpp"
#include "nexus/diagnostics/diagnostic.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace nexus::compat {

enum class FindingKind {
    Syntax,
    RobloxGlobal,
    RobloxDatatype,
    RobloxService,
    ContextRestriction, // client-only / server-only / studio-only / restricted API
    ExecutorApi,
    Deprecated,
    UnknownGlobal,
    DangerousOperation,
    RuntimeAssumption,
};
const char* toString(FindingKind k);

struct Finding {
    FindingKind kind = FindingKind::UnknownGlobal;
    Severity severity = Severity::Info;
    std::string symbol;
    std::string message;
    SourceRange range;     // first occurrence
    uint32_t occurrences = 1;
    Availability standalone = Availability::Available;
    Availability studio = Availability::Available;
    std::string risk;      // security/privacy note when relevant

    json::Value toJson() const;
};

struct TargetVerdict {
    std::string targetId;
    std::string displayName;
    Availability verdict = Availability::Unknown;
    std::string summary;
    std::vector<std::string> blockers;

    json::Value toJson() const;
};

struct CompatibilityReport {
    bool syntaxOk = true;
    std::vector<Diagnostic> syntax;
    std::vector<Finding> findings;
    std::vector<std::string> globalsUsed;   // free (non-local) names read by the script
    std::vector<std::string> globalsDefined;// globals assigned by the script itself
    std::vector<std::string> services;      // services requested via GetService / game.Service
    std::vector<TargetVerdict> verdicts;
    double analysisMs = 0;

    size_t count(Severity s) const;
    json::Value toJson() const;
    std::string toText() const;
    // Findings converted to editor diagnostics (for inline display).
    std::vector<Diagnostic> toDiagnostics() const;
};

CompatibilityReport analyzeCompatibility(std::string_view source);

} // namespace nexus::compat
