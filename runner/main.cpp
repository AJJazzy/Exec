// nexus-runner: executes exactly one Luau script on behalf of Nexus, then exits.
//
// Isolation sequence:
//   1. read the run request from stdin (before any restriction applies);
//   2. start the stdin watcher that receives cancellation requests;
//   3. apply rlimits (CPU seconds, no regular-file writes, few descriptors, no core dumps, no fork);
//   4. apply the OS sandbox (macOS Seatbelt pure-computation profile);
//   5. run the script with the runtime's own time/memory/output limits;
//   6. report the result and exit.
// The host additionally SIGKILLs the runner if it does not finish within the grace period.

#include "nexus/common/json.hpp"
#include "nexus/compiler/compiler.hpp"
#include "nexus/execution/runner_protocol.hpp"
#include "nexus/platform/system.hpp"
#include "nexus/runtime/luau_runtime.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifndef NEXUS_VERSION
#define NEXUS_VERSION "dev"
#endif

namespace {

std::mutex gWriteMutex;

void writeMessage(const nexus::json::Value& message) {
    std::string line = nexus::json::dump(message) + "\n";
    std::lock_guard lock(gWriteMutex);
    size_t done = 0;
    while (done < line.size()) {
        ssize_t n = write(STDOUT_FILENO, line.data() + done, line.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            _exit(3); // host went away
        }
        done += static_cast<size_t>(n);
    }
}

bool readLine(int fd, std::string& buffer, std::string& line) {
    while (true) {
        size_t nl = buffer.find('\n');
        if (nl != std::string::npos) {
            line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            return true;
        }
        if (buffer.size() > nexus::runner::kMaxMessageBytes) return false;
        char chunk[65536];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
    }
}

int probeSandbox(bool sandbox) {
    // Used by the integration tests to measure (not assume) what the sandbox blocks.
    nexus::platform::SandboxStatus status;
    if (sandbox) status = nexus::platform::applyComputeOnlySandbox();
    std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/nexus-sandbox-probe-" + std::to_string(getpid());
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    int fileErrno = fd < 0 ? errno : 0;
    if (fd >= 0) {
        close(fd);
        unlink(path.c_str());
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int netErrno = 0;
    if (sock < 0) {
        netErrno = errno;
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) netErrno = errno;
        close(sock);
    }
    nexus::json::Value out = nexus::json::Object{
        {"sandboxApplied", status.applied},
        {"mechanism", status.mechanism},
        {"detail", status.detail},
        {"fileWriteBlocked", fd < 0 && (fileErrno == EPERM || fileErrno == EACCES)},
        {"fileErrno", fileErrno},
        {"networkBlocked", netErrno == EPERM || netErrno == EACCES},
        {"networkErrno", netErrno},
    };
    std::printf("%s\n", nexus::json::dump(out).c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool sandbox = true;
    bool serve = false;
    bool probe = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--serve-one") serve = true;
        else if (arg == "--no-sandbox") sandbox = false;
        else if (arg == "--probe-sandbox") probe = true;
        else if (arg == "--version") {
            std::printf("nexus-runner %s (Luau %s, protocol %d)\n", NEXUS_VERSION, nexus::luauVersion(), nexus::runner::kProtocolVersion);
            return 0;
        } else {
            std::fprintf(stderr, "usage: nexus-runner --serve-one [--no-sandbox] | --probe-sandbox [--no-sandbox] | --version\n");
            return 2;
        }
    }
    if (probe) return probeSandbox(sandbox);
    if (!serve) {
        std::fprintf(stderr, "nexus-runner is started by Nexus; run with --version for information.\n");
        return 2;
    }

    std::string buffer;
    std::string line;
    if (!readLine(STDIN_FILENO, buffer, line)) {
        std::fprintf(stderr, "nexus-runner: no request received\n");
        return 2;
    }
    auto request = nexus::json::parse(line);
    if (!request.ok() || request.value().getString("type") != "run") {
        std::fprintf(stderr, "nexus-runner: invalid request\n");
        return 2;
    }
    if (request.value().getInt("protocol") != nexus::runner::kProtocolVersion) {
        std::fprintf(stderr, "nexus-runner: protocol mismatch\n");
        return 2;
    }

    nexus::RuntimeOptions options;
    options.targetId = "luau-isolated";
    options.chunkName = request.value().getString("chunkName", "Script");
    if (const nexus::json::Value* limits = request.value().get("limits")) options.limits = nexus::ExecutionLimits::fromJson(*limits);
    std::string source = request.value().getString("source");

    nexus::CancellationSource cancel;
    std::thread watcher([&cancel, buffer]() mutable {
        std::string msg;
        while (readLine(STDIN_FILENO, buffer, msg)) {
            auto m = nexus::json::parse(msg);
            if (m.ok() && m.value().getString("type") == "cancel") cancel.cancel();
        }
        // stdin closed: the host is gone or stopped us; treat as cancellation.
        cancel.cancel();
    });
    watcher.detach();

    nexus::platform::ResourceLimitRequest rl;
    rl.cpuSeconds = static_cast<uint64_t>(std::ceil(options.limits.timeout.count() / 1000.0)) + 2;
    rl.maxFileBytes = 0;
    rl.maxOpenFiles = 16;
    std::vector<std::string> limitNotes = nexus::platform::applyResourceLimits(rl);

    nexus::platform::SandboxStatus sandboxStatus;
    if (sandbox) sandboxStatus = nexus::platform::applyComputeOnlySandbox();
    else sandboxStatus.detail = "disabled by the host (--no-sandbox)";

    nexus::json::Array notes;
    for (auto& n : limitNotes) notes.push_back(n);
    writeMessage(nexus::json::Object{
        {"type", "ready"},
        {"protocol", nexus::runner::kProtocolVersion},
        {"pid", static_cast<int64_t>(getpid())},
        {"sandbox", nexus::json::Object{{"applied", sandboxStatus.applied}, {"mechanism", sandboxStatus.mechanism}, {"detail", sandboxStatus.detail}}},
        {"limits", std::move(notes)},
    });

    nexus::ExecutionResult result = nexus::LuauRuntime::run(
        source, options, [](const nexus::OutputEvent& e) { writeMessage(nexus::runner::makeOutputMessage(e)); }, cancel.token());
    writeMessage(nexus::runner::makeResultMessage(result));
    // _exit: the stdin watcher may still be blocked in read(); nothing else needs cleanup.
    _exit(0);
}
