#include "nexus/compatibility/catalog.hpp"

#include <array>

namespace nexus::compat {

const char* toString(Availability a) {
    switch (a) {
    case Availability::Available: return "available";
    case Availability::Partial: return "partial";
    case Availability::Unavailable: return "unavailable";
    case Availability::Unknown: return "unknown";
    }
    return "unknown";
}

const char* toString(SymbolKind k) {
    switch (k) {
    case SymbolKind::LuauBuiltin: return "luau-builtin";
    case SymbolKind::NexusExtension: return "nexus-extension";
    case SymbolKind::RobloxGlobal: return "roblox-global";
    case SymbolKind::RobloxDatatype: return "roblox-datatype";
    case SymbolKind::RobloxDeprecated: return "roblox-deprecated";
    case SymbolKind::ExecutorApi: return "executor-api";
    }
    return "unknown";
}

const char* toString(ServiceContext c) {
    switch (c) {
    case ServiceContext::Any: return "any";
    case ServiceContext::ClientOnly: return "client-only";
    case ServiceContext::ServerOnly: return "server-only";
    case ServiceContext::StudioOnly: return "studio-only";
    case ServiceContext::Restricted: return "restricted";
    }
    return "any";
}

namespace {

using K = SymbolKind;
using A = Availability;

constexpr std::string_view kFsRisk = "reads or writes files on the user's computer";
constexpr std::string_view kNetRisk = "performs network requests";
constexpr std::string_view kHookRisk = "modifies functions or metatables of the running game client";
constexpr std::string_view kInputRisk = "synthesizes mouse or keyboard input";

constexpr std::array kGlobals = std::to_array<SymbolInfo>({
    // Luau standard library (descriptions only; availability is computed from a real VM).
    {"assert", K::LuauBuiltin, "Raises an error if the value is false or nil", A::Available, ""},
    {"error", K::LuauBuiltin, "Raises an error", A::Available, ""},
    {"print", K::LuauBuiltin, "Writes values to the output", A::Available, ""},
    {"pcall", K::LuauBuiltin, "Calls a function in protected mode", A::Available, ""},
    {"xpcall", K::LuauBuiltin, "Calls a function in protected mode with a handler", A::Available, ""},
    {"pairs", K::LuauBuiltin, "Iterates over a table", A::Available, ""},
    {"ipairs", K::LuauBuiltin, "Iterates over the array part of a table", A::Available, ""},
    {"next", K::LuauBuiltin, "Returns the next key/value of a table", A::Available, ""},
    {"select", K::LuauBuiltin, "Selects arguments from a vararg list", A::Available, ""},
    {"type", K::LuauBuiltin, "Returns the basic type of a value", A::Available, ""},
    {"typeof", K::LuauBuiltin, "Returns the type of a value, including userdata type names", A::Available, ""},
    {"tostring", K::LuauBuiltin, "Converts a value to a string", A::Available, ""},
    {"tonumber", K::LuauBuiltin, "Converts a value to a number", A::Available, ""},
    {"rawget", K::LuauBuiltin, "Reads a table field without metamethods", A::Available, ""},
    {"rawset", K::LuauBuiltin, "Writes a table field without metamethods", A::Available, ""},
    {"rawequal", K::LuauBuiltin, "Compares without metamethods", A::Available, ""},
    {"rawlen", K::LuauBuiltin, "Length without metamethods", A::Available, ""},
    {"setmetatable", K::LuauBuiltin, "Sets a table's metatable", A::Available, ""},
    {"getmetatable", K::LuauBuiltin, "Gets a value's metatable", A::Available, ""},
    {"unpack", K::LuauBuiltin, "Returns table elements as values", A::Available, ""},
    {"getfenv", K::LuauBuiltin, "Gets a function environment (deoptimizes the script)", A::Available, ""},
    {"setfenv", K::LuauBuiltin, "Sets a function environment (deoptimizes the script)", A::Available, ""},
    {"newproxy", K::LuauBuiltin, "Creates an empty userdata", A::Available, ""},
    {"gcinfo", K::LuauBuiltin, "Returns heap size in KB", A::Available, ""},
    {"collectgarbage", K::LuauBuiltin, "Garbage collector control (collect/count)", A::Available, ""},
    {"_G", K::LuauBuiltin, "Global table", A::Available, ""},
    {"_VERSION", K::LuauBuiltin, "Language version string", A::Available, ""},
    {"math", K::LuauBuiltin, "Math library", A::Available, ""},
    {"string", K::LuauBuiltin, "String library", A::Available, ""},
    {"table", K::LuauBuiltin, "Table library", A::Available, ""},
    {"coroutine", K::LuauBuiltin, "Coroutine library", A::Available, ""},
    {"os", K::LuauBuiltin, "Time functions (clock, date, time, difftime)", A::Available, ""},
    {"debug", K::LuauBuiltin, "Debug library (info, traceback)", A::Available, ""},
    {"utf8", K::LuauBuiltin, "UTF-8 library", A::Available, ""},
    {"bit32", K::LuauBuiltin, "Bitwise operations", A::Available, ""},
    {"buffer", K::LuauBuiltin, "Fixed-size byte buffers", A::Available, ""},
    {"vector", K::LuauBuiltin, "Native 3-component vector type", A::Available, ""},

    // Nexus standalone runtime extensions (real implementations, not stubs).
    {"task", K::NexusExtension, "Task scheduler (spawn, defer, delay, wait, cancel); Nexus implements the Roblox semantics in standalone mode", A::Available, ""},
    {"warn", K::NexusExtension, "Writes a warning to the output", A::Available, ""},
    {"loadstring", K::NexusExtension, "Compiles Luau source at runtime; disabled by default in Roblox (ServerScriptService.LoadStringEnabled)", A::Partial, "executes dynamically constructed code"},

    // Roblox engine globals.
    {"game", K::RobloxGlobal, "The DataModel (root of the Roblox instance tree)", A::Available, ""},
    {"Game", K::RobloxDeprecated, "Deprecated alias of game", A::Available, ""},
    {"workspace", K::RobloxGlobal, "The Workspace service", A::Available, ""},
    {"Workspace", K::RobloxDeprecated, "Deprecated alias of workspace", A::Available, ""},
    {"script", K::RobloxGlobal, "The Script instance running the code; may be nil for code run outside a Script", A::Partial, ""},
    {"plugin", K::RobloxGlobal, "The Plugin object; only exists inside Studio plugins", A::Unavailable, ""},
    {"shared", K::RobloxGlobal, "Table shared between scripts of the same context", A::Available, ""},
    {"require", K::RobloxGlobal, "Loads a ModuleScript (or a module by asset id)", A::Available, ""},
    {"Instance", K::RobloxDatatype, "Instance constructor (Instance.new)", A::Available, ""},
    {"Enum", K::RobloxGlobal, "Roblox enumerations", A::Available, ""},
    {"settings", K::RobloxGlobal, "Global engine settings", A::Partial, ""},
    {"UserSettings", K::RobloxGlobal, "User settings (client)", A::Partial, ""},
    {"tick", K::RobloxDeprecated, "Local Unix time; prefer os.time/os.clock", A::Available, ""},
    {"time", K::RobloxGlobal, "Time since the game started running", A::Available, ""},
    {"elapsedTime", K::RobloxDeprecated, "Time since Roblox started", A::Available, ""},
    {"wait", K::RobloxDeprecated, "Yields; deprecated in favour of task.wait", A::Available, ""},
    {"delay", K::RobloxDeprecated, "Deprecated; use task.delay", A::Available, ""},
    {"spawn", K::RobloxDeprecated, "Deprecated; use task.spawn/task.defer", A::Available, ""},
    {"ypcall", K::RobloxDeprecated, "Deprecated alias of pcall", A::Available, ""},
    {"version", K::RobloxGlobal, "Roblox engine version string", A::Available, ""},
    {"printidentity", K::RobloxGlobal, "Prints the current thread identity", A::Available, ""},
    {"stats", K::RobloxGlobal, "Returns the Stats service", A::Available, ""},
    {"Stats", K::RobloxDeprecated, "Deprecated Stats accessor", A::Available, ""},
    {"PluginManager", K::RobloxDeprecated, "Deprecated plugin API", A::Unavailable, ""},
    {"DebuggerManager", K::RobloxGlobal, "Script debugger manager (Studio)", A::Partial, ""},
    {"LoadLibrary", K::RobloxDeprecated, "Removed Roblox API", A::Unavailable, ""},
    {"Vector3", K::RobloxDatatype, "3D vector", A::Available, ""},
    {"Vector2", K::RobloxDatatype, "2D vector", A::Available, ""},
    {"Vector3int16", K::RobloxDatatype, "16-bit integer 3D vector", A::Available, ""},
    {"Vector2int16", K::RobloxDatatype, "16-bit integer 2D vector", A::Available, ""},
    {"CFrame", K::RobloxDatatype, "Coordinate frame (position + rotation)", A::Available, ""},
    {"Color3", K::RobloxDatatype, "RGB color", A::Available, ""},
    {"BrickColor", K::RobloxDatatype, "Legacy palette color", A::Available, ""},
    {"ColorSequence", K::RobloxDatatype, "Color gradient", A::Available, ""},
    {"ColorSequenceKeypoint", K::RobloxDatatype, "Color gradient keypoint", A::Available, ""},
    {"NumberSequence", K::RobloxDatatype, "Number curve", A::Available, ""},
    {"NumberSequenceKeypoint", K::RobloxDatatype, "Number curve keypoint", A::Available, ""},
    {"NumberRange", K::RobloxDatatype, "Number range", A::Available, ""},
    {"UDim", K::RobloxDatatype, "GUI dimension", A::Available, ""},
    {"UDim2", K::RobloxDatatype, "2D GUI dimension", A::Available, ""},
    {"Rect", K::RobloxDatatype, "2D rectangle", A::Available, ""},
    {"Region3", K::RobloxDatatype, "3D region", A::Available, ""},
    {"Region3int16", K::RobloxDatatype, "Integer 3D region", A::Available, ""},
    {"Ray", K::RobloxDatatype, "Ray", A::Available, ""},
    {"RaycastParams", K::RobloxDatatype, "Raycast parameters", A::Available, ""},
    {"OverlapParams", K::RobloxDatatype, "Spatial query parameters", A::Available, ""},
    {"TweenInfo", K::RobloxDatatype, "Tween configuration", A::Available, ""},
    {"PhysicalProperties", K::RobloxDatatype, "Physical material properties", A::Available, ""},
    {"Random", K::RobloxDatatype, "Pseudo-random generator", A::Available, ""},
    {"DateTime", K::RobloxDatatype, "Date and time", A::Available, ""},
    {"Faces", K::RobloxDatatype, "Set of faces", A::Available, ""},
    {"Axes", K::RobloxDatatype, "Set of axes", A::Available, ""},
    {"PathWaypoint", K::RobloxDatatype, "Pathfinding waypoint", A::Available, ""},
    {"Font", K::RobloxDatatype, "Font descriptor", A::Available, ""},
    {"SharedTable", K::RobloxDatatype, "Table shareable across Actors", A::Available, ""},
    {"CatalogSearchParams", K::RobloxDatatype, "Avatar catalog query", A::Available, ""},
    {"DockWidgetPluginGuiInfo", K::RobloxDatatype, "Plugin widget configuration", A::Partial, ""},
    {"FloatCurveKey", K::RobloxDatatype, "Float curve key", A::Available, ""},
    {"RotationCurveKey", K::RobloxDatatype, "Rotation curve key", A::Available, ""},
    {"Content", K::RobloxDatatype, "Content reference", A::Available, ""},

    // Third-party executor APIs (identified, never provided).
    {"getgenv", K::ExecutorApi, "Executor global environment", A::Unavailable, ""},
    {"getrenv", K::ExecutorApi, "Game script environment of the client", A::Unavailable, kHookRisk},
    {"getreg", K::ExecutorApi, "Luau registry of the client", A::Unavailable, kHookRisk},
    {"getgc", K::ExecutorApi, "Enumerates garbage-collected objects of the client", A::Unavailable, kHookRisk},
    {"getinstances", K::ExecutorApi, "Enumerates all instances", A::Unavailable, ""},
    {"getnilinstances", K::ExecutorApi, "Enumerates parentless instances", A::Unavailable, ""},
    {"getscripts", K::ExecutorApi, "Enumerates scripts", A::Unavailable, ""},
    {"getrunningscripts", K::ExecutorApi, "Enumerates running scripts", A::Unavailable, ""},
    {"getloadedmodules", K::ExecutorApi, "Enumerates loaded modules", A::Unavailable, ""},
    {"getsenv", K::ExecutorApi, "Environment of another script", A::Unavailable, kHookRisk},
    {"getcallingscript", K::ExecutorApi, "Script that called the current function", A::Unavailable, ""},
    {"getscriptclosure", K::ExecutorApi, "Closure of a script", A::Unavailable, kHookRisk},
    {"getscripthash", K::ExecutorApi, "Hash of a script's bytecode", A::Unavailable, ""},
    {"getscriptbytecode", K::ExecutorApi, "Bytecode of a game script", A::Unavailable, kHookRisk},
    {"decompile", K::ExecutorApi, "Decompiles game scripts", A::Unavailable, kHookRisk},
    {"saveinstance", K::ExecutorApi, "Dumps the game to a file", A::Unavailable, kFsRisk},
    {"hookfunction", K::ExecutorApi, "Replaces a function in the client", A::Unavailable, kHookRisk},
    {"hookfunc", K::ExecutorApi, "Alias of hookfunction", A::Unavailable, kHookRisk},
    {"replaceclosure", K::ExecutorApi, "Alias of hookfunction", A::Unavailable, kHookRisk},
    {"hookmetamethod", K::ExecutorApi, "Hooks a metamethod of a game object", A::Unavailable, kHookRisk},
    {"newcclosure", K::ExecutorApi, "Wraps a function as a C closure", A::Unavailable, kHookRisk},
    {"newlclosure", K::ExecutorApi, "Wraps a function as a Luau closure", A::Unavailable, ""},
    {"clonefunction", K::ExecutorApi, "Copies a function", A::Unavailable, ""},
    {"iscclosure", K::ExecutorApi, "Tests for C closures", A::Unavailable, ""},
    {"islclosure", K::ExecutorApi, "Tests for Luau closures", A::Unavailable, ""},
    {"isexecutorclosure", K::ExecutorApi, "Tests whether a function belongs to the executor", A::Unavailable, ""},
    {"checkcaller", K::ExecutorApi, "Tests whether the caller is the executor", A::Unavailable, ""},
    {"getrawmetatable", K::ExecutorApi, "Bypasses __metatable protection", A::Unavailable, kHookRisk},
    {"setrawmetatable", K::ExecutorApi, "Bypasses __metatable protection", A::Unavailable, kHookRisk},
    {"setreadonly", K::ExecutorApi, "Changes table read-only state", A::Unavailable, kHookRisk},
    {"isreadonly", K::ExecutorApi, "Queries table read-only state", A::Unavailable, ""},
    {"make_writeable", K::ExecutorApi, "Makes a protected table writable", A::Unavailable, kHookRisk},
    {"make_readonly", K::ExecutorApi, "Makes a table read-only", A::Unavailable, ""},
    {"getnamecallmethod", K::ExecutorApi, "Current __namecall method", A::Unavailable, kHookRisk},
    {"setnamecallmethod", K::ExecutorApi, "Overrides the __namecall method", A::Unavailable, kHookRisk},
    {"getconnections", K::ExecutorApi, "Enumerates signal connections", A::Unavailable, kHookRisk},
    {"firesignal", K::ExecutorApi, "Fires a signal artificially", A::Unavailable, kHookRisk},
    {"fireclickdetector", K::ExecutorApi, "Simulates a ClickDetector click", A::Unavailable, kInputRisk},
    {"firetouchinterest", K::ExecutorApi, "Simulates part touches", A::Unavailable, kInputRisk},
    {"fireproximityprompt", K::ExecutorApi, "Triggers a ProximityPrompt", A::Unavailable, kInputRisk},
    {"gethiddenproperty", K::ExecutorApi, "Reads non-scriptable properties", A::Unavailable, ""},
    {"sethiddenproperty", K::ExecutorApi, "Writes non-scriptable properties", A::Unavailable, kHookRisk},
    {"setsimulationradius", K::ExecutorApi, "Changes physics ownership radius", A::Unavailable, kHookRisk},
    {"gethui", K::ExecutorApi, "Hidden UI container", A::Unavailable, ""},
    {"cloneref", K::ExecutorApi, "Clones an instance reference", A::Unavailable, ""},
    {"compareinstances", K::ExecutorApi, "Compares instance references", A::Unavailable, ""},
    {"getcustomasset", K::ExecutorApi, "Loads a local file as an asset", A::Unavailable, kFsRisk},
    {"getsynasset", K::ExecutorApi, "Loads a local file as an asset", A::Unavailable, kFsRisk},
    {"request", K::ExecutorApi, "HTTP request from the client", A::Unavailable, kNetRisk},
    {"http_request", K::ExecutorApi, "HTTP request from the client", A::Unavailable, kNetRisk},
    {"http", K::ExecutorApi, "Executor HTTP library", A::Unavailable, kNetRisk},
    {"syn", K::ExecutorApi, "Synapse X library", A::Unavailable, kNetRisk},
    {"fluxus", K::ExecutorApi, "Fluxus library", A::Unavailable, ""},
    {"krnl", K::ExecutorApi, "KRNL library", A::Unavailable, ""},
    {"Drawing", K::ExecutorApi, "Screen overlay drawing library", A::Unavailable, ""},
    {"cleardrawcache", K::ExecutorApi, "Clears Drawing objects", A::Unavailable, ""},
    {"isrenderobj", K::ExecutorApi, "Drawing object test", A::Unavailable, ""},
    {"getrenderproperty", K::ExecutorApi, "Drawing property read", A::Unavailable, ""},
    {"setrenderproperty", K::ExecutorApi, "Drawing property write", A::Unavailable, ""},
    {"crypt", K::ExecutorApi, "Executor cryptography library", A::Unavailable, ""},
    {"base64_encode", K::ExecutorApi, "Base64 helper", A::Unavailable, ""},
    {"base64_decode", K::ExecutorApi, "Base64 helper", A::Unavailable, ""},
    {"lz4compress", K::ExecutorApi, "LZ4 helper", A::Unavailable, ""},
    {"lz4decompress", K::ExecutorApi, "LZ4 helper", A::Unavailable, ""},
    {"writefile", K::ExecutorApi, "Writes a file", A::Unavailable, kFsRisk},
    {"readfile", K::ExecutorApi, "Reads a file", A::Unavailable, kFsRisk},
    {"appendfile", K::ExecutorApi, "Appends to a file", A::Unavailable, kFsRisk},
    {"listfiles", K::ExecutorApi, "Lists files", A::Unavailable, kFsRisk},
    {"isfile", K::ExecutorApi, "Tests for a file", A::Unavailable, kFsRisk},
    {"isfolder", K::ExecutorApi, "Tests for a folder", A::Unavailable, kFsRisk},
    {"makefolder", K::ExecutorApi, "Creates a folder", A::Unavailable, kFsRisk},
    {"delfolder", K::ExecutorApi, "Deletes a folder", A::Unavailable, kFsRisk},
    {"delfile", K::ExecutorApi, "Deletes a file", A::Unavailable, kFsRisk},
    {"loadfile", K::ExecutorApi, "Loads code from a file", A::Unavailable, kFsRisk},
    {"dofile", K::ExecutorApi, "Runs code from a file", A::Unavailable, kFsRisk},
    {"setclipboard", K::ExecutorApi, "Writes the system clipboard", A::Unavailable, "writes the system clipboard"},
    {"toclipboard", K::ExecutorApi, "Writes the system clipboard", A::Unavailable, "writes the system clipboard"},
    {"setfpscap", K::ExecutorApi, "Changes the client frame-rate cap", A::Unavailable, ""},
    {"getfpscap", K::ExecutorApi, "Reads the client frame-rate cap", A::Unavailable, ""},
    {"identifyexecutor", K::ExecutorApi, "Returns the executor name", A::Unavailable, ""},
    {"getexecutorname", K::ExecutorApi, "Returns the executor name", A::Unavailable, ""},
    {"queue_on_teleport", K::ExecutorApi, "Runs code after a teleport", A::Unavailable, "persists code across game sessions"},
    {"queueonteleport", K::ExecutorApi, "Runs code after a teleport", A::Unavailable, "persists code across game sessions"},
    {"mouse1click", K::ExecutorApi, "Synthesized mouse click", A::Unavailable, kInputRisk},
    {"mouse1press", K::ExecutorApi, "Synthesized mouse press", A::Unavailable, kInputRisk},
    {"mouse1release", K::ExecutorApi, "Synthesized mouse release", A::Unavailable, kInputRisk},
    {"mouse2click", K::ExecutorApi, "Synthesized mouse click", A::Unavailable, kInputRisk},
    {"mouse2press", K::ExecutorApi, "Synthesized mouse press", A::Unavailable, kInputRisk},
    {"mouse2release", K::ExecutorApi, "Synthesized mouse release", A::Unavailable, kInputRisk},
    {"mousemoveabs", K::ExecutorApi, "Synthesized mouse move", A::Unavailable, kInputRisk},
    {"mousemoverel", K::ExecutorApi, "Synthesized mouse move", A::Unavailable, kInputRisk},
    {"mousescroll", K::ExecutorApi, "Synthesized scroll", A::Unavailable, kInputRisk},
    {"keypress", K::ExecutorApi, "Synthesized key press", A::Unavailable, kInputRisk},
    {"keyrelease", K::ExecutorApi, "Synthesized key release", A::Unavailable, kInputRisk},
    {"isrbxactive", K::ExecutorApi, "Tests whether the client window is focused", A::Unavailable, ""},
    {"isgameactive", K::ExecutorApi, "Tests whether the client window is focused", A::Unavailable, ""},
    {"getthreadidentity", K::ExecutorApi, "Reads the thread security identity", A::Unavailable, kHookRisk},
    {"setthreadidentity", K::ExecutorApi, "Changes the thread security identity", A::Unavailable, kHookRisk},
    {"getidentity", K::ExecutorApi, "Reads the thread security identity", A::Unavailable, kHookRisk},
    {"setidentity", K::ExecutorApi, "Changes the thread security identity", A::Unavailable, kHookRisk},
    {"getthreadcontext", K::ExecutorApi, "Reads the thread security identity", A::Unavailable, kHookRisk},
    {"setthreadcontext", K::ExecutorApi, "Changes the thread security identity", A::Unavailable, kHookRisk},
    {"WebSocket", K::ExecutorApi, "WebSocket client", A::Unavailable, kNetRisk},
    {"messagebox", K::ExecutorApi, "Native message box", A::Unavailable, ""},
    {"rconsoleprint", K::ExecutorApi, "External console output", A::Unavailable, ""},
    {"rconsolename", K::ExecutorApi, "External console title", A::Unavailable, ""},
    {"rconsoleclear", K::ExecutorApi, "External console clear", A::Unavailable, ""},
    {"rconsoleinput", K::ExecutorApi, "External console input", A::Unavailable, ""},
    {"getobjects", K::ExecutorApi, "Loads assets by id", A::Unavailable, kNetRisk},
    {"gethwid", K::ExecutorApi, "Hardware identifier", A::Unavailable, "fingerprints the user's machine"},
});

constexpr std::array kServices = std::to_array<ServiceInfo>({
    {"Players", ServiceContext::Any, "Connected players; LocalPlayer exists only on the client"},
    {"Workspace", ServiceContext::Any, "3D world"},
    {"ReplicatedStorage", ServiceContext::Any, "Storage replicated to clients"},
    {"ReplicatedFirst", ServiceContext::Any, "Content replicated first to clients"},
    {"ServerStorage", ServiceContext::ServerOnly, "Server-only storage"},
    {"ServerScriptService", ServiceContext::ServerOnly, "Server scripts"},
    {"StarterGui", ServiceContext::Any, "GUI templates"},
    {"StarterPack", ServiceContext::Any, "Tool templates"},
    {"StarterPlayer", ServiceContext::Any, "Player templates"},
    {"Lighting", ServiceContext::Any, "Lighting settings"},
    {"SoundService", ServiceContext::Any, "Audio settings"},
    {"Teams", ServiceContext::Any, "Teams"},
    {"Chat", ServiceContext::Any, "Legacy chat"},
    {"TextChatService", ServiceContext::Any, "Text chat"},
    {"RunService", ServiceContext::Any, "Frame events; RenderStepped is client-only"},
    {"UserInputService", ServiceContext::ClientOnly, "Keyboard, mouse, touch and gamepad input"},
    {"ContextActionService", ServiceContext::ClientOnly, "Input action binding"},
    {"GuiService", ServiceContext::ClientOnly, "GUI state"},
    {"HapticService", ServiceContext::ClientOnly, "Haptics"},
    {"VRService", ServiceContext::ClientOnly, "VR input"},
    {"TweenService", ServiceContext::Any, "Property animation"},
    {"HttpService", ServiceContext::Any, "JSON helpers; HTTP requests are server-only and require HttpEnabled"},
    {"DataStoreService", ServiceContext::ServerOnly, "Persistent storage"},
    {"MessagingService", ServiceContext::ServerOnly, "Cross-server messaging"},
    {"MemoryStoreService", ServiceContext::ServerOnly, "Shared memory store"},
    {"MarketplaceService", ServiceContext::Any, "Purchases and product info"},
    {"TeleportService", ServiceContext::Any, "Teleports between places"},
    {"PathfindingService", ServiceContext::Any, "Path computation"},
    {"PhysicsService", ServiceContext::Any, "Collision groups"},
    {"CollectionService", ServiceContext::Any, "Instance tags"},
    {"Debris", ServiceContext::Any, "Timed instance removal"},
    {"BadgeService", ServiceContext::Any, "Badges"},
    {"GroupService", ServiceContext::Any, "Group info"},
    {"LocalizationService", ServiceContext::Any, "Localization"},
    {"PolicyService", ServiceContext::Any, "Regional policy"},
    {"SocialService", ServiceContext::Any, "Social prompts"},
    {"TextService", ServiceContext::Any, "Text measurement and filtering"},
    {"AssetService", ServiceContext::Any, "Assets"},
    {"InsertService", ServiceContext::Any, "Asset insertion"},
    {"ContentProvider", ServiceContext::Any, "Asset preloading"},
    {"AnalyticsService", ServiceContext::Any, "Analytics events"},
    {"LogService", ServiceContext::Any, "Output log"},
    {"ProximityPromptService", ServiceContext::Any, "Proximity prompts"},
    {"VoiceChatService", ServiceContext::Any, "Voice chat"},
    {"AvatarEditorService", ServiceContext::ClientOnly, "Avatar editing prompts"},
    {"StarterPlayerScripts", ServiceContext::Any, "Player script templates"},
    {"ScriptContext", ServiceContext::Any, "Script errors"},
    {"Stats", ServiceContext::Any, "Performance stats"},
    {"KeyframeSequenceProvider", ServiceContext::Any, "Animation assets"},
    {"Selection", ServiceContext::StudioOnly, "Studio selection"},
    {"ChangeHistoryService", ServiceContext::StudioOnly, "Studio undo history"},
    {"StudioService", ServiceContext::StudioOnly, "Studio state"},
    {"ScriptEditorService", ServiceContext::StudioOnly, "Studio script editor"},
    {"CoreGui", ServiceContext::Restricted, "Roblox core UI; not accessible to game scripts"},
    {"CorePackages", ServiceContext::Restricted, "Roblox core packages; not accessible to game scripts"},
    {"RobloxReplicatedStorage", ServiceContext::Restricted, "Internal Roblox storage"},
});

constexpr std::array kExecutorMembers = std::to_array<MemberInfo>({
    {"HttpGet", "Executor-provided DataModel method that downloads a URL", kNetRisk},
    {"HttpGetAsync", "Executor-provided DataModel method that downloads a URL", kNetRisk},
    {"HttpPost", "Executor-provided DataModel method that posts to a URL", kNetRisk},
    {"HttpPostAsync", "Executor-provided DataModel method that posts to a URL", kNetRisk},
    {"GetObjects", "Executor-enabled asset loader", kNetRisk},
});

constexpr std::array kExecutorDebug = std::to_array<MemberInfo>({
    {"getupvalue", "Reads an upvalue of another function", kHookRisk},
    {"getupvalues", "Reads upvalues of another function", kHookRisk},
    {"setupvalue", "Writes an upvalue of another function", kHookRisk},
    {"getconstant", "Reads a function constant", kHookRisk},
    {"getconstants", "Reads function constants", kHookRisk},
    {"setconstant", "Writes a function constant", kHookRisk},
    {"getproto", "Reads a nested function prototype", kHookRisk},
    {"getprotos", "Reads nested function prototypes", kHookRisk},
    {"getstack", "Reads a stack frame", kHookRisk},
    {"setstack", "Writes a stack frame", kHookRisk},
    {"getregistry", "Reads the Luau registry", kHookRisk},
    {"setmetatable", "Sets a metatable bypassing protection", kHookRisk},
    {"getmetatable", "Gets a metatable bypassing protection", kHookRisk},
    {"getinfo", "Executor debug.getinfo", ""},
});

template <class Array>
auto findByName(const Array& arr, std::string_view name) -> const typename Array::value_type* {
    for (const auto& item : arr) {
        if (item.name == name) return &item;
    }
    return nullptr;
}

} // namespace

const SymbolInfo* lookupGlobal(std::string_view name) { return findByName(kGlobals, name); }
const ServiceInfo* lookupService(std::string_view name) { return findByName(kServices, name); }
const MemberInfo* lookupExecutorMember(std::string_view name) { return findByName(kExecutorMembers, name); }
const MemberInfo* lookupExecutorDebugMember(std::string_view name) { return findByName(kExecutorDebug, name); }
std::span<const SymbolInfo> allGlobals() { return kGlobals; }
std::span<const ServiceInfo> allServices() { return kServices; }

} // namespace nexus::compat
