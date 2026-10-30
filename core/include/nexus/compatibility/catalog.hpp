#pragma once
// Static knowledge base describing where globals and services come from. This is what lets Nexus
// say *why* a script cannot run somewhere instead of failing silently.
//
// Sources:
//   * Luau builtins: enumerated at runtime from a real Luau VM (see runtime/stdlib.hpp), so they
//     always match the linked Luau version; this catalog only adds descriptions.
//   * Roblox engine globals/datatypes/services: Roblox Creator Documentation (create.roblox.com).
//   * Executor APIs: names used by third-party Roblox executors (commonly the "UNC" naming
//     convention). They are listed only so that Nexus can *identify* them; Nexus does not provide
//     them in any target, and neither Roblox Studio nor the Roblox Player provides them.

#include <span>
#include <string>
#include <string_view>

namespace nexus::compat {

enum class Availability { Available, Partial, Unavailable, Unknown };
const char* toString(Availability a);

enum class SymbolKind {
    LuauBuiltin,     // part of the Luau standard library
    NexusExtension,  // provided by the Nexus standalone runtime (task, warn, loadstring)
    RobloxGlobal,    // Roblox engine global (game, workspace, script, require, ...)
    RobloxDatatype,  // Roblox datatype constructor (Vector3, CFrame, Color3, ...)
    RobloxDeprecated,// deprecated Roblox global (wait, spawn, delay, ...)
    ExecutorApi,     // third-party executor API (getgenv, hookfunction, ...) — never provided
};
const char* toString(SymbolKind k);

struct SymbolInfo {
    std::string_view name;
    SymbolKind kind;
    std::string_view description;
    Availability studio; // availability through the Roblox Studio integration
    std::string_view risk; // non-empty when the API is security/privacy sensitive
};

enum class ServiceContext { Any, ClientOnly, ServerOnly, StudioOnly, Restricted };
const char* toString(ServiceContext c);

struct ServiceInfo {
    std::string_view name;
    ServiceContext context;
    std::string_view description;
};

// Executor-provided methods on Roblox objects (e.g. game:HttpGet).
struct MemberInfo {
    std::string_view name;
    std::string_view description;
    std::string_view risk;
};

const SymbolInfo* lookupGlobal(std::string_view name);
const ServiceInfo* lookupService(std::string_view name);
const MemberInfo* lookupExecutorMember(std::string_view name);
// Executor extensions of the debug library (debug.getupvalue, ...).
const MemberInfo* lookupExecutorDebugMember(std::string_view name);

std::span<const SymbolInfo> allGlobals();
std::span<const ServiceInfo> allServices();

} // namespace nexus::compat
