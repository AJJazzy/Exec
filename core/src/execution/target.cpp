#include "nexus/execution/target.hpp"

namespace nexus {

const char* toString(TargetKind k) {
    switch (k) {
    case TargetKind::StandaloneLuau: return "standalone-luau";
    case TargetKind::RobloxStudio: return "roblox-studio";
    case TargetKind::RobloxPlayer: return "roblox-player";
    }
    return "standalone-luau";
}

json::Value TargetCapabilities::toJson() const {
    return json::Object{
        {"robloxEngineApis", robloxEngineApis}, {"cancellation", cancellation}, {"guaranteedStop", guaranteedStop},
        {"resourceLimits", resourceLimits},     {"outputCapture", outputCapture}, {"processIsolation", processIsolation},
        {"taskScheduler", taskScheduler},
    };
}

json::Value TargetInfo::toJson() const {
    return json::Object{
        {"id", id}, {"displayName", displayName}, {"description", description},
        {"kind", nexus::toString(kind)}, {"capabilities", capabilities.toJson()},
    };
}

void TargetRegistry::add(std::shared_ptr<ExecutionTarget> target) {
    std::lock_guard lock(mutex_);
    targets_.push_back(std::move(target));
}

std::shared_ptr<ExecutionTarget> TargetRegistry::find(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& t : targets_) {
        if (t->info().id == id) return t;
    }
    return nullptr;
}

std::vector<std::shared_ptr<ExecutionTarget>> TargetRegistry::list() const {
    std::lock_guard lock(mutex_);
    return targets_;
}

ExecutionResult makeUnavailableResult(const std::string& targetId, const std::string& reason) {
    ExecutionResult r;
    r.status = ExecutionStatus::Unavailable;
    r.target = targetId;
    r.summary = "Not executed: " + reason;
    return r;
}

} // namespace nexus
