#pragma once
// Standalone Luau targets.
//
//   InProcessTarget   runs the VM on a worker thread inside the host. Fast to start, but a script
//                     stuck inside a single native call cannot be forcibly stopped.
//   IsolatedTarget    runs every script in a fresh nexus-runner process with rlimits and (on macOS)
//                     a Seatbelt sandbox. Stop is guaranteed: the runner is sent a cancel message
//                     and is SIGKILLed if it has not exited after the grace period.

#include "nexus/execution/target.hpp"

#include <atomic>
#include <string>

namespace nexus {

class InProcessTarget final : public ExecutionTarget {
public:
    static constexpr const char* kId = "luau-inprocess";

    TargetInfo info() const override;
    TargetAvailability availability() const override;
    ExecutionResult execute(const ExecutionRequest& request, const OutputCallback& output, const CancellationToken& cancel) override;
};

class IsolatedTarget final : public ExecutionTarget {
public:
    static constexpr const char* kId = "luau-isolated";

    // `runnerPath` is the absolute path to the nexus-runner executable.
    explicit IsolatedTarget(std::string runnerPath, bool enableSandbox = true);

    TargetInfo info() const override;
    TargetAvailability availability() const override;
    ExecutionResult execute(const ExecutionRequest& request, const OutputCallback& output, const CancellationToken& cancel) override;

    const std::string& runnerPath() const { return runnerPath_; }
    void setSandboxEnabled(bool enabled) { enableSandbox_ = enabled; }
    bool sandboxEnabled() const { return enableSandbox_; }

private:
    std::string runnerPath_;
    std::atomic<bool> enableSandbox_;
};

// Locates nexus-runner next to the current executable (or inside the app bundle).
std::string findRunnerExecutable();

} // namespace nexus
