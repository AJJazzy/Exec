// Objective-C++ implementation of NXBridge.h on top of nexus::NexusHost.
#import "NXBridge.h"

#import <AppKit/AppKit.h>

#include "nexus/common/strings.hpp"
#include "nexus/compiler/compiler.hpp"
#include "nexus/compiler/source_map.hpp"
#include "nexus/editor/editor_services.hpp"
#include "nexus/host/nexus_host.hpp"
#include "nexus/platform/system.hpp"

#include <atomic>
#include <memory>

#ifndef NEXUS_VERSION
#define NEXUS_VERSION "dev"
#endif

namespace {

NSString* NS(const std::string& s) {
    NSString* out = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return out ?: @"";
}

std::string STD(NSString* s) {
    if (!s) return {};
    const char* utf8 = s.UTF8String;
    return utf8 ? std::string(utf8) : std::string();
}

NSError* makeError(const nexus::Error& e) {
    return [NSError errorWithDomain:@"io.github.ajjazzy.nexus"
                               code:1
                           userInfo:@{NSLocalizedDescriptionKey : NS(e.message), @"NexusErrorCode" : NS(e.code)}];
}

template <class T>
BOOL fail(const nexus::Result<T>& r, NSError** error) {
    if (r.ok()) return YES;
    if (error) *error = makeError(r.error());
    return NO;
}

NSDate* toDate(std::chrono::system_clock::time_point tp) {
    return [NSDate dateWithTimeIntervalSince1970:static_cast<double>(nexus::str::unixMillis(tp)) / 1000.0];
}

NSArray<NSString*>* toArray(const std::vector<std::string>& v) {
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:v.size()];
    for (const auto& s : v) [a addObject:NS(s)];
    return a;
}

// UTF-16 offset -> UTF-8 byte offset within `source`.
size_t byteOffset(NSString* source, NSUInteger utf16) {
    NSUInteger clamped = MIN(utf16, source.length);
    NSRange composed = [source rangeOfComposedCharacterSequenceAtIndex:clamped == source.length ? (clamped ? clamped - 1 : 0) : clamped];
    if (clamped < source.length && composed.location < clamped) clamped = composed.location; // never split a surrogate pair
    return [[source substringToIndex:clamped] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

NSRange utf16Range(const nexus::SourceMap& map, const nexus::SourceRange& r) {
    if (!r.valid()) return NSMakeRange(NSNotFound, 0);
    size_t begin = map.offsetOf(r.line - 1, r.column ? r.column - 1 : 0);
    size_t end = r.endLine ? map.offsetOf(r.endLine - 1, r.endColumn ? r.endColumn - 1 : 0) : begin;
    if (end <= begin) end = begin + 1;
    size_t u0 = map.utf16Offset(begin);
    size_t u1 = map.utf16Offset(end);
    if (u1 <= u0) u1 = u0 + 1;
    return NSMakeRange(u0, u1 - u0);
}

NXSessionState toState(nexus::SessionState s) { return static_cast<NXSessionState>(static_cast<int>(s)); }
NXOutputStream toStream(nexus::OutputStream s) { return static_cast<NXOutputStream>(static_cast<int>(s)); }

} // namespace

// ---------------------------------------------------------------------------------------------
// Value classes

@interface NXDiagnostic ()
+ (instancetype)nx_from:(const nexus::Diagnostic&)d map:(const nexus::SourceMap*)map;
@end
@implementation NXDiagnostic
+ (instancetype)nx_from:(const nexus::Diagnostic&)d map:(const nexus::SourceMap*)map {
    NXDiagnostic* o = [self new];
    o->_severity = NS(nexus::toString(d.severity));
    o->_code = NS(d.code);
    o->_message = NS(d.message);
    o->_origin = NS(d.origin);
    o->_line = d.range.line;
    o->_column = d.range.column;
    o->_range = map ? utf16Range(*map, d.range) : NSMakeRange(NSNotFound, 0);
    return o;
}
@end

@interface NXConsoleLine ()
+ (instancetype)nx_from:(const nexus::ConsoleLine&)l sequence:(uint64_t)seq;
@end
@implementation NXConsoleLine
+ (instancetype)nx_from:(const nexus::ConsoleLine&)l sequence:(uint64_t)seq {
    NXConsoleLine* o = [self new];
    o->_sequence = seq;
    o->_sessionId = l.sessionId;
    o->_stream = toStream(l.stream);
    o->_text = NS(l.text);
    o->_time = toDate(l.time);
    o->_elapsedMs = l.elapsedMs;
    return o;
}
@end

@interface NXSessionSnapshot ()
+ (instancetype)nx_from:(const nexus::SessionSnapshot&)s;
@end
@implementation NXSessionSnapshot
+ (instancetype)nx_from:(const nexus::SessionSnapshot&)s {
    NXSessionSnapshot* o = [self new];
    o->_sessionId = s.id;
    o->_state = toState(s.state);
    o->_stateName = NS(nexus::toString(s.state));
    o->_scriptName = NS(s.scriptName);
    o->_targetId = NS(s.targetId);
    o->_message = NS(s.message);
    o->_startedAt = s.id ? toDate(s.startedAt) : nil;
    o->_active = nexus::isActive(s.state);
    o->_hasResult = s.result.has_value();
    o->_resultStatus = s.result ? NS(nexus::toString(s.result->status)) : @"";
    o->_resultSummary = s.result ? NS(s.result->summary) : @"";
    o->_errorLine = s.result && s.result->error ? s.result->error->line : 0;
    o->_errorMessage = s.result && s.result->error ? NS(s.result->error->message) : @"";
    o->_traceback = s.result && s.result->error ? NS(s.result->error->traceback) : @"";
    if (s.result) {
        const auto& st = s.result->stats;
        o->_wallMs = st.wallMs;
        o->_cpuMs = st.cpuMs;
        o->_compileMs = st.compileMs;
        o->_peakMemoryBytes = st.peakMemoryBytes;
        o->_outputBytes = st.outputBytes;
        o->_threadsCreated = st.threadsCreated;
    }
    NSMutableArray* diags = [NSMutableArray array];
    if (s.result)
        for (const auto& d : s.result->diagnostics) [diags addObject:[NXDiagnostic nx_from:d map:nullptr]];
    o->_diagnostics = diags;
    return o;
}
@end

@interface NXExecutionRecord ()
+ (instancetype)nx_from:(const nexus::ExecutionRecord&)r;
@end
@implementation NXExecutionRecord
+ (instancetype)nx_from:(const nexus::ExecutionRecord&)r {
    NXExecutionRecord* o = [self new];
    o->_sessionId = r.sessionId;
    o->_scriptName = NS(r.scriptName);
    o->_targetId = NS(r.targetId);
    o->_startedAt = toDate(r.startedAt);
    o->_status = NS(nexus::toString(r.result.status));
    o->_finalState = NS(nexus::toString(r.finalState));
    o->_summary = NS(r.result.summary);
    o->_wallMs = r.result.stats.wallMs;
    o->_cpuMs = r.result.stats.cpuMs;
    o->_peakMemoryBytes = r.result.stats.peakMemoryBytes;
    o->_sourceBytes = r.sourceBytes;
    return o;
}
@end

@interface NXTargetInfo ()
+ (instancetype)nx_from:(const nexus::TargetInfo&)i availability:(const nexus::TargetAvailability&)a;
@end
@implementation NXTargetInfo
+ (instancetype)nx_from:(const nexus::TargetInfo&)i availability:(const nexus::TargetAvailability&)a {
    NXTargetInfo* o = [self new];
    o->_identifier = NS(i.id);
    o->_displayName = NS(i.displayName);
    o->_detail = NS(i.description);
    o->_kind = NS(nexus::toString(i.kind));
    o->_available = a.available;
    o->_availability = NS(a.reason);
    o->_robloxEngineApis = i.capabilities.robloxEngineApis;
    o->_guaranteedStop = i.capabilities.guaranteedStop;
    o->_processIsolation = i.capabilities.processIsolation;
    o->_resourceLimits = i.capabilities.resourceLimits;
    return o;
}
@end

@interface NXCompletionItem ()
+ (instancetype)nx_from:(const nexus::editor::CompletionItem&)c;
@end
@implementation NXCompletionItem
+ (instancetype)nx_from:(const nexus::editor::CompletionItem&)c {
    NXCompletionItem* o = [self new];
    o->_label = NS(c.label);
    o->_kind = NS(nexus::editor::toString(c.kind));
    o->_detail = NS(c.detail);
    o->_insertText = NS(c.insertText.empty() ? c.label : c.insertText);
    return o;
}
@end

@interface NXCompletionResult ()
+ (instancetype)nx_range:(NSRange)range items:(NSArray<NXCompletionItem*>*)items;
@end
@implementation NXCompletionResult
+ (instancetype)nx_range:(NSRange)range items:(NSArray<NXCompletionItem*>*)items {
    NXCompletionResult* o = [self new];
    o->_replaceRange = range;
    o->_items = items;
    return o;
}
@end

@interface NXCompatFinding ()
+ (instancetype)nx_from:(const nexus::compat::Finding&)f;
@end
@implementation NXCompatFinding
+ (instancetype)nx_from:(const nexus::compat::Finding&)f {
    NXCompatFinding* o = [self new];
    o->_kind = NS(nexus::compat::toString(f.kind));
    o->_severity = NS(nexus::toString(f.severity));
    o->_symbol = NS(f.symbol);
    o->_message = NS(f.message);
    o->_line = f.range.line;
    o->_occurrences = f.occurrences;
    o->_standalone = NS(nexus::compat::toString(f.standalone));
    o->_studio = NS(nexus::compat::toString(f.studio));
    o->_risk = NS(f.risk);
    return o;
}
@end

@interface NXCompatVerdict ()
+ (instancetype)nx_from:(const nexus::compat::TargetVerdict&)v;
@end
@implementation NXCompatVerdict
+ (instancetype)nx_from:(const nexus::compat::TargetVerdict&)v {
    NXCompatVerdict* o = [self new];
    o->_targetId = NS(v.targetId);
    o->_displayName = NS(v.displayName);
    o->_verdict = NS(nexus::compat::toString(v.verdict));
    o->_summary = NS(v.summary);
    o->_blockers = toArray(v.blockers);
    return o;
}
@end

@interface NXCompatReport ()
+ (instancetype)nx_from:(const nexus::compat::CompatibilityReport&)r map:(const nexus::SourceMap&)map;
@end
@implementation NXCompatReport
+ (instancetype)nx_from:(const nexus::compat::CompatibilityReport&)r map:(const nexus::SourceMap&)map {
    NXCompatReport* o = [self new];
    o->_syntaxOk = r.syntaxOk;
    NSMutableArray* syn = [NSMutableArray array];
    for (const auto& d : r.syntax) [syn addObject:[NXDiagnostic nx_from:d map:&map]];
    o->_syntaxErrors = syn;
    NSMutableArray* fin = [NSMutableArray array];
    for (const auto& f : r.findings) [fin addObject:[NXCompatFinding nx_from:f]];
    o->_findings = fin;
    NSMutableArray* ver = [NSMutableArray array];
    for (const auto& v : r.verdicts) [ver addObject:[NXCompatVerdict nx_from:v]];
    o->_verdicts = ver;
    o->_services = toArray(r.services);
    o->_globalsUsed = toArray(r.globalsUsed);
    NSMutableArray* ed = [NSMutableArray array];
    for (const auto& d : r.toDiagnostics()) [ed addObject:[NXDiagnostic nx_from:d map:&map]];
    o->_editorDiagnostics = ed;
    o->_analysisMs = r.analysisMs;
    o->_text = NS(r.toText());
    o->_json = NS(nexus::json::dump(r.toJson(), 2));
    return o;
}
@end

@interface NXLibraryItem ()
+ (instancetype)nx_from:(const nexus::LibraryEntry&)e;
@end
@implementation NXLibraryItem
+ (instancetype)nx_from:(const nexus::LibraryEntry&)e {
    NXLibraryItem* o = [self new];
    o->_path = NS(e.path);
    o->_name = NS(e.name);
    o->_isFolder = e.isFolder;
    o->_isProject = e.isProject;
    o->_favorite = e.favorite;
    o->_sizeBytes = e.sizeBytes;
    o->_modified = toDate(e.modified);
    if (e.isFolder) {
        NSMutableArray* kids = [NSMutableArray arrayWithCapacity:e.children.size()];
        for (const auto& c : e.children) [kids addObject:[NXLibraryItem nx_from:c]];
        o->_children = kids;
    }
    return o;
}
@end

@interface NXSearchHit ()
+ (instancetype)nx_from:(const nexus::LibrarySearchHit&)h;
@end
@implementation NXSearchHit
+ (instancetype)nx_from:(const nexus::LibrarySearchHit&)h {
    NXSearchHit* o = [self new];
    o->_path = NS(h.path);
    o->_nameMatch = h.nameMatch;
    o->_line = h.line;
    o->_preview = NS(h.preview);
    return o;
}
@end

@implementation NXDraft
- (instancetype)init {
    if ((self = [super init])) {
        _draftId = @"";
        _title = @"";
        _content = @"";
        _libraryPath = @"";
    }
    return self;
}
@end

namespace {
NSArray<NSString*>* historyStrings(const std::vector<nexus::roblox::StateTransition>& history) {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& h : history)
        [a addObject:NS(nexus::str::isoTimestamp(h.time) + "  " + nexus::roblox::toString(h.from) + " → " + nexus::roblox::toString(h.to) +
                        (h.reason.empty() ? std::string() : "  (" + h.reason + ")"))];
    return a;
}
} // namespace

@interface NXRobloxStatus ()
+ (instancetype)nx_from:(const nexus::roblox::PlayerStatus&)p;
@end
@implementation NXRobloxStatus
+ (instancetype)nx_from:(const nexus::roblox::PlayerStatus&)p {
    NXRobloxStatus* o = [self new];
    o->_state = NS(nexus::roblox::toString(p.state));
    o->_summary = NS(p.summary);
    o->_installed = p.installed.has_value();
    o->_bundlePath = p.installed ? NS(p.installed->bundlePath) : @"";
    o->_version = p.installed ? NS(p.installed->version) : @"";
    o->_build = p.installed ? NS(p.installed->build) : @"";
    o->_minimumSystemVersion = p.installed ? NS(p.installed->minimumSystemVersion) : @"";
    o->_architectures = p.installed && p.installed->binary ? toArray(p.installed->binary->architectures()) : @[];
    o->_signatureChecked = p.installed && p.installed->signature.supported;
    o->_signatureValid = p.installed && p.installed->signature.valid;
    o->_signatureTeamId = p.installed ? NS(p.installed->signature.teamIdentifier) : @"";
    o->_signatureError = p.installed ? NS(p.installed->signature.error) : @"";
    std::vector<std::string> notes;
    if (p.installed && p.installed->binary && !p.installed->binary->slices.empty()) {
        auto posture = nexus::roblox::evaluateSecurityPosture(p.installed->binary->slices.front().signature);
        o->_hardenedRuntime = posture.hardenedRuntime;
        notes = posture.explanations;
    }
    o->_securityNotes = toArray(notes);
    NSMutableArray* pids = [NSMutableArray array];
    NSMutableArray* desc = [NSMutableArray array];
    for (const auto& r : p.running) {
        [pids addObject:@(r.pid)];
        std::string mode = r.translated ? (*r.translated ? "x86_64 under Rosetta 2" : "native") : "architecture unknown";
        [desc addObject:NS("pid " + std::to_string(r.pid) + " — " + mode)];
    }
    o->_runningPids = pids;
    o->_runningDescriptions = desc;
    o->_unsupportedReasons = toArray(p.unsupportedReasons);
    o->_notes = toArray(p.notes);
    o->_previousVersion = p.previousVersion ? NS(*p.previousVersion) : @"";
    o->_generation = p.generation;
    o->_history = historyStrings(p.history);
    o->_checkedAt = p.checkedAt.time_since_epoch().count() ? toDate(p.checkedAt) : nil;
    o->_json = NS(nexus::json::dump(p.toJson(), 2));
    return o;
}
@end

@interface NXStudioInstance ()
+ (instancetype)nx_from:(const nexus::studio::StudioInstance&)s;
@end
@implementation NXStudioInstance
+ (instancetype)nx_from:(const nexus::studio::StudioInstance&)s {
    NXStudioInstance* o = [self new];
    o->_identifier = NS(s.id);
    o->_name = NS(s.name);
    o->_placeId = NS(s.placeId);
    return o;
}
@end

@interface NXStudioStatus ()
+ (instancetype)nx_from:(const nexus::studio::StudioStatus&)s;
@end
@implementation NXStudioStatus
+ (instancetype)nx_from:(const nexus::studio::StudioStatus&)s {
    NXStudioStatus* o = [self new];
    o->_state = NS(nexus::roblox::toString(s.state));
    o->_message = NS(s.message);
    o->_installed = s.installed.has_value();
    o->_version = s.installed ? NS(s.installed->version) : @"";
    o->_bundlePath = s.installed ? NS(s.installed->bundlePath) : @"";
    NSMutableArray* pids = [NSMutableArray array];
    for (const auto& r : s.running) [pids addObject:@(r.pid)];
    o->_runningPids = pids;
    o->_mcpPath = NS(s.mcpPath);
    o->_mcpSignatureValid = s.mcpSignature.valid;
    o->_mcpSignatureError = NS(s.mcpSignature.error);
    o->_serverName = NS(s.serverName);
    o->_serverVersion = NS(s.serverVersion);
    o->_protocolVersion = NS(s.protocolVersion);
    o->_tools = toArray(s.tools);
    o->_executeTool = NS(s.mapping.tool);
    NSMutableArray* studios = [NSMutableArray array];
    for (const auto& st : s.studios) [studios addObject:[NXStudioInstance nx_from:st]];
    o->_studios = studios;
    o->_selectedStudioId = NS(s.selectedStudioId);
    o->_connected = s.state == nexus::roblox::ConnectionState::Connected;
    o->_history = historyStrings(s.history);
    o->_json = NS(nexus::json::dump(s.toJson(), 2));
    return o;
}
@end

@implementation NXSettings
- (id)copyWithZone:(NSZone*)zone {
    NXSettings* c = [[NXSettings allocWithZone:zone] init];
    c.appearance = self.appearance;
    c.accentColor = self.accentColor;
    c.editorFontSize = self.editorFontSize;
    c.editorFontName = self.editorFontName;
    c.indentWidth = self.indentWidth;
    c.useTabs = self.useTabs;
    c.autoCloseBrackets = self.autoCloseBrackets;
    c.autoIndent = self.autoIndent;
    c.highlightMatchingBrackets = self.highlightMatchingBrackets;
    c.liveDiagnostics = self.liveDiagnostics;
    c.autocomplete = self.autocomplete;
    c.showMinimap = self.showMinimap;
    c.restoreDrafts = self.restoreDrafts;
    c.defaultTarget = self.defaultTarget;
    c.timeoutMs = self.timeoutMs;
    c.memoryLimitMB = self.memoryLimitMB;
    c.maxOutputKB = self.maxOutputKB;
    c.runnerSandbox = self.runnerSandbox;
    c.studioDataModel = self.studioDataModel;
    c.allowRobloxVersionCheck = self.allowRobloxVersionCheck;
    c.studioIntegrationEnabled = self.studioIntegrationEnabled;
    c.requireRobloxSignature = self.requireRobloxSignature;
    c.consoleCapacity = self.consoleCapacity;
    return c;
}
@end

namespace {
NXSettings* toNX(const nexus::Settings& s) {
    NXSettings* o = [NXSettings new];
    o.appearance = NS(s.appearance);
    o.accentColor = NS(s.accentColor);
    o.editorFontSize = s.editorFontSize;
    o.editorFontName = NS(s.editorFontName);
    o.indentWidth = s.indentWidth;
    o.useTabs = s.useTabs;
    o.autoCloseBrackets = s.autoCloseBrackets;
    o.autoIndent = s.autoIndent;
    o.highlightMatchingBrackets = s.highlightMatchingBrackets;
    o.liveDiagnostics = s.liveDiagnostics;
    o.autocomplete = s.autocomplete;
    o.showMinimap = s.showMinimap;
    o.restoreDrafts = s.restoreDrafts;
    o.defaultTarget = NS(s.defaultTarget);
    o.timeoutMs = static_cast<NSInteger>(s.timeoutMs);
    o.memoryLimitMB = static_cast<NSInteger>(s.memoryLimitMB);
    o.maxOutputKB = static_cast<NSInteger>(s.maxOutputKB);
    o.runnerSandbox = s.runnerSandbox;
    o.studioDataModel = NS(s.studioDataModel);
    o.allowRobloxVersionCheck = s.allowRobloxVersionCheck;
    o.studioIntegrationEnabled = s.studioIntegrationEnabled;
    o.requireRobloxSignature = s.requireRobloxSignature;
    o.consoleCapacity = s.consoleCapacity;
    return o;
}

nexus::Settings fromNX(NXSettings* o) {
    nexus::Settings s;
    s.appearance = STD(o.appearance);
    s.accentColor = STD(o.accentColor);
    s.editorFontSize = o.editorFontSize;
    s.editorFontName = STD(o.editorFontName);
    s.indentWidth = static_cast<int>(o.indentWidth);
    s.useTabs = o.useTabs;
    s.autoCloseBrackets = o.autoCloseBrackets;
    s.autoIndent = o.autoIndent;
    s.highlightMatchingBrackets = o.highlightMatchingBrackets;
    s.liveDiagnostics = o.liveDiagnostics;
    s.autocomplete = o.autocomplete;
    s.showMinimap = o.showMinimap;
    s.restoreDrafts = o.restoreDrafts;
    s.defaultTarget = STD(o.defaultTarget);
    s.timeoutMs = o.timeoutMs;
    s.memoryLimitMB = static_cast<uint64_t>(MAX(o.memoryLimitMB, 0));
    s.maxOutputKB = static_cast<uint64_t>(MAX(o.maxOutputKB, 0));
    s.runnerSandbox = o.runnerSandbox;
    s.studioDataModel = STD(o.studioDataModel);
    s.allowRobloxVersionCheck = o.allowRobloxVersionCheck;
    s.studioIntegrationEnabled = o.studioIntegrationEnabled;
    s.requireRobloxSignature = o.requireRobloxSignature;
    s.consoleCapacity = static_cast<int>(o.consoleCapacity);
    return nexus::Settings::fromJson(s.toJson()); // validate/clamp
}
} // namespace

@interface NXLogEntry ()
+ (instancetype)nx_from:(const nexus::LogEntry&)e;
@end
@implementation NXLogEntry
+ (instancetype)nx_from:(const nexus::LogEntry&)e {
    NXLogEntry* o = [self new];
    o->_sequence = e.sequence;
    o->_time = toDate(e.time);
    o->_level = NS(nexus::toString(e.level));
    o->_category = NS(e.category);
    o->_message = NS(e.message);
    std::string fields;
    for (const auto& [k, v] : e.fields) fields += (fields.empty() ? "" : "  ") + k + "=" + v;
    o->_fields = NS(fields);
    return o;
}
@end

// ---------------------------------------------------------------------------------------------
// NXHost

static void* kRunningAppsContext = &kRunningAppsContext;

@implementation NXHost {
    std::unique_ptr<nexus::NexusHost> _host;
    uint64_t _sessionToken;
    std::atomic<bool> _flushScheduled;
    uint64_t _deliveredSequence; // main queue only
    BOOL _watching;
    std::atomic<bool> _robloxRefreshScheduled;
}

- (instancetype)init {
    return [self initWithDataDirectory:nil runnerPath:nil];
}

- (instancetype)initWithDataDirectory:(NSString*)dataDirectory runnerPath:(NSString*)runnerPath {
    if ((self = [super init])) {
        nexus::HostOptions options;
        if (dataDirectory.length) options.dataDirectory = STD(dataDirectory);
        NSString* runner = runnerPath;
        if (!runner.length) {
            // Contents/MacOS/nexus-runner, next to the app executable.
            runner = [[NSBundle.mainBundle.executablePath stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"nexus-runner"];
        }
        options.runnerPath = STD(runner);
        _host = std::make_unique<nexus::NexusHost>(options);
        _flushScheduled = false;
        _robloxRefreshScheduled = false;
        _deliveredSequence = _host->sessions().consoleSequence();

        __weak NXHost* weakSelf = self;
        _sessionToken = _host->sessions().subscribe([weakSelf](const nexus::SessionEvent& e) {
            NXHost* strongSelf = weakSelf;
            if (!strongSelf) return;
            if (e.kind == nexus::SessionEventKind::Output) {
                [strongSelf scheduleConsoleFlush];
                return;
            }
            NXSessionSnapshot* snapshot = [NXSessionSnapshot nx_from:e.snapshot];
            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf flushConsole]; // deliver output produced before the state change first
                if (strongSelf.sessionHandler) strongSelf.sessionHandler(snapshot);
            });
        });
    }
    return self;
}

- (void)dealloc {
    [self shutdown];
}

- (void)shutdown {
    [self stopWatchingRobloxProcesses];
    if (_host) {
        _host->sessions().unsubscribe(_sessionToken);
        _host.reset(); // stops sessions, disconnects Studio, saves history
    }
}

// Coalesces console events: at most ~30 deliveries per second regardless of output volume.
- (void)scheduleConsoleFlush {
    if (_flushScheduled.exchange(true)) return;
    __weak NXHost* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 33 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        NXHost* s = weakSelf;
        if (!s) return;
        s->_flushScheduled = false;
        [s flushConsole];
    });
}

- (void)flushConsole {
    if (!_host) return;
    NSArray<NXConsoleLine*>* lines = [self consoleLinesAfter:_deliveredSequence limit:5000];
    if (lines.count == 0) return;
    _deliveredSequence = lines.lastObject.sequence;
    if (self.consoleHandler) self.consoleHandler(lines);
}

- (NSString*)nexusVersion { return @NEXUS_VERSION; }
- (NSString*)luauVersion { return NS(nexus::luauVersion()); }
- (NSString*)operatingSystem { return NS(nexus::platform::operatingSystemDescription()); }
- (NSString*)dataDirectory { return _host ? NS(_host->dataDirectory().string()) : @""; }
- (NSString*)runnerPath { return _host ? NS(_host->runnerPath()) : @""; }
- (NSString*)libraryRoot { return _host ? NS(_host->library().root().string()) : @""; }

// ---- Execution

- (BOOL)runSource:(NSString*)source name:(NSString*)name target:(NSString*)targetId error:(NSError**)error {
    if (!_host) return NO;
    auto r = _host->run(STD(source), STD(name), STD(targetId));
    return fail(r, error);
}

- (BOOL)stop { return _host && _host->sessions().stop(); }

- (BOOL)restartWithError:(NSError**)error {
    if (!_host) return NO;
    return fail(_host->sessions().restart(), error);
}

- (NXSessionSnapshot*)currentSession { return [NXSessionSnapshot nx_from:_host ? _host->sessions().current() : nexus::SessionSnapshot{}]; }

- (NSArray<NXConsoleLine*>*)consoleLinesAfter:(uint64_t)sequence limit:(NSUInteger)limit {
    NSMutableArray* out = [NSMutableArray array];
    if (!_host) return out;
    for (const auto& [seq, line] : _host->sessions().console(sequence, limit)) [out addObject:[NXConsoleLine nx_from:line sequence:seq]];
    return out;
}

- (void)clearConsole {
    if (_host) _host->sessions().clearConsole();
}

- (NSArray<NXExecutionRecord*>*)history {
    NSMutableArray* out = [NSMutableArray array];
    if (!_host) return out;
    auto records = _host->sessions().history();
    for (auto it = records.rbegin(); it != records.rend(); ++it) [out addObject:[NXExecutionRecord nx_from:*it]];
    return out;
}

- (void)clearHistory {
    if (!_host) return;
    _host->sessions().clearHistory();
    _host->saveHistory();
}

- (NSArray<NXTargetInfo*>*)targets {
    NSMutableArray* out = [NSMutableArray array];
    if (!_host) return out;
    for (const auto& t : _host->targets().list()) [out addObject:[NXTargetInfo nx_from:t->info() availability:t->availability()]];
    return out;
}

// ---- Language services

- (NSData*)highlightSource:(NSString*)source {
    std::string utf8 = STD(source);
    nexus::SourceMap map(utf8);
    auto tokens = nexus::editor::highlight(utf8);
    NSMutableData* data = [NSMutableData dataWithLength:tokens.size() * sizeof(NXTokenRange)];
    auto* out = static_cast<NXTokenRange*>(data.mutableBytes);
    for (size_t i = 0; i < tokens.size(); ++i) {
        size_t u0 = map.utf16Offset(tokens[i].offset);
        size_t u1 = map.utf16Offset(tokens[i].offset + tokens[i].length);
        out[i] = NXTokenRange{static_cast<uint32_t>(u0), static_cast<uint32_t>(u1 - u0), static_cast<uint32_t>(tokens[i].kind)};
    }
    return data;
}

- (NSArray<NXDiagnostic*>*)syntaxDiagnosticsForSource:(NSString*)source {
    std::string utf8 = STD(source);
    nexus::SourceMap map(utf8);
    NSMutableArray* out = [NSMutableArray array];
    for (const auto& d : nexus::checkSyntax(utf8)) [out addObject:[NXDiagnostic nx_from:d map:&map]];
    return out;
}

- (NSString*)formatSource:(NSString*)source error:(NSError**)error {
    nexus::editor::FormatOptions o;
    if (_host) {
        auto s = _host->settings();
        o.indentWidth = s.indentWidth;
        o.useTabs = s.useTabs;
    }
    auto r = nexus::editor::format(STD(source), o);
    if (!r.ok) {
        if (error) *error = makeError(nexus::Error("format", r.error));
        return nil;
    }
    return NS(r.text);
}

- (NXCompletionResult*)completionsForSource:(NSString*)source cursor:(NSUInteger)cursor {
    std::string utf8 = STD(source);
    size_t byteCursor = byteOffset(source, cursor);
    auto c = nexus::editor::complete(utf8, byteCursor, 100);
    nexus::SourceMap map(utf8);
    size_t start = map.utf16Offset(c.replaceOffset);
    size_t end = map.utf16Offset(byteCursor);
    NSMutableArray* items = [NSMutableArray array];
    for (const auto& item : c.items) [items addObject:[NXCompletionItem nx_from:item]];
    return [NXCompletionResult nx_range:NSMakeRange(start, end - start) items:items];
}

- (NSInteger)matchingBracketInSource:(NSString*)source location:(NSUInteger)location {
    std::string utf8 = STD(source);
    int64_t match = nexus::editor::matchingBracket(utf8, byteOffset(source, location));
    if (match < 0) return -1;
    nexus::SourceMap map(utf8);
    return static_cast<NSInteger>(map.utf16Offset(static_cast<size_t>(match)));
}

- (NXCompatReport*)analyzeSource:(NSString*)source {
    std::string utf8 = STD(source);
    nexus::SourceMap map(utf8);
    return [NXCompatReport nx_from:nexus::compat::analyzeCompatibility(utf8) map:map];
}

// ---- Roblox

- (NXRobloxStatus*)refreshRoblox { return [NXRobloxStatus nx_from:_host ? _host->refreshRoblox() : nexus::roblox::PlayerStatus{}]; }
- (NXRobloxStatus*)robloxStatus { return [NXRobloxStatus nx_from:_host ? _host->player().status() : nexus::roblox::PlayerStatus{}]; }
- (NXStudioStatus*)refreshStudio { return [NXStudioStatus nx_from:_host ? _host->refreshStudio() : nexus::studio::StudioStatus{}]; }
- (NXStudioStatus*)connectStudio { return [NXStudioStatus nx_from:_host ? _host->connectStudio() : nexus::studio::StudioStatus{}]; }
- (void)disconnectStudio {
    if (_host) _host->disconnectStudio();
}
- (NXStudioStatus*)studioStatus { return [NXStudioStatus nx_from:_host ? _host->studio().status() : nexus::studio::StudioStatus{}]; }
- (void)selectStudio:(NSString*)identifier {
    if (_host) _host->studio().selectStudio(STD(identifier));
}

- (NSString*)describeRemoteVersionResponse:(NSString*)body {
    auto remote = nexus::roblox::parseClientVersionResponse(STD(body));
    if (!remote.ok()) return NS("Could not interpret the response: " + remote.error().message);
    std::string text = "Latest Roblox Player for macOS (live channel): " + remote.value().version;
    if (_host) {
        auto st = _host->player().status();
        if (st.installed && !st.installed->version.empty()) {
            int cmp = nexus::roblox::compareVersions(st.installed->version, remote.value().version);
            text += "\nInstalled: " + st.installed->version + (cmp == 0 ? " (up to date)" : cmp < 0 ? " (older — Roblox updates itself on launch)" : " (newer than the live channel)");
        } else {
            text += "\nThe Roblox Player is not installed.";
        }
        _host->logger().info("roblox", "User-initiated version check: " + remote.value().version);
    }
    text += "\nLive script execution remains unsupported regardless of version.";
    return NS(text);
}

- (void)startWatchingRobloxProcesses {
    if (_watching) return;
    _watching = YES;
    [NSWorkspace.sharedWorkspace addObserver:self
                                  forKeyPath:@"runningApplications"
                                     options:NSKeyValueObservingOptionNew | NSKeyValueObservingOptionOld
                                     context:kRunningAppsContext];
}

- (void)stopWatchingRobloxProcesses {
    if (!_watching) return;
    _watching = NO;
    [NSWorkspace.sharedWorkspace removeObserver:self forKeyPath:@"runningApplications" context:kRunningAppsContext];
}

static BOOL containsRoblox(id apps) {
    if (![apps isKindOfClass:NSArray.class]) return NO;
    for (NSRunningApplication* app in (NSArray*)apps) {
        NSString* bid = app.bundleIdentifier.lowercaseString;
        if ([bid isEqualToString:@"com.roblox.robloxplayer"] || [bid isEqualToString:@"com.roblox.robloxstudio"]) return YES;
    }
    return NO;
}

- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context {
    if (context != kRunningAppsContext) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    if (!containsRoblox(change[NSKeyValueChangeNewKey]) && !containsRoblox(change[NSKeyValueChangeOldKey])) return;
    if (_robloxRefreshScheduled.exchange(true)) return;
    __weak NXHost* weakSelf = self;
    // Debounce: launches emit several notifications; wait briefly, then refresh off the main queue.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NXHost* s = weakSelf;
        if (!s) return;
        s->_robloxRefreshScheduled = false;
        NXRobloxStatus* player = [s refreshRoblox];
        NXStudioStatus* studio = [s refreshStudio];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (s.robloxHandler) s.robloxHandler(player, studio);
        });
    });
}

// ---- Library

- (NXLibraryItem*)libraryTree { return [NXLibraryItem nx_from:_host ? _host->library().tree() : nexus::LibraryEntry{}]; }

- (NSString*)readScript:(NSString*)path error:(NSError**)error {
    if (!_host) return nil;
    auto r = _host->library().read(STD(path));
    if (!fail(r, error)) return nil;
    return NS(r.value());
}

- (BOOL)writeScript:(NSString*)path content:(NSString*)content error:(NSError**)error {
    return _host && fail(_host->library().write(STD(path), STD(content)), error);
}

#define NX_STRING_RESULT(expr)                    \
    if (!_host) return nil;                       \
    auto r = (expr);                              \
    if (!fail(r, error)) return nil;              \
    return NS(r.value());

- (NSString*)createScriptInFolder:(NSString*)folder name:(NSString*)name content:(NSString*)content error:(NSError**)error {
    NX_STRING_RESULT(_host->library().createScript(STD(folder), STD(name), STD(content)))
}
- (NSString*)createFolderInFolder:(NSString*)parent name:(NSString*)name project:(BOOL)project error:(NSError**)error {
    NX_STRING_RESULT(_host->library().createFolder(STD(parent), STD(name), project))
}
- (NSString*)renameItem:(NSString*)path to:(NSString*)name error:(NSError**)error {
    NX_STRING_RESULT(_host->library().rename(STD(path), STD(name)))
}
- (NSString*)duplicateItem:(NSString*)path error:(NSError**)error {
    NX_STRING_RESULT(_host->library().duplicate(STD(path)))
}
- (NSString*)moveItem:(NSString*)path toFolder:(NSString*)folder error:(NSError**)error {
    NX_STRING_RESULT(_host->library().move(STD(path), STD(folder)))
}
- (NSString*)importFile:(NSString*)externalPath intoFolder:(NSString*)folder error:(NSError**)error {
    NX_STRING_RESULT(_host->library().importFile(STD(externalPath), STD(folder)))
}

- (BOOL)deleteItem:(NSString*)path error:(NSError**)error { return _host && fail(_host->library().remove(STD(path)), error); }
- (BOOL)setFavorite:(BOOL)favorite forItem:(NSString*)path error:(NSError**)error {
    return _host && fail(_host->library().setFavorite(STD(path), favorite), error);
}
- (BOOL)setProject:(BOOL)project forFolder:(NSString*)path error:(NSError**)error {
    return _host && fail(_host->library().setProject(STD(path), project), error);
}
- (void)markOpened:(NSString*)path {
    if (_host) _host->library().markOpened(STD(path));
}
- (NSArray<NSString*>*)recentScripts { return _host ? toArray(_host->library().recent()) : @[]; }
- (NSArray<NSString*>*)favoriteScripts { return _host ? toArray(_host->library().favorites()) : @[]; }
- (NSArray<NXSearchHit*>*)searchLibrary:(NSString*)query {
    NSMutableArray* out = [NSMutableArray array];
    if (!_host) return out;
    for (const auto& h : _host->library().search(STD(query))) [out addObject:[NXSearchHit nx_from:h]];
    return out;
}
- (BOOL)exportItem:(NSString*)path to:(NSString*)destination error:(NSError**)error {
    return _host && fail(_host->library().exportItem(STD(path), STD(destination)), error);
}

// ---- Drafts

- (NSArray<NXDraft*>*)drafts {
    NSMutableArray* out = [NSMutableArray array];
    if (!_host) return out;
    for (const auto& d : _host->drafts().list()) {
        NXDraft* x = [NXDraft new];
        x.draftId = NS(d.id);
        x.title = NS(d.title);
        x.content = NS(d.content);
        x.libraryPath = NS(d.libraryPath);
        x.savedAt = toDate(d.savedAt);
        [out addObject:x];
    }
    return out;
}

- (BOOL)saveDraft:(NXDraft*)draft error:(NSError**)error {
    if (!_host) return NO;
    nexus::Draft d;
    d.id = STD(draft.draftId);
    d.title = STD(draft.title);
    d.content = STD(draft.content);
    d.libraryPath = STD(draft.libraryPath);
    d.savedAt = std::chrono::system_clock::now();
    return fail(_host->drafts().save(d), error);
}

- (void)removeDraft:(NSString*)draftId {
    if (_host) (void)_host->drafts().remove(STD(draftId));
}

// ---- Settings

- (NXSettings*)settings { return toNX(_host ? _host->settings() : nexus::Settings{}); }

- (BOOL)updateSettings:(NXSettings*)settings error:(NSError**)error {
    return _host && fail(_host->updateSettings(fromNX(settings)), error);
}

// ---- Diagnostics

- (NSArray<NXLogEntry*>*)logEntriesAfter:(uint64_t)sequence {
    NSMutableArray* out = [NSMutableArray array];
    if (!_host) return out;
    for (const auto& e : _host->logger().entries(sequence, 2000)) [out addObject:[NXLogEntry nx_from:e]];
    return out;
}

- (NSString*)diagnosticsJSON { return _host ? NS(nexus::json::dump(_host->diagnosticsSnapshot(), 2)) : @"{}"; }

- (BOOL)exportDiagnosticsTo:(NSString*)path error:(NSError**)error {
    return _host && fail(_host->exportDiagnostics(STD(path)), error);
}

- (BOOL)exportLogsTo:(NSString*)path error:(NSError**)error {
    if (!_host) return NO;
    std::string p = STD(path);
    return fail(nexus::str::endsWith(p, ".json") ? _host->logger().exportJson(p) : _host->logger().exportText(p), error);
}

- (void)logInfo:(NSString*)message category:(NSString*)category {
    if (_host) _host->logger().info(STD(category), STD(message));
}

@end
