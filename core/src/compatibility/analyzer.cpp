#include "nexus/compatibility/analyzer.hpp"

#include "nexus/runtime/luau_runtime.hpp"

#include "Luau/Ast.h"
#include "Luau/Parser.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

namespace nexus::compat {

const char* toString(FindingKind k) {
    switch (k) {
    case FindingKind::Syntax: return "syntax";
    case FindingKind::RobloxGlobal: return "roblox-global";
    case FindingKind::RobloxDatatype: return "roblox-datatype";
    case FindingKind::RobloxService: return "roblox-service";
    case FindingKind::ContextRestriction: return "context-restriction";
    case FindingKind::ExecutorApi: return "executor-api";
    case FindingKind::Deprecated: return "deprecated";
    case FindingKind::UnknownGlobal: return "unknown-global";
    case FindingKind::DangerousOperation: return "dangerous-operation";
    case FindingKind::RuntimeAssumption: return "runtime-assumption";
    }
    return "unknown";
}

json::Value Finding::toJson() const {
    return json::Object{
        {"kind", toString(kind)},
        {"severity", nexus::toString(severity)},
        {"symbol", symbol},
        {"message", message},
        {"range", nexus::toJson(range)},
        {"occurrences", occurrences},
        {"standalone", toString(standalone)},
        {"studio", toString(studio)},
        {"risk", risk},
    };
}

json::Value TargetVerdict::toJson() const {
    json::Array b;
    for (const auto& x : blockers) b.push_back(x);
    return json::Object{{"targetId", targetId}, {"displayName", displayName}, {"verdict", toString(verdict)}, {"summary", summary}, {"blockers", std::move(b)}};
}

size_t CompatibilityReport::count(Severity s) const {
    size_t n = s == Severity::Error ? syntax.size() : 0;
    for (const auto& f : findings) n += f.severity == s ? 1 : 0;
    return n;
}

json::Value CompatibilityReport::toJson() const {
    auto strings = [](const std::vector<std::string>& v) {
        json::Array a;
        for (const auto& s : v) a.push_back(s);
        return a;
    };
    json::Array syn, fin, ver;
    for (const auto& d : syntax) syn.push_back(d.toJson());
    for (const auto& f : findings) fin.push_back(f.toJson());
    for (const auto& v : verdicts) ver.push_back(v.toJson());
    return json::Object{
        {"format", "nexus-compatibility-v1"},
        {"syntaxOk", syntaxOk},
        {"syntax", std::move(syn)},
        {"findings", std::move(fin)},
        {"globalsUsed", strings(globalsUsed)},
        {"globalsDefined", strings(globalsDefined)},
        {"services", strings(services)},
        {"verdicts", std::move(ver)},
        {"analysisMs", analysisMs},
    };
}

std::string CompatibilityReport::toText() const {
    std::string out = "Compatibility report\n====================\n";
    for (const auto& v : verdicts) {
        out += "\n[" + std::string(toString(v.verdict)) + "] " + v.displayName + ": " + v.summary + "\n";
        for (const auto& b : v.blockers) out += "    - " + b + "\n";
    }
    if (!syntax.empty()) {
        out += "\nSyntax errors:\n";
        for (const auto& d : syntax) out += "  " + formatDiagnostic(d) + "\n";
    }
    if (!findings.empty()) {
        out += "\nFindings:\n";
        for (const auto& f : findings) {
            out += "  line " + std::to_string(f.range.line) + " [" + nexus::toString(f.severity) + "] " + f.message;
            if (f.occurrences > 1) out += " (" + std::to_string(f.occurrences) + " occurrences)";
            if (!f.risk.empty()) out += " — risk: " + f.risk;
            out += "\n";
        }
    }
    if (!services.empty()) {
        out += "\nServices used:";
        for (const auto& s : services) out += " " + s;
        out += "\n";
    }
    return out;
}

std::vector<Diagnostic> CompatibilityReport::toDiagnostics() const {
    std::vector<Diagnostic> out = syntax;
    for (const auto& f : findings) {
        Diagnostic d;
        d.severity = f.severity;
        d.code = std::string("compat.") + toString(f.kind);
        d.message = f.message;
        d.range = f.range;
        d.origin = "analyzer";
        out.push_back(std::move(d));
    }
    return out;
}

namespace {

SourceRange toRange(const Luau::Location& loc) {
    return SourceRange{loc.begin.line + 1, loc.begin.column + 1, loc.end.line + 1, loc.end.column + 1};
}

std::string_view stringConstant(Luau::AstExpr* e) {
    if (auto* s = e ? e->as<Luau::AstExprConstantString>() : nullptr) return std::string_view(s->value.data, s->value.size);
    return {};
}

bool isGlobalNamed(Luau::AstExpr* e, std::string_view name) {
    auto* g = e ? e->as<Luau::AstExprGlobal>() : nullptr;
    return g && g->name.value && name == g->name.value;
}

const char* kYieldNames[] = {"wait", "Wait", "yield", "delay", "spawn", "defer", "WaitForChild", "sleep"};

class Visitor : public Luau::AstVisitor {
public:
    std::map<std::string, std::pair<Luau::Location, uint32_t>> globalReads;
    std::set<std::string> globalWrites;
    std::map<std::string, std::pair<Luau::Location, uint32_t>> services;
    std::vector<Finding> findings;
    std::map<std::string, size_t> findingIndex; // dedupe key -> index in findings

    void add(Finding f, const std::string& key) {
        auto it = findingIndex.find(key);
        if (it != findingIndex.end()) {
            ++findings[it->second].occurrences;
            return;
        }
        findingIndex[key] = findings.size();
        findings.push_back(std::move(f));
    }

    bool visit(Luau::AstExprGlobal* node) override {
        auto& entry = globalReads[node->name.value];
        if (entry.second++ == 0) entry.first = node->location;
        return true;
    }

    bool visit(Luau::AstStatAssign* node) override {
        for (Luau::AstExpr* var : node->vars) {
            if (auto* g = var->as<Luau::AstExprGlobal>()) globalWrites.insert(g->name.value);
        }
        // Visit children but do not count assignment targets as reads.
        for (Luau::AstExpr* var : node->vars) {
            if (!var->is<Luau::AstExprGlobal>()) var->visit(this);
        }
        for (Luau::AstExpr* value : node->values) value->visit(this);
        return false;
    }

    bool visit(Luau::AstStatFunction* node) override {
        if (auto* g = node->name->as<Luau::AstExprGlobal>()) globalWrites.insert(g->name.value);
        else node->name->visit(this);
        node->func->visit(this);
        return false;
    }

    void recordService(std::string_view name, const Luau::Location& loc) {
        auto& entry = services[std::string(name)];
        if (entry.second++ == 0) entry.first = loc;
    }

    bool visit(Luau::AstExprIndexName* node) override {
        std::string_view index = node->index.value ? node->index.value : "";
        // game.Players / game.Workspace style service access.
        if (isGlobalNamed(node->expr, "game") && node->op == '.' && lookupService(index)) recordService(index, node->location);
        // Executor extensions of the debug library.
        if (isGlobalNamed(node->expr, "debug")) {
            if (const MemberInfo* m = lookupExecutorDebugMember(index); m && !isStandaloneDebugMember(index)) {
                Finding f;
                f.kind = FindingKind::ExecutorApi;
                f.severity = Severity::Error;
                f.symbol = "debug." + std::string(index);
                f.message = "debug." + std::string(index) + " is a third-party executor API (" + std::string(m->description) +
                            "); it is not provided by Luau, Roblox or Nexus";
                f.range = toRange(node->location);
                f.standalone = f.studio = Availability::Unavailable;
                f.risk = std::string(m->risk);
                add(std::move(f), "dbg:" + std::string(index));
            }
        }
        if (index == "LocalPlayer") {
            Finding f;
            f.kind = FindingKind::ContextRestriction;
            f.severity = Severity::Info;
            f.symbol = "Players.LocalPlayer";
            f.message = "Players.LocalPlayer only exists in a client context (in Studio: the Client data model during Play)";
            f.range = toRange(node->location);
            f.standalone = Availability::Unavailable;
            f.studio = Availability::Partial;
            add(std::move(f), "ctx:LocalPlayer");
        } else if (index == "RenderStepped" || index == "CurrentCamera") {
            Finding f;
            f.kind = FindingKind::ContextRestriction;
            f.severity = Severity::Info;
            f.symbol = std::string(index);
            f.message = std::string(index) + " is only meaningful on a rendering client (in Studio: the Client data model during Play)";
            f.range = toRange(node->location);
            f.standalone = Availability::Unavailable;
            f.studio = Availability::Partial;
            add(std::move(f), "ctx:" + std::string(index));
        }
        return true;
    }

    static bool isStandaloneDebugMember(std::string_view name) {
        const auto& members = standaloneLibraryMembers();
        auto it = members.find("debug");
        return it != members.end() && std::find(it->second.begin(), it->second.end(), name) != it->second.end();
    }

    bool visit(Luau::AstExprCall* node) override {
        // X:GetService("Name") / X:FindService("Name")
        if (auto* idx = node->func->as<Luau::AstExprIndexName>()) {
            std::string_view method = idx->index.value ? idx->index.value : "";
            if ((method == "GetService" || method == "FindService") && node->args.size >= 1) {
                std::string_view name = stringConstant(node->args.data[0]);
                if (!name.empty()) recordService(name, node->location);
            }
            if (node->self) {
                if (const MemberInfo* m = lookupExecutorMember(method)) {
                    Finding f;
                    f.kind = FindingKind::ExecutorApi;
                    f.severity = Severity::Error;
                    f.symbol = ":" + std::string(method);
                    f.message = ":" + std::string(method) + "() is an executor-provided method (" + std::string(m->description) +
                                "); Roblox does not expose it to scripts";
                    f.range = toRange(node->location);
                    f.standalone = f.studio = Availability::Unavailable;
                    f.risk = std::string(m->risk);
                    add(std::move(f), "member:" + std::string(method));
                }
            }
            // string.rep with a huge constant count.
            if (isGlobalNamed(idx->expr, "string") && method == "rep" && node->args.size >= 2) {
                if (auto* n = node->args.data[1]->as<Luau::AstExprConstantNumber>(); n && n->value > 1e8) {
                    Finding f;
                    f.kind = FindingKind::DangerousOperation;
                    f.severity = Severity::Warning;
                    f.symbol = "string.rep";
                    f.message = "string.rep with a very large count can exhaust memory; Nexus enforces its memory limit";
                    f.range = toRange(node->location);
                    add(std::move(f), "rep");
                }
            }
        }

        if (isGlobalNamed(node->func, "loadstring")) {
            bool remote = false;
            if (node->args.size >= 1) {
                if (auto* inner = node->args.data[0]->as<Luau::AstExprCall>()) {
                    if (auto* idx = inner->func->as<Luau::AstExprIndexName>()) {
                        std::string_view m = idx->index.value ? idx->index.value : "";
                        remote = m == "HttpGet" || m == "HttpGetAsync" || m == "GetAsync" || m == "RequestAsync";
                    }
                    if (isGlobalNamed(inner->func, "request") || isGlobalNamed(inner->func, "http_request")) remote = true;
                }
            }
            Finding f;
            f.kind = FindingKind::DangerousOperation;
            f.symbol = "loadstring";
            f.range = toRange(node->location);
            if (remote) {
                f.severity = Severity::Error;
                f.message = "Downloads code from the internet and executes it (loadstring(HttpGet(...))). The downloaded code is "
                            "not visible to this analysis and could do anything the target allows";
                f.risk = "executes unreviewed remote code";
                f.standalone = f.studio = Availability::Unavailable;
                add(std::move(f), "remote-loadstring");
            } else {
                f.severity = Severity::Warning;
                f.message = "loadstring compiles code at runtime; that code is not covered by this analysis. In Roblox it requires "
                            "ServerScriptService.LoadStringEnabled";
                f.risk = "executes dynamically constructed code";
                f.studio = Availability::Partial;
                add(std::move(f), "loadstring");
            }
        }

        if (isGlobalNamed(node->func, "require") && node->args.size >= 1 && node->args.data[0]->is<Luau::AstExprConstantNumber>()) {
            Finding f;
            f.kind = FindingKind::DangerousOperation;
            f.severity = Severity::Warning;
            f.symbol = "require(assetId)";
            f.message = "require(<asset id>) loads a third-party module from the Roblox catalogue at runtime; its code is not "
                        "covered by this analysis";
            f.risk = "executes remote third-party code";
            f.range = toRange(node->location);
            f.standalone = Availability::Unavailable;
            f.studio = Availability::Partial;
            add(std::move(f), "require-asset");
        }

        if (isGlobalNamed(node->func, "getfenv") || isGlobalNamed(node->func, "setfenv")) {
            Finding f;
            f.kind = FindingKind::RuntimeAssumption;
            f.severity = Severity::Info;
            f.symbol = static_cast<Luau::AstExprGlobal*>(node->func)->name.value;
            f.message = f.symbol + " manipulates function environments, which disables Luau optimisations (and import "
                                   "resolution) for the whole script";
            f.range = toRange(node->location);
            add(std::move(f), "fenv:" + f.symbol);
        }
        return true;
    }

    bool visit(Luau::AstStatWhile* node) override {
        auto* cond = node->condition->as<Luau::AstExprConstantBool>();
        if (cond && cond->value) checkUnboundedLoop(node->body, node->location, "while true");
        return true;
    }

    bool visit(Luau::AstStatRepeat* node) override {
        auto* cond = node->condition->as<Luau::AstExprConstantBool>();
        if (cond && !cond->value) checkUnboundedLoop(node->body, node->location, "repeat ... until false");
        return true;
    }

private:
    // Scans a loop body (not nested functions) for calls, yields and exits.
    struct LoopScan : Luau::AstVisitor {
        bool calls = false;
        bool yields = false;
        bool exits = false;
        bool visit(Luau::AstExprFunction*) override { return false; }
        bool visit(Luau::AstStatBreak*) override { exits = true; return false; }
        bool visit(Luau::AstStatReturn*) override { exits = true; return true; }
        bool visit(Luau::AstExprCall* call) override {
            calls = true;
            std::string_view name;
            if (auto* g = call->func->as<Luau::AstExprGlobal>()) name = g->name.value;
            else if (auto* i = call->func->as<Luau::AstExprIndexName>()) name = i->index.value;
            for (const char* y : kYieldNames)
                if (name == y) yields = true;
            return true;
        }
    };

    void checkUnboundedLoop(Luau::AstStatBlock* body, const Luau::Location& loc, const char* form) {
        LoopScan scan;
        body->visit(&scan);
        if (scan.exits || scan.yields) return;
        Finding f;
        f.kind = FindingKind::RuntimeAssumption;
        f.symbol = form;
        f.range = toRange(loc);
        if (!scan.calls) {
            f.severity = Severity::Warning;
            f.message = std::string(form) + " loop never yields or exits; it will run until Nexus stops it at the time limit";
        } else {
            f.severity = Severity::Info;
            f.message = std::string(form) + " loop has no visible yield (task.wait) or exit; unless a called function yields it "
                                            "will block until stopped";
        }
        add(std::move(f), "loop:" + std::to_string(loc.begin.line));
    }
};

Availability worst(Availability a, Availability b) {
    auto rank = [](Availability x) {
        switch (x) {
        case Availability::Available: return 0;
        case Availability::Unknown: return 1;
        case Availability::Partial: return 2;
        case Availability::Unavailable: return 3;
        }
        return 3;
    };
    return rank(a) >= rank(b) ? a : b;
}

} // namespace

CompatibilityReport analyzeCompatibility(std::string_view source) {
    auto start = std::chrono::steady_clock::now();
    CompatibilityReport report;

    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator, Luau::ParseOptions());
    for (const auto& e : parsed.errors) {
        Diagnostic d;
        d.severity = Severity::Error;
        d.code = "syntax";
        d.message = e.getMessage();
        d.range = toRange(e.getLocation());
        d.origin = "parser";
        report.syntax.push_back(std::move(d));
    }
    report.syntaxOk = report.syntax.empty();

    Visitor visitor;
    if (parsed.root) parsed.root->visit(&visitor);

    for (const auto& name : visitor.globalWrites) report.globalsDefined.push_back(name);

    // Classify global reads.
    for (const auto& [name, info] : visitor.globalReads) {
        report.globalsUsed.push_back(name);
        const auto& [loc, count] = info;
        if (isStandaloneGlobal(name)) continue;           // Luau builtin or Nexus extension
        const SymbolInfo* sym = lookupGlobal(name);
        if (!sym && visitor.globalWrites.count(name)) continue; // defined by the script itself

        Finding f;
        f.symbol = name;
        f.range = toRange(loc);
        f.occurrences = count;
        if (!sym) {
            f.kind = FindingKind::UnknownGlobal;
            f.severity = Severity::Warning;
            f.message = "Unknown global '" + name + "': not part of Luau, Roblox or Nexus; it will be nil unless another script defines it";
            f.standalone = Availability::Unknown;
            f.studio = Availability::Unknown;
        } else {
            switch (sym->kind) {
            case SymbolKind::LuauBuiltin:
            case SymbolKind::NexusExtension:
                continue;
            case SymbolKind::RobloxGlobal:
            case SymbolKind::RobloxDatatype:
                f.kind = sym->kind == SymbolKind::RobloxGlobal ? FindingKind::RobloxGlobal : FindingKind::RobloxDatatype;
                f.severity = Severity::Warning;
                f.message = "'" + name + "' is a Roblox engine " + (sym->kind == SymbolKind::RobloxGlobal ? "global" : "datatype") + " (" +
                            std::string(sym->description) + "); it does not exist in the standalone Luau runtime";
                f.standalone = Availability::Unavailable;
                f.studio = sym->studio;
                break;
            case SymbolKind::RobloxDeprecated:
                f.kind = FindingKind::Deprecated;
                f.severity = Severity::Warning;
                f.message = "'" + name + "' is a deprecated Roblox global (" + std::string(sym->description) + ")";
                f.standalone = Availability::Unavailable;
                f.studio = sym->studio;
                break;
            case SymbolKind::ExecutorApi:
                f.kind = FindingKind::ExecutorApi;
                f.severity = Severity::Error;
                f.message = "'" + name + "' is a third-party executor API (" + std::string(sym->description) +
                            "). It is not provided by Luau, Roblox Studio, the Roblox Player or Nexus";
                f.standalone = Availability::Unavailable;
                f.studio = Availability::Unavailable;
                f.risk = std::string(sym->risk);
                break;
            }
        }
        visitor.findings.push_back(std::move(f));
    }

    // Services.
    for (const auto& [name, info] : visitor.services) {
        report.services.push_back(name);
        const ServiceInfo* svc = lookupService(name);
        Finding f;
        f.kind = FindingKind::RobloxService;
        f.symbol = name;
        f.range = toRange(info.first);
        f.occurrences = info.second;
        f.standalone = Availability::Unavailable;
        f.severity = Severity::Info;
        if (!svc) {
            f.message = "Service '" + name + "' is not in Nexus' service catalogue; it may not exist";
            f.studio = Availability::Unknown;
            f.severity = Severity::Warning;
        } else {
            f.message = "Uses Roblox service " + name + " (" + std::string(svc->description) + ")";
            switch (svc->context) {
            case ServiceContext::Any: f.studio = Availability::Available; break;
            case ServiceContext::ClientOnly:
                f.kind = FindingKind::ContextRestriction;
                f.message += "; client-only: in Studio it needs the Client data model during Play";
                f.studio = Availability::Partial;
                break;
            case ServiceContext::ServerOnly:
                f.kind = FindingKind::ContextRestriction;
                f.message += "; server-only: not available on a client";
                f.studio = Availability::Available;
                break;
            case ServiceContext::StudioOnly:
                f.kind = FindingKind::ContextRestriction;
                f.message += "; only available in Roblox Studio";
                f.studio = Availability::Available;
                break;
            case ServiceContext::Restricted:
                f.kind = FindingKind::ContextRestriction;
                f.severity = Severity::Error;
                f.message += "; restricted to Roblox internal code (not accessible to scripts)";
                f.studio = Availability::Unavailable;
                f.risk = "attempts to access protected Roblox internals";
                break;
            }
        }
        visitor.findings.push_back(std::move(f));
    }

    report.findings = std::move(visitor.findings);
    std::stable_sort(report.findings.begin(), report.findings.end(), [](const Finding& a, const Finding& b) {
        if (a.severity != b.severity) return a.severity < b.severity; // errors first
        return a.range.line < b.range.line;
    });

    // Verdicts per target.
    auto verdictFor = [&](const char* id, const char* displayName, Availability Finding::*column) {
        TargetVerdict v;
        v.targetId = id;
        v.displayName = displayName;
        v.verdict = Availability::Available;
        if (!report.syntaxOk) {
            v.verdict = Availability::Unavailable;
            v.blockers.push_back("the script has " + std::to_string(report.syntax.size()) + " syntax error(s)");
        }
        for (const auto& f : report.findings) {
            Availability a = f.*column;
            if (a == Availability::Available) continue;
            v.verdict = worst(v.verdict, a);
            if (a == Availability::Unavailable || a == Availability::Partial)
                v.blockers.push_back(f.symbol + (a == Availability::Partial ? " (conditional)" : ""));
        }
        switch (v.verdict) {
        case Availability::Available: v.summary = "No incompatibilities detected"; break;
        case Availability::Partial: v.summary = "Can run, but some APIs only work in specific contexts"; break;
        case Availability::Unknown: v.summary = "Uses names Nexus cannot classify; results depend on the environment"; break;
        case Availability::Unavailable: v.summary = "Cannot run as written"; break;
        }
        return v;
    };
    report.verdicts.push_back(verdictFor("luau-isolated", "Standalone Luau (Nexus runtime)", &Finding::standalone));
    TargetVerdict studio = verdictFor("roblox-studio", "Roblox Studio (official MCP server)", &Finding::studio);
    studio.summary += "; requires the Studio integration to be connected";
    report.verdicts.push_back(studio);

    TargetVerdict player;
    player.targetId = "roblox-player";
    player.displayName = "Roblox Player (live game client)";
    player.verdict = Availability::Unavailable;
    player.summary = "Not supported: there is no authorized interface for executing code inside the live Roblox Player";
    player.blockers.push_back("no supported execution interface (see docs/feasibility.md)");
    report.verdicts.push_back(player);

    report.analysisMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return report;
}

} // namespace nexus::compat
