#include "nexus/execution/local_targets.hpp"

#include "nexus/common/strings.hpp"
#include "nexus/execution/runner_protocol.hpp"
#include "nexus/platform/subprocess.hpp"
#include "nexus/platform/system.hpp"
#include "nexus/runtime/luau_runtime.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <unistd.h>

namespace nexus {

// ---------------------------------------------------------------------------------------------
// InProcessTarget

TargetInfo InProcessTarget::info() const {
    TargetInfo i;
    i.id = kId;
    i.displayName = "Luau (in-process)";
    i.description = "Official Luau VM running on a background thread inside Nexus. Fastest start-up; "
                    "a script blocked inside a single native call cannot be forcibly terminated.";
    i.kind = TargetKind::StandaloneLuau;
    i.capabilities.cancellation = true;
    i.capabilities.guaranteedStop = false;
    i.capabilities.resourceLimits = true;
    i.capabilities.outputCapture = true;
    i.capabilities.taskScheduler = true;
    return i;
}

TargetAvailability InProcessTarget::availability() const { return {true, "Ready"}; }

ExecutionResult InProcessTarget::execute(const ExecutionRequest& request, const OutputCallback& output, const CancellationToken& cancel) {
    RuntimeOptions options;
    options.chunkName = request.scriptName;
    options.targetId = kId;
    options.limits = request.limits;
    return LuauRuntime::run(request.source, options, output, cancel);
}

// ---------------------------------------------------------------------------------------------
// IsolatedTarget

namespace {

// Self-pipe that wakes poll() when cancellation is requested. Shared ownership keeps it valid if
// the cancellation callback fires after execute() returned.
class WakePipe {
public:
    WakePipe() {
        if (pipe(fds_) == 0) {
            for (int fd : fds_) fcntl(fd, F_SETFD, FD_CLOEXEC);
            fcntl(fds_[1], F_SETFL, O_NONBLOCK);
        }
    }
    ~WakePipe() { close(); }
    void wake() {
        std::lock_guard lock(mutex_);
        if (fds_[1] >= 0) {
            char c = 1;
            (void)!::write(fds_[1], &c, 1);
        }
    }
    void close() {
        std::lock_guard lock(mutex_);
        for (int& fd : fds_) {
            if (fd >= 0) ::close(fd);
            fd = -1;
        }
    }
    int readFd() const { return fds_[0]; }

private:
    std::mutex mutex_;
    int fds_[2] = {-1, -1};
};

std::string tail(const std::string& s, size_t max) { return s.size() <= max ? s : "…" + s.substr(s.size() - max); }

} // namespace

IsolatedTarget::IsolatedTarget(std::string runnerPath, bool enableSandbox)
    : runnerPath_(std::move(runnerPath)), enableSandbox_(enableSandbox) {}

TargetInfo IsolatedTarget::info() const {
    TargetInfo i;
    i.id = kId;
    i.displayName = "Luau (isolated process)";
    i.description = "Official Luau VM in a separate nexus-runner process with CPU, file and process "
                    "resource limits and, on macOS, a pure-computation Seatbelt sandbox. Stop is "
                    "enforced by terminating the process.";
    i.kind = TargetKind::StandaloneLuau;
    i.capabilities.cancellation = true;
    i.capabilities.guaranteedStop = true;
    i.capabilities.resourceLimits = true;
    i.capabilities.outputCapture = true;
    i.capabilities.processIsolation = true;
    i.capabilities.taskScheduler = true;
    return i;
}

TargetAvailability IsolatedTarget::availability() const {
    std::error_code ec;
    if (runnerPath_.empty() || !std::filesystem::exists(runnerPath_, ec))
        return {false, "nexus-runner executable not found" + (runnerPath_.empty() ? std::string() : " at " + runnerPath_)};
    if (access(runnerPath_.c_str(), X_OK) != 0) return {false, "nexus-runner is not executable: " + runnerPath_};
    return {true, "Ready"};
}

ExecutionResult IsolatedTarget::execute(const ExecutionRequest& request, const OutputCallback& output, const CancellationToken& cancel) {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    auto elapsedMs = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };

    if (auto avail = availability(); !avail.available) return makeUnavailableResult(kId, avail.reason);

    platform::SpawnOptions spawn;
    spawn.executable = runnerPath_;
    spawn.arguments = {"--serve-one"};
    if (!enableSandbox_) spawn.arguments.push_back("--no-sandbox");
    std::string spawnError;
    auto child = platform::Subprocess::spawn(spawn, &spawnError);
    if (!child) {
        ExecutionResult r;
        r.status = ExecutionStatus::InternalError;
        r.target = kId;
        r.summary = "Could not start nexus-runner: " + spawnError;
        return r;
    }

    auto wake = std::make_shared<WakePipe>();
    cancel.onCancel([wake] { wake->wake(); });

    std::optional<ExecutionResult> result;
    std::string stderrText;
    runner::LineBuffer lines;
    bool cancelSent = false;
    bool killedForCancel = false;
    bool killedForDeadline = false;
    bool protocolError = false;
    std::string protocolDetail;

    // Hard deadline = script limit + grace period + start-up allowance. The runner enforces the
    // script limit itself; this is the backstop for a runner stuck in native code.
    auto hardDeadline = start + request.limits.timeout + request.limits.killGracePeriod + std::chrono::seconds(2);
    std::optional<Clock::time_point> killAt;

    std::string runLine = json::dump(runner::makeRunMessage(request)) + "\n";
    if (!child->writeStdin(runLine)) {
        protocolError = true;
        protocolDetail = "runner closed its input before receiving the script";
    }

    bool stdoutOpen = true;
    bool stderrOpen = true;
    while (stdoutOpen && !protocolError) {
        auto now = Clock::now();
        if (cancel.requested() && !cancelSent) {
            child->writeStdin(json::dump(runner::makeCancelMessage()) + "\n");
            cancelSent = true;
            killAt = now + request.limits.killGracePeriod;
        }
        if (killAt && now >= *killAt) {
            child->kill();
            killedForCancel = true;
            break;
        }
        if (now >= hardDeadline) {
            child->kill();
            killedForDeadline = true;
            break;
        }
        auto until = killAt ? std::min(*killAt, hardDeadline) : hardDeadline;
        int timeoutMs = static_cast<int>(std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count()));

        pollfd fds[3];
        nfds_t n = 0;
        fds[n++] = {child->stdoutFd(), POLLIN, 0};
        int stderrIndex = -1;
        if (stderrOpen) {
            stderrIndex = static_cast<int>(n);
            fds[n++] = {child->stderrFd(), POLLIN, 0};
        }
        int wakeIndex = static_cast<int>(n);
        fds[n++] = {wake->readFd(), POLLIN, 0};

        int rc = poll(fds, n, timeoutMs);
        if (rc < 0) {
            if (errno == EINTR) continue;
            protocolError = true;
            protocolDetail = std::string("poll failed: ") + std::strerror(errno);
            break;
        }
        if (fds[wakeIndex].revents & POLLIN) {
            char drain[16];
            (void)!read(wake->readFd(), drain, sizeof(drain));
        }
        if (stderrIndex >= 0 && (fds[stderrIndex].revents & (POLLIN | POLLHUP))) {
            std::string chunk;
            if (!platform::readAvailable(child->stderrFd(), chunk)) stderrOpen = false;
            stderrText = tail(stderrText + chunk, 16384);
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            std::string chunk;
            if (!platform::readAvailable(child->stdoutFd(), chunk)) stdoutOpen = false;
            lines.append(chunk);
            if (lines.pendingBytes() > runner::kMaxMessageBytes) {
                protocolError = true;
                protocolDetail = "runner sent an oversized message";
                child->kill();
                break;
            }
            std::string line;
            while (lines.next(line)) {
                auto msg = json::parse(line);
                if (!msg.ok()) {
                    protocolError = true;
                    protocolDetail = "malformed message from runner: " + msg.error().message;
                    child->kill();
                    break;
                }
                const std::string type = msg.value().getString("type");
                if (type == "output") {
                    if (const json::Value* e = msg.value().get("event"); e && output) output(OutputEvent::fromJson(*e));
                } else if (type == "ready") {
                    const json::Value& m = msg.value();
                    std::string sandbox = m.get("sandbox") && m.get("sandbox")->getBool("applied")
                                              ? m.get("sandbox")->getString("mechanism")
                                              : "not applied (" + (m.get("sandbox") ? m.get("sandbox")->getString("detail") : std::string("unknown")) + ")";
                    if (output)
                        output(OutputEvent{OutputStream::Info,
                                           "Isolated runner started (pid " + std::to_string(m.getInt("pid")) + "); sandbox: " + sandbox,
                                           elapsedMs()});
                } else if (type == "result") {
                    if (const json::Value* r = msg.value().get("result")) result = ExecutionResult::fromJson(*r);
                }
            }
        }
    }

    // Reap the runner. It normally exits right after sending the result.
    auto exit = child->waitFor(std::chrono::milliseconds(1500));
    if (!exit) {
        child->kill();
        exit = child->wait();
    }
    wake->close();

    if (result && !killedForCancel && !killedForDeadline && !protocolError) {
        result->target = kId;
        return *result;
    }

    ExecutionResult r;
    r.target = kId;
    r.stats.wallMs = elapsedMs();
    if (killedForCancel) {
        r.status = ExecutionStatus::Cancelled;
        r.summary = "Stopped by user; the runner did not stop within " + str::formatDuration(static_cast<double>(request.limits.killGracePeriod.count())) +
                    " and was terminated (SIGKILL)";
    } else if (killedForDeadline) {
        r.status = ExecutionStatus::TimedOut;
        r.summary = "Stopped: the runner exceeded the time limit while blocked and was terminated (SIGKILL)";
    } else if (exit && exit->signaled && (exit->signal == SIGXCPU || exit->signal == SIGKILL) && !cancelSent) {
        r.status = ExecutionStatus::TimedOut;
        r.summary = "Stopped: the runner exceeded its CPU-time limit (" + exit->describe() + ")";
    } else if (cancelSent) {
        r.status = ExecutionStatus::Cancelled;
        r.summary = "Stopped by user";
    } else {
        r.status = ExecutionStatus::InternalError;
        r.summary = "nexus-runner failed: " + (protocolError ? protocolDetail : (exit ? exit->describe() : std::string("no result")));
        if (!stderrText.empty()) r.summary += " — " + std::string(str::trim(tail(stderrText, 400)));
    }
    return r;
}

std::string findRunnerExecutable() {
    std::filesystem::path dir = platform::executableDirectory();
    for (const auto& candidate : {dir / "nexus-runner", dir / "../Helpers/nexus-runner", dir / "../MacOS/nexus-runner"}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return std::filesystem::weakly_canonical(candidate, ec).string();
    }
    return {};
}

} // namespace nexus
