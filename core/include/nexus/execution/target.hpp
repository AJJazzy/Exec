#pragma once
// An execution target is somewhere Nexus can run Luau: the in-process VM, the isolated runner
// process, or Roblox Studio through its official MCP server. The Roblox Player is modelled as a
// target too, but it always reports itself unavailable (see integrations/roblox).

#include "nexus/common/cancellation.hpp"
#include "nexus/runtime/types.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nexus {

enum class TargetKind { StandaloneLuau, RobloxStudio, RobloxPlayer };
const char* toString(TargetKind k);

struct TargetCapabilities {
    bool robloxEngineApis = false; // game, workspace, Instance, Vector3, ... exist
    bool cancellation = false;     // a running script can be stopped
    bool guaranteedStop = false;   // stopping is enforced (process kill), not just requested
    bool resourceLimits = false;   // memory/time limits are enforced by Nexus
    bool outputCapture = false;    // print/warn output is returned
    bool processIsolation = false; // runs outside the Nexus process
    bool taskScheduler = false;    // task.wait/spawn/defer/delay work

    json::Value toJson() const;
};

struct TargetInfo {
    std::string id;
    std::string displayName;
    std::string description;
    TargetKind kind = TargetKind::StandaloneLuau;
    TargetCapabilities capabilities;

    json::Value toJson() const;
};

struct TargetAvailability {
    bool available = false;
    std::string reason; // why not, or a short status when available
};

struct ExecutionRequest {
    std::string source;
    std::string scriptName = "Script";
    ExecutionLimits limits;
    json::Object options; // target-specific (e.g. {"dataModel": "Edit"} for Roblox Studio)
};

class ExecutionTarget {
public:
    virtual ~ExecutionTarget() = default;

    virtual TargetInfo info() const = 0;
    virtual TargetAvailability availability() const = 0;

    // Runs the request and blocks until it finishes. Implementations must honour `cancel`, report
    // output through `output` and never report success for work that did not happen.
    virtual ExecutionResult execute(const ExecutionRequest& request, const OutputCallback& output,
                                    const CancellationToken& cancel) = 0;

    // Incremented whenever the underlying connection is re-established, so sessions started on an
    // older connection can be recognised as stale.
    virtual uint64_t generation() const { return 0; }
};

class TargetRegistry {
public:
    void add(std::shared_ptr<ExecutionTarget> target);
    std::shared_ptr<ExecutionTarget> find(const std::string& id) const;
    std::vector<std::shared_ptr<ExecutionTarget>> list() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ExecutionTarget>> targets_;
};

// Builds an ExecutionResult for a target that cannot run anything right now.
ExecutionResult makeUnavailableResult(const std::string& targetId, const std::string& reason);

} // namespace nexus
