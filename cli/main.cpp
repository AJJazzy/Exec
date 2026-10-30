// nexus-cli: command-line front end over the same NexusHost used by the macOS application.

#include "nexus/common/strings.hpp"
#include "nexus/compiler/compiler.hpp"
#include "nexus/editor/editor_services.hpp"
#include "nexus/host/nexus_host.hpp"
#include "nexus/platform/system.hpp"
#include "nexus/roblox/macho.hpp"
#include "nexus/roblox/plist.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <map>
#include <pthread.h>
#include <sstream>
#include <thread>
#include <unistd.h>

#ifndef NEXUS_VERSION
#define NEXUS_VERSION "dev"
#endif

using namespace nexus;

namespace {

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> options; // --key value / --flag (value "1")

    bool flag(const std::string& name) const { return options.count(name) > 0; }
    std::string get(const std::string& name, const std::string& fallback = {}) const {
        auto it = options.find(name);
        return it == options.end() ? fallback : it->second;
    }
};

const std::vector<std::string> kValueOptions = {"--target", "--timeout", "--memory", "--data-model", "--data-dir", "--indent", "--export", "--runner"};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (str::startsWith(s, "--")) {
            bool takesValue = std::find(kValueOptions.begin(), kValueOptions.end(), s) != kValueOptions.end();
            if (takesValue && i + 1 < argc) a.options[s] = argv[++i];
            else a.options[s] = "1";
        } else if (a.command.empty()) {
            a.command = s;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

void usage() {
    std::fprintf(stderr,
                 "Nexus %s — Luau scripting platform\n\n"
                 "usage: nexus-cli <command> [options]\n\n"
                 "commands:\n"
                 "  run <file|->         run a script (--target luau-isolated|luau-inprocess|roblox-studio,\n"
                 "                       --timeout MS, --memory MB, --data-model Edit|Server|Client, --json)\n"
                 "  check <file>         report syntax errors\n"
                 "  analyze <file>       compatibility report (--json)\n"
                 "  format <file>        print formatted source (--write, --indent N, --tabs)\n"
                 "  roblox               detect Roblox Player/Studio and report integration status (--json)\n"
                 "  studio connect       launch Roblox Studio's MCP server and validate it (explicit consent)\n"
                 "  inspect <path>       read-only Mach-O / bundle inspection (--json)\n"
                 "  targets              list execution targets and their availability (--json)\n"
                 "  diagnostics          print diagnostics (--export FILE)\n"
                 "  version              print version information\n\n"
                 "global options: --data-dir DIR (default: application support), --ephemeral (no files written),\n"
                 "                --runner PATH (nexus-runner location)\n",
                 NEXUS_VERSION);
}

Result<std::string> readInput(const std::string& path) {
    if (path == "-") {
        std::string data((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        return data;
    }
    return readFile(path, ScriptLibrary::kMaxScriptBytes);
}

std::unique_ptr<NexusHost> makeHost(const Args& a) {
    HostOptions o;
    if (a.flag("--data-dir")) o.dataDirectory = a.get("--data-dir");
    o.persistent = !a.flag("--ephemeral");
    o.runnerPath = a.get("--runner");
    return std::make_unique<NexusHost>(o);
}

std::atomic<SessionManager*> gSessions{nullptr};

// SIGINT is blocked in every thread and consumed here, so Ctrl-C stops the running script
// cleanly instead of killing the process mid-execution.
void startSignalThread() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set]() {
        int sig = 0;
        int count = 0;
        while (sigwait(&set, &sig) == 0) {
            if (++count > 1) _exit(130);
            if (SessionManager* s = gSessions.load()) {
                std::fprintf(stderr, "\nStopping… (press Ctrl-C again to quit immediately)\n");
                s->stop();
            } else {
                _exit(130);
            }
        }
    }).detach();
}

int exitCodeFor(ExecutionStatus s) {
    switch (s) {
    case ExecutionStatus::Succeeded: return 0;
    case ExecutionStatus::CompileError:
    case ExecutionStatus::RuntimeError: return 1;
    case ExecutionStatus::Unavailable: return 3;
    case ExecutionStatus::MemoryLimit: return 4;
    case ExecutionStatus::InternalError: return 5;
    case ExecutionStatus::TimedOut: return 124;
    case ExecutionStatus::Cancelled: return 130;
    }
    return 5;
}

int cmdRun(const Args& a) {
    if (a.positional.empty()) {
        usage();
        return 2;
    }
    auto src = readInput(a.positional[0]);
    if (!src.ok()) {
        std::fprintf(stderr, "error: %s\n", src.error().message.c_str());
        return 2;
    }
    auto host = makeHost(a);
    Settings s = host->settings();
    if (a.flag("--timeout")) s.timeoutMs = std::stoll(a.get("--timeout"));
    if (a.flag("--memory")) s.memoryLimitMB = std::stoull(a.get("--memory"));
    if (a.flag("--data-model")) s.studioDataModel = a.get("--data-model");
    // CLI overrides apply to this invocation only (validated/clamped like saved settings).
    s = Settings::fromJson(s.toJson());
    std::string target = a.get("--target", s.defaultTarget);
    bool json = a.flag("--json");

    if (target == studio::StudioIntegration::kId) {
        auto st = host->connectStudio();
        if (st.state != roblox::ConnectionState::Connected) {
            std::fprintf(stderr, "Roblox Studio is not connected (%s): %s\n", roblox::toString(st.state), st.message.c_str());
            return 3;
        }
    }

    gSessions = &host->sessions();
    host->sessions().subscribe([json](const SessionEvent& e) {
        if (e.kind != SessionEventKind::Output || !e.line || json) return;
        const ConsoleLine& l = *e.line;
        switch (l.stream) {
        case OutputStream::Output: std::fprintf(stdout, "%s\n", l.text.c_str()); std::fflush(stdout); break;
        case OutputStream::Warning: std::fprintf(stderr, "warning: %s\n", l.text.c_str()); break;
        case OutputStream::Error: std::fprintf(stderr, "error: %s\n", l.text.c_str()); break;
        case OutputStream::Info:
        case OutputStream::System: std::fprintf(stderr, "[nexus] %s\n", l.text.c_str()); break;
        }
    });

    ExecutionRequest req;
    req.source = src.value();
    req.scriptName = a.positional[0] == "-" ? "stdin" : std::filesystem::path(a.positional[0]).stem().string();
    req.limits = s.limits();
    req.options["dataModel"] = s.studioDataModel;
    auto started = host->sessions().start(req, target);
    if (!started.ok()) {
        std::fprintf(stderr, "error: %s\n", started.error().message.c_str());
        return 2;
    }
    while (!host->sessions().waitForIdle(std::chrono::seconds(3600))) {
    }
    gSessions = nullptr;
    SessionSnapshot snap = host->sessions().current();
    if (!snap.result) return 5;
    if (json) std::printf("%s\n", json::dump(json::Object{{"state", toString(snap.state)}, {"result", snap.result->toJson()}}, 2).c_str());
    else if (!snap.result->succeeded() && snap.result->error && !snap.result->error->traceback.empty())
        std::fprintf(stderr, "stack trace:\n%s", snap.result->error->traceback.c_str());
    return exitCodeFor(snap.result->status);
}

int cmdCheck(const Args& a) {
    if (a.positional.empty()) return usage(), 2;
    auto src = readInput(a.positional[0]);
    if (!src.ok()) return std::fprintf(stderr, "error: %s\n", src.error().message.c_str()), 2;
    auto diags = checkSyntax(src.value());
    for (const auto& d : diags) std::printf("%s\n", formatDiagnostic(d, a.positional[0]).c_str());
    if (diags.empty()) std::printf("%s: no syntax errors\n", a.positional[0].c_str());
    return diags.empty() ? 0 : 1;
}

int cmdAnalyze(const Args& a) {
    if (a.positional.empty()) return usage(), 2;
    auto src = readInput(a.positional[0]);
    if (!src.ok()) return std::fprintf(stderr, "error: %s\n", src.error().message.c_str()), 2;
    auto report = compat::analyzeCompatibility(src.value());
    if (a.flag("--json")) std::printf("%s\n", json::dump(report.toJson(), 2).c_str());
    else std::printf("%s", report.toText().c_str());
    return report.syntaxOk ? 0 : 1;
}

int cmdFormat(const Args& a) {
    if (a.positional.empty()) return usage(), 2;
    auto src = readInput(a.positional[0]);
    if (!src.ok()) return std::fprintf(stderr, "error: %s\n", src.error().message.c_str()), 2;
    editor::FormatOptions o;
    if (a.flag("--indent")) o.indentWidth = std::max(1, std::min(8, std::stoi(a.get("--indent"))));
    o.useTabs = a.flag("--tabs");
    auto r = editor::format(src.value(), o);
    if (!r.ok) return std::fprintf(stderr, "error: %s\n", r.error.c_str()), 1;
    if (a.flag("--write") && a.positional[0] != "-") {
        if (auto w = writeFileAtomic(a.positional[0], r.text); !w.ok()) return std::fprintf(stderr, "error: %s\n", w.error().message.c_str()), 1;
        std::fprintf(stderr, "%s %s\n", a.positional[0].c_str(), r.changed ? "formatted" : "already formatted");
    } else {
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return 0;
}

void printPlayer(const roblox::PlayerStatus& p) {
    std::printf("Roblox Player\n  state:   %s\n  summary: %s\n", roblox::toString(p.state), p.summary.c_str());
    if (p.installed) {
        const auto& i = *p.installed;
        std::printf("  bundle:  %s\n  version: %s (build %s), minimum macOS %s\n", i.bundlePath.c_str(), i.version.c_str(), i.build.c_str(),
                    i.minimumSystemVersion.c_str());
        if (i.binary) std::printf("  arch:    %s\n", str::join(i.binary->architectures(), ", ").c_str());
        if (i.signature.supported)
            std::printf("  signature: %s (team %s)\n", i.signature.valid ? "valid" : ("INVALID: " + i.signature.error).c_str(), i.signature.teamIdentifier.c_str());
    }
    for (const auto& r : p.running)
        std::printf("  running: pid %d%s\n", r.pid, r.translated ? (*r.translated ? " (Rosetta 2)" : " (native)") : "");
    if (p.previousVersion) std::printf("  updated: previously %s\n", p.previousVersion->c_str());
    if (!p.unsupportedReasons.empty()) {
        std::printf("  live execution: NOT SUPPORTED\n");
        for (const auto& r : p.unsupportedReasons) std::printf("    - %s\n", r.c_str());
    }
}

void printStudio(const studio::StudioStatus& s) {
    std::printf("Roblox Studio\n  state:   %s\n  message: %s\n", roblox::toString(s.state), s.message.c_str());
    if (s.installed) std::printf("  bundle:  %s\n  version: %s\n", s.installed->bundlePath.c_str(), s.installed->version.c_str());
    if (!s.mcpPath.empty()) std::printf("  MCP:     %s%s\n", s.mcpPath.c_str(), s.usingOverride ? " (override)" : "");
    if (!s.tools.empty()) std::printf("  tools:   %s\n", str::join(s.tools, ", ").c_str());
    if (!s.mapping.tool.empty())
        std::printf("  execute: %s(code=%s%s%s)\n", s.mapping.tool.c_str(), s.mapping.codeParam.c_str(),
                    s.mapping.dataModelParam.empty() ? "" : (", dataModel=" + s.mapping.dataModelParam).c_str(),
                    s.mapping.studioIdParam.empty() ? "" : (", studio=" + s.mapping.studioIdParam).c_str());
    for (const auto& st : s.studios) std::printf("  studio:  %s %s %s\n", st.id.c_str(), st.name.c_str(), st.placeId.c_str());
}

int cmdRoblox(const Args& a) {
    auto host = makeHost(a);
    auto p = host->refreshRoblox();
    auto s = host->refreshStudio();
    if (a.flag("--json")) {
        std::printf("%s\n", json::dump(json::Object{{"player", p.toJson()}, {"studio", s.toJson()}}, 2).c_str());
    } else {
        printPlayer(p);
        std::printf("\n");
        printStudio(s);
    }
    return 0;
}

int cmdStudio(const Args& a) {
    std::string sub = a.positional.empty() ? "status" : a.positional[0];
    auto host = makeHost(a);
    studio::StudioStatus s = sub == "connect" ? host->connectStudio() : host->refreshStudio();
    if (a.flag("--json")) std::printf("%s\n", json::dump(s.toJson(), 2).c_str());
    else printStudio(s);
    return s.state == roblox::ConnectionState::Connected || sub != "connect" ? 0 : 3;
}

int cmdInspect(const Args& a) {
    if (a.positional.empty()) return usage(), 2;
    std::filesystem::path p = a.positional[0];
    std::error_code ec;
    json::Object out;
    std::filesystem::path binary = p;
    if (std::filesystem::is_directory(p, ec)) {
        auto plist = roblox::readPlistFile(p / "Contents" / "Info.plist");
        if (!plist.ok()) return std::fprintf(stderr, "error: %s\n", plist.error().message.c_str()), 1;
        out["infoPlist"] = plist.value();
        binary = p / "Contents" / "MacOS" / plist.value().getString("CFBundleExecutable");
    }
    auto info = roblox::inspectMachO(binary);
    if (!info.ok()) return std::fprintf(stderr, "error: %s\n", info.error().message.c_str()), 1;
    out["binary"] = info.value().toJson();
    if (!info.value().slices.empty()) out["securityPosture"] = roblox::evaluateSecurityPosture(info.value().slices.front().signature).toJson();
    if (a.flag("--json")) {
        std::printf("%s\n", json::dump(out, 2).c_str());
        return 0;
    }
    std::printf("%s: %s\n", binary.string().c_str(), info.value().universal ? "universal binary" : "single-architecture binary");
    for (const auto& s : info.value().slices) {
        std::printf("  %-7s minOS %-6s SDK %-6s signature: %s", s.architecture.c_str(), s.minimumOS.c_str(), s.sdk.c_str(),
                    s.signature.present ? "present" : "none");
        if (s.signature.present)
            std::printf(" id=%s team=%s flags=0x%x%s", s.signature.identifier.c_str(), s.signature.teamId.empty() ? "-" : s.signature.teamId.c_str(),
                        s.signature.flags, s.signature.hardenedRuntime ? " [hardened runtime]" : "");
        std::printf("\n");
        for (const auto& [k, v] : s.signature.entitlements) std::printf("      entitlement %s = %s\n", k.c_str(), json::dump(v).c_str());
    }
    if (!info.value().slices.empty())
        for (const auto& e : roblox::evaluateSecurityPosture(info.value().slices.front().signature).explanations) std::printf("  - %s\n", e.c_str());
    return 0;
}

int cmdTargets(const Args& a) {
    auto host = makeHost(a);
    host->refreshRoblox();
    host->refreshStudio();
    json::Array arr;
    for (const auto& t : host->targets().list()) {
        auto info = t->info();
        auto av = t->availability();
        if (a.flag("--json")) {
            json::Value v = info.toJson();
            v["available"] = av.available;
            v["availability"] = av.reason;
            arr.push_back(std::move(v));
        } else {
            std::printf("%-16s %-12s %s\n    %s\n", info.id.c_str(), av.available ? "available" : "unavailable", info.displayName.c_str(), av.reason.c_str());
        }
    }
    if (a.flag("--json")) std::printf("%s\n", json::dump(arr, 2).c_str());
    return 0;
}

int cmdDiagnostics(const Args& a) {
    auto host = makeHost(a);
    host->refreshRoblox();
    host->refreshStudio();
    if (a.flag("--export")) {
        auto r = host->exportDiagnostics(a.get("--export"));
        if (!r.ok()) return std::fprintf(stderr, "error: %s\n", r.error().message.c_str()), 1;
        std::fprintf(stderr, "Diagnostics written to %s\n", a.get("--export").c_str());
        return 0;
    }
    std::printf("%s\n", json::dump(host->diagnosticsSnapshot(), 2).c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a = parseArgs(argc, argv);
    if (a.command.empty() || a.command == "help" || a.flag("--help")) {
        usage();
        return a.command.empty() ? 2 : 0;
    }
    startSignalThread();
    try {
        if (a.command == "version") {
            std::printf("nexus-cli %s\nLuau %s\n%s\n", NEXUS_VERSION, luauVersion(), platform::operatingSystemDescription().c_str());
            return 0;
        }
        if (a.command == "run") return cmdRun(a);
        if (a.command == "check") return cmdCheck(a);
        if (a.command == "analyze") return cmdAnalyze(a);
        if (a.command == "format") return cmdFormat(a);
        if (a.command == "roblox") return cmdRoblox(a);
        if (a.command == "studio") return cmdStudio(a);
        if (a.command == "inspect") return cmdInspect(a);
        if (a.command == "targets") return cmdTargets(a);
        if (a.command == "diagnostics") return cmdDiagnostics(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 5;
    }
    std::fprintf(stderr, "unknown command '%s'\n\n", a.command.c_str());
    usage();
    return 2;
}
