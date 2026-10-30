#pragma once
// Standalone Luau runtime: the official Luau VM, sandboxed with luaL_sandbox, plus a small set of
// Nexus extensions (print/warn capture, task scheduler, loadstring).
//
// Guarantees enforced here (independently of process isolation):
//   * wall-clock timeout and user cancellation, checked at every VM safepoint (loop back-edges,
//     calls, returns) through the Luau interrupt callback; pcall cannot swallow the abort because
//     the error is re-raised at every subsequent safepoint;
//   * a hard heap limit enforced by a custom allocator;
//   * bounded output.
// Limitation: code blocked inside a single native call (e.g. a pathological string pattern) only
// observes the limits when that call returns. The isolated runner adds a SIGKILL backstop.

#include "nexus/common/cancellation.hpp"
#include "nexus/runtime/types.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace nexus {

struct RuntimeOptions {
    std::string chunkName = "Script";
    std::string targetId = "luau-inprocess";
    ExecutionLimits limits;
    bool enableTaskLibrary = true;
    bool enableLoadstring = true;
};

class LuauRuntime {
public:
    // Compiles and runs `source` on the calling thread until the main chunk and every scheduled
    // task finish, an error occurs, a limit is hit or `cancel` is triggered.
    static ExecutionResult run(std::string_view source, const RuntimeOptions& options, const OutputCallback& output,
                               const CancellationToken& cancel);
};

// Globals that exist in the standalone runtime, enumerated from a real VM configured exactly like
// LuauRuntime (so they always match the linked Luau version).
const std::vector<std::string>& standaloneGlobals();
bool isStandaloneGlobal(std::string_view name);
// Members of table-valued standalone globals (math, string, task, ...).
const std::map<std::string, std::vector<std::string>>& standaloneLibraryMembers();

} // namespace nexus
