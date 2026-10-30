// Objective-C interface over the Nexus C++ core (NexusHost). Imported into Swift through the
// bridging header. All callbacks are delivered on the main queue. Text offsets exchanged with the
// UI are UTF-16 offsets (NSString indices); the bridge converts them to/from UTF-8 byte offsets.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, NXSessionState) {
    NXSessionStateIdle,
    NXSessionStateInitializing,
    NXSessionStateCompiling,
    NXSessionStateReady,
    NXSessionStateRunning,
    NXSessionStateStopping,
    NXSessionStateStopped,
    NXSessionStateFailed,
    NXSessionStateDisconnected,
};

typedef NS_ENUM(NSInteger, NXOutputStream) {
    NXOutputStreamOutput,
    NXOutputStreamWarning,
    NXOutputStreamError,
    NXOutputStreamInfo,
    NXOutputStreamSystem,
};

// Mirrors nexus::editor::TokenKind.
typedef NS_ENUM(uint32_t, NXTokenKind) {
    NXTokenKindPlain,
    NXTokenKindKeyword,
    NXTokenKindConstant,
    NXTokenKindNumber,
    NXTokenKindString,
    NXTokenKindComment,
    NXTokenKindOperator,
    NXTokenKindBuiltin,
    NXTokenKindRobloxGlobal,
    NXTokenKindExecutorGlobal,
    NXTokenKindFunction,
    NXTokenKindProperty,
    NXTokenKindAttribute,
    NXTokenKindError,
};

// Packed element of the NSData returned by -highlightSource:.
typedef struct {
    uint32_t location; // UTF-16
    uint32_t length;   // UTF-16
    uint32_t kind;     // NXTokenKind
} NXTokenRange;

@interface NXDiagnostic : NSObject
@property (nonatomic, readonly) NSString *severity; // error | warning | info | hint
@property (nonatomic, readonly) NSString *code;
@property (nonatomic, readonly) NSString *message;
@property (nonatomic, readonly) NSString *origin;
@property (nonatomic, readonly) NSInteger line;     // 1-based, 0 = no location
@property (nonatomic, readonly) NSInteger column;   // 1-based (bytes)
@property (nonatomic, readonly) NSRange range;      // UTF-16 range in the analysed source (location NSNotFound if unknown)
@end

@interface NXConsoleLine : NSObject
@property (nonatomic, readonly) uint64_t sequence;
@property (nonatomic, readonly) uint64_t sessionId;
@property (nonatomic, readonly) NXOutputStream stream;
@property (nonatomic, readonly) NSString *text;
@property (nonatomic, readonly) NSDate *time;
@property (nonatomic, readonly) double elapsedMs;
@end

@interface NXSessionSnapshot : NSObject
@property (nonatomic, readonly) uint64_t sessionId;
@property (nonatomic, readonly) NXSessionState state;
@property (nonatomic, readonly) NSString *stateName;
@property (nonatomic, readonly) NSString *scriptName;
@property (nonatomic, readonly) NSString *targetId;
@property (nonatomic, readonly) NSString *message;
@property (nonatomic, readonly, nullable) NSDate *startedAt;
@property (nonatomic, readonly) BOOL active;
@property (nonatomic, readonly) BOOL hasResult;
@property (nonatomic, readonly) NSString *resultStatus; // succeeded, runtime-error, ... (empty without result)
@property (nonatomic, readonly) NSString *resultSummary;
@property (nonatomic, readonly) NSInteger errorLine;
@property (nonatomic, readonly) NSString *errorMessage;
@property (nonatomic, readonly) NSString *traceback;
@property (nonatomic, readonly) double wallMs;
@property (nonatomic, readonly) double cpuMs;
@property (nonatomic, readonly) double compileMs;
@property (nonatomic, readonly) uint64_t peakMemoryBytes;
@property (nonatomic, readonly) uint64_t outputBytes;
@property (nonatomic, readonly) uint32_t threadsCreated;
@property (nonatomic, readonly) NSArray<NXDiagnostic *> *diagnostics;
@end

@interface NXExecutionRecord : NSObject
@property (nonatomic, readonly) uint64_t sessionId;
@property (nonatomic, readonly) NSString *scriptName;
@property (nonatomic, readonly) NSString *targetId;
@property (nonatomic, readonly) NSDate *startedAt;
@property (nonatomic, readonly) NSString *status;
@property (nonatomic, readonly) NSString *finalState;
@property (nonatomic, readonly) NSString *summary;
@property (nonatomic, readonly) double wallMs;
@property (nonatomic, readonly) double cpuMs;
@property (nonatomic, readonly) uint64_t peakMemoryBytes;
@property (nonatomic, readonly) uint64_t sourceBytes;
@end

@interface NXTargetInfo : NSObject
@property (nonatomic, readonly) NSString *identifier;
@property (nonatomic, readonly) NSString *displayName;
@property (nonatomic, readonly) NSString *detail;
@property (nonatomic, readonly) NSString *kind;
@property (nonatomic, readonly) BOOL available;
@property (nonatomic, readonly) NSString *availability;
@property (nonatomic, readonly) BOOL robloxEngineApis;
@property (nonatomic, readonly) BOOL guaranteedStop;
@property (nonatomic, readonly) BOOL processIsolation;
@property (nonatomic, readonly) BOOL resourceLimits;
@end

@interface NXCompletionItem : NSObject
@property (nonatomic, readonly) NSString *label;
@property (nonatomic, readonly) NSString *kind;
@property (nonatomic, readonly) NSString *detail;
@property (nonatomic, readonly) NSString *insertText; // label when no snippet text
@end

@interface NXCompletionResult : NSObject
@property (nonatomic, readonly) NSRange replaceRange; // UTF-16
@property (nonatomic, readonly) NSArray<NXCompletionItem *> *items;
@end

@interface NXCompatFinding : NSObject
@property (nonatomic, readonly) NSString *kind;
@property (nonatomic, readonly) NSString *severity;
@property (nonatomic, readonly) NSString *symbol;
@property (nonatomic, readonly) NSString *message;
@property (nonatomic, readonly) NSInteger line;
@property (nonatomic, readonly) NSInteger occurrences;
@property (nonatomic, readonly) NSString *standalone;
@property (nonatomic, readonly) NSString *studio;
@property (nonatomic, readonly) NSString *risk;
@end

@interface NXCompatVerdict : NSObject
@property (nonatomic, readonly) NSString *targetId;
@property (nonatomic, readonly) NSString *displayName;
@property (nonatomic, readonly) NSString *verdict; // available | partial | unknown | unavailable
@property (nonatomic, readonly) NSString *summary;
@property (nonatomic, readonly) NSArray<NSString *> *blockers;
@end

@interface NXCompatReport : NSObject
@property (nonatomic, readonly) BOOL syntaxOk;
@property (nonatomic, readonly) NSArray<NXDiagnostic *> *syntaxErrors;
@property (nonatomic, readonly) NSArray<NXCompatFinding *> *findings;
@property (nonatomic, readonly) NSArray<NXCompatVerdict *> *verdicts;
@property (nonatomic, readonly) NSArray<NSString *> *services;
@property (nonatomic, readonly) NSArray<NSString *> *globalsUsed;
@property (nonatomic, readonly) NSArray<NXDiagnostic *> *editorDiagnostics; // findings as diagnostics with ranges
@property (nonatomic, readonly) double analysisMs;
@property (nonatomic, readonly) NSString *text;
@property (nonatomic, readonly) NSString *json;
@end

@interface NXLibraryItem : NSObject
@property (nonatomic, readonly) NSString *path;
@property (nonatomic, readonly) NSString *name;
@property (nonatomic, readonly) BOOL isFolder;
@property (nonatomic, readonly) BOOL isProject;
@property (nonatomic, readonly) BOOL favorite;
@property (nonatomic, readonly) uint64_t sizeBytes;
@property (nonatomic, readonly) NSDate *modified;
@property (nonatomic, readonly, nullable) NSArray<NXLibraryItem *> *children; // nil for scripts
@end

@interface NXSearchHit : NSObject
@property (nonatomic, readonly) NSString *path;
@property (nonatomic, readonly) BOOL nameMatch;
@property (nonatomic, readonly) NSInteger line;
@property (nonatomic, readonly) NSString *preview;
@end

@interface NXDraft : NSObject
@property (nonatomic, copy) NSString *draftId;
@property (nonatomic, copy) NSString *title;
@property (nonatomic, copy) NSString *content;
@property (nonatomic, copy) NSString *libraryPath;
@property (nonatomic, strong, nullable) NSDate *savedAt;
@end

@interface NXRobloxStatus : NSObject
@property (nonatomic, readonly) NSString *state; // DISCONNECTED ... UNSUPPORTED
@property (nonatomic, readonly) NSString *summary;
@property (nonatomic, readonly) BOOL installed;
@property (nonatomic, readonly) NSString *bundlePath;
@property (nonatomic, readonly) NSString *version;
@property (nonatomic, readonly) NSString *build;
@property (nonatomic, readonly) NSString *minimumSystemVersion;
@property (nonatomic, readonly) NSArray<NSString *> *architectures;
@property (nonatomic, readonly) BOOL signatureChecked;
@property (nonatomic, readonly) BOOL signatureValid;
@property (nonatomic, readonly) NSString *signatureTeamId;
@property (nonatomic, readonly) NSString *signatureError;
@property (nonatomic, readonly) BOOL hardenedRuntime;
@property (nonatomic, readonly) NSArray<NSString *> *securityNotes;
@property (nonatomic, readonly) NSArray<NSNumber *> *runningPids;
@property (nonatomic, readonly) NSArray<NSString *> *runningDescriptions;
@property (nonatomic, readonly) NSArray<NSString *> *unsupportedReasons;
@property (nonatomic, readonly) NSArray<NSString *> *notes;
@property (nonatomic, readonly) NSString *previousVersion;
@property (nonatomic, readonly) uint64_t generation;
@property (nonatomic, readonly) NSArray<NSString *> *history;
@property (nonatomic, readonly, nullable) NSDate *checkedAt;
@property (nonatomic, readonly) NSString *json;
@end

@interface NXStudioInstance : NSObject
@property (nonatomic, readonly) NSString *identifier;
@property (nonatomic, readonly) NSString *name;
@property (nonatomic, readonly) NSString *placeId;
@end

@interface NXStudioStatus : NSObject
@property (nonatomic, readonly) NSString *state;
@property (nonatomic, readonly) NSString *message;
@property (nonatomic, readonly) BOOL installed;
@property (nonatomic, readonly) NSString *version;
@property (nonatomic, readonly) NSString *bundlePath;
@property (nonatomic, readonly) NSArray<NSNumber *> *runningPids;
@property (nonatomic, readonly) NSString *mcpPath;
@property (nonatomic, readonly) BOOL mcpSignatureValid;
@property (nonatomic, readonly) NSString *mcpSignatureError;
@property (nonatomic, readonly) NSString *serverName;
@property (nonatomic, readonly) NSString *serverVersion;
@property (nonatomic, readonly) NSString *protocolVersion;
@property (nonatomic, readonly) NSArray<NSString *> *tools;
@property (nonatomic, readonly) NSString *executeTool;
@property (nonatomic, readonly) NSArray<NXStudioInstance *> *studios;
@property (nonatomic, readonly) NSString *selectedStudioId;
@property (nonatomic, readonly) BOOL connected;
@property (nonatomic, readonly) NSArray<NSString *> *history;
@property (nonatomic, readonly) NSString *json;
@end

@interface NXSettings : NSObject <NSCopying>
@property (nonatomic, copy) NSString *appearance;      // system | light | dark
@property (nonatomic, copy) NSString *accentColor;     // #RRGGBB
@property (nonatomic) double editorFontSize;
@property (nonatomic, copy) NSString *editorFontName;
@property (nonatomic) NSInteger indentWidth;
@property (nonatomic) BOOL useTabs;
@property (nonatomic) BOOL autoCloseBrackets;
@property (nonatomic) BOOL autoIndent;
@property (nonatomic) BOOL highlightMatchingBrackets;
@property (nonatomic) BOOL liveDiagnostics;
@property (nonatomic) BOOL autocomplete;
@property (nonatomic) BOOL showMinimap;
@property (nonatomic) BOOL restoreDrafts;
@property (nonatomic, copy) NSString *defaultTarget;
@property (nonatomic) NSInteger timeoutMs;
@property (nonatomic) NSInteger memoryLimitMB;
@property (nonatomic) NSInteger maxOutputKB;
@property (nonatomic) BOOL runnerSandbox;
@property (nonatomic, copy) NSString *studioDataModel; // Edit | Server | Client
@property (nonatomic) BOOL allowRobloxVersionCheck;
@property (nonatomic) BOOL studioIntegrationEnabled;
@property (nonatomic) BOOL requireRobloxSignature;
@property (nonatomic) NSInteger consoleCapacity;
@end

@interface NXLogEntry : NSObject
@property (nonatomic, readonly) uint64_t sequence;
@property (nonatomic, readonly) NSDate *time;
@property (nonatomic, readonly) NSString *level;
@property (nonatomic, readonly) NSString *category;
@property (nonatomic, readonly) NSString *message;
@property (nonatomic, readonly) NSString *fields;
@end

@interface NXHost : NSObject

// dataDirectory nil = ~/Library/Application Support/Nexus; runnerPath nil = inside the app bundle.
- (instancetype)initWithDataDirectory:(nullable NSString *)dataDirectory runnerPath:(nullable NSString *)runnerPath NS_DESIGNATED_INITIALIZER NS_SWIFT_NAME(init(dataDirectory:runnerPath:));
- (instancetype)init;

@property (nonatomic, readonly) NSString *nexusVersion;
@property (nonatomic, readonly) NSString *luauVersion;
@property (nonatomic, readonly) NSString *operatingSystem;
@property (nonatomic, readonly) NSString *dataDirectory;
@property (nonatomic, readonly) NSString *runnerPath;
@property (nonatomic, readonly) NSString *libraryRoot;

// Main-queue callbacks.
@property (nonatomic, copy, nullable) void (^sessionHandler)(NXSessionSnapshot *snapshot);
@property (nonatomic, copy, nullable) void (^consoleHandler)(NSArray<NXConsoleLine *> *lines);
@property (nonatomic, copy, nullable) void (^robloxHandler)(NXRobloxStatus *player, NXStudioStatus *studio);

// Execution
- (BOOL)runSource:(NSString *)source name:(NSString *)name target:(nullable NSString *)targetId error:(NSError **)error NS_SWIFT_NAME(run(source:name:target:));
- (BOOL)stop NS_SWIFT_NAME(stop());
- (BOOL)restartWithError:(NSError **)error NS_SWIFT_NAME(restart());
- (NXSessionSnapshot *)currentSession NS_SWIFT_NAME(currentSession());
- (NSArray<NXConsoleLine *> *)consoleLinesAfter:(uint64_t)sequence limit:(NSUInteger)limit NS_SWIFT_NAME(consoleLines(after:limit:));
- (void)clearConsole NS_SWIFT_NAME(clearConsole());
- (NSArray<NXExecutionRecord *> *)history NS_SWIFT_NAME(executionHistory());
- (void)clearHistory NS_SWIFT_NAME(clearHistory());
- (NSArray<NXTargetInfo *> *)targets NS_SWIFT_NAME(executionTargets());

// Language services
- (NSData *)highlightSource:(NSString *)source NS_SWIFT_NAME(highlight(source:));
- (NSArray<NXDiagnostic *> *)syntaxDiagnosticsForSource:(NSString *)source NS_SWIFT_NAME(syntaxDiagnostics(source:));
- (nullable NSString *)formatSource:(NSString *)source error:(NSError **)error NS_SWIFT_NAME(format(source:));
- (NXCompletionResult *)completionsForSource:(NSString *)source cursor:(NSUInteger)cursor NS_SWIFT_NAME(completions(source:cursor:));
- (NSInteger)matchingBracketInSource:(NSString *)source location:(NSUInteger)location NS_SWIFT_NAME(matchingBracket(source:location:));
- (NXCompatReport *)analyzeSource:(NSString *)source NS_SWIFT_NAME(analyze(source:));

// Roblox
- (NXRobloxStatus *)refreshRoblox NS_SWIFT_NAME(refreshRoblox());
- (NXRobloxStatus *)robloxStatus NS_SWIFT_NAME(robloxStatus());
- (NXStudioStatus *)refreshStudio NS_SWIFT_NAME(refreshStudio());
- (NXStudioStatus *)connectStudio NS_SWIFT_NAME(connectStudio());
- (void)disconnectStudio NS_SWIFT_NAME(disconnectStudio());
- (NXStudioStatus *)studioStatus NS_SWIFT_NAME(studioStatus());
- (void)selectStudio:(NSString *)identifier NS_SWIFT_NAME(selectStudio(_:));
// Interprets the body of Roblox's public client-version endpoint and compares it with the
// installed Player version. The network request itself is made by the UI only after consent.
- (NSString *)describeRemoteVersionResponse:(NSString *)body NS_SWIFT_NAME(describeRemoteVersion(response:));
// Watches NSWorkspace for Roblox launches/exits (event driven, no polling).
- (void)startWatchingRobloxProcesses NS_SWIFT_NAME(startWatchingRobloxProcesses());
- (void)stopWatchingRobloxProcesses NS_SWIFT_NAME(stopWatchingRobloxProcesses());

// Library
- (NXLibraryItem *)libraryTree NS_SWIFT_NAME(libraryTree());
- (nullable NSString *)readScript:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(readScript(_:));
- (BOOL)writeScript:(NSString *)path content:(NSString *)content error:(NSError **)error NS_SWIFT_NAME(writeScript(_:content:));
- (nullable NSString *)createScriptInFolder:(NSString *)folder name:(NSString *)name content:(NSString *)content error:(NSError **)error NS_SWIFT_NAME(createScript(folder:name:content:));
- (nullable NSString *)createFolderInFolder:(NSString *)parent name:(NSString *)name project:(BOOL)project error:(NSError **)error NS_SWIFT_NAME(createFolder(parent:name:project:));
- (nullable NSString *)renameItem:(NSString *)path to:(NSString *)name error:(NSError **)error NS_SWIFT_NAME(renameItem(_:to:));
- (nullable NSString *)duplicateItem:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(duplicateItem(_:));
- (nullable NSString *)moveItem:(NSString *)path toFolder:(NSString *)folder error:(NSError **)error NS_SWIFT_NAME(moveItem(_:toFolder:));
- (BOOL)deleteItem:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(deleteItem(_:));
- (BOOL)setFavorite:(BOOL)favorite forItem:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(setFavorite(_:item:));
- (BOOL)setProject:(BOOL)project forFolder:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(setProject(_:folder:));
- (void)markOpened:(NSString *)path NS_SWIFT_NAME(markOpened(_:));
- (NSArray<NSString *> *)recentScripts NS_SWIFT_NAME(recentScripts());
- (NSArray<NSString *> *)favoriteScripts NS_SWIFT_NAME(favoriteScripts());
- (NSArray<NXSearchHit *> *)searchLibrary:(NSString *)query NS_SWIFT_NAME(searchLibrary(_:));
- (nullable NSString *)importFile:(NSString *)externalPath intoFolder:(NSString *)folder error:(NSError **)error NS_SWIFT_NAME(importFile(_:folder:));
- (BOOL)exportItem:(NSString *)path to:(NSString *)destination error:(NSError **)error NS_SWIFT_NAME(exportItem(_:to:));

// Drafts
- (NSArray<NXDraft *> *)drafts NS_SWIFT_NAME(savedDrafts());
- (BOOL)saveDraft:(NXDraft *)draft error:(NSError **)error NS_SWIFT_NAME(saveDraft(_:));
- (void)removeDraft:(NSString *)draftId NS_SWIFT_NAME(removeDraft(_:));

// Settings
- (NXSettings *)settings NS_SWIFT_NAME(currentSettings());
- (BOOL)updateSettings:(NXSettings *)settings error:(NSError **)error NS_SWIFT_NAME(updateSettings(_:));

// Diagnostics
- (NSArray<NXLogEntry *> *)logEntriesAfter:(uint64_t)sequence NS_SWIFT_NAME(logEntries(after:));
- (NSString *)diagnosticsJSON NS_SWIFT_NAME(diagnosticsJSON());
- (BOOL)exportDiagnosticsTo:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(exportDiagnostics(to:));
- (BOOL)exportLogsTo:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(exportLogs(to:));
- (void)logInfo:(NSString *)message category:(NSString *)category NS_SWIFT_NAME(logInfo(_:category:));

// Stops running scripts, disconnects Studio and persists state. Called on application exit.
- (void)shutdown NS_SWIFT_NAME(shutdown());

@end

NS_ASSUME_NONNULL_END
