#pragma once
// OS services used by Nexus. Every function is read-only with respect to other processes: Nexus
// enumerates processes and inspects files on disk, but never attaches to, signals or modifies a
// process it did not start.

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace nexus::platform {

// ---- Process discovery -----------------------------------------------------------------------

struct ProcessEntry {
    pid_t pid = 0;
    std::string executablePath; // absolute path of the main executable when readable
};

// Snapshot of running processes owned by any user whose executable path is readable.
std::vector<ProcessEntry> listProcesses();
bool processExists(pid_t pid);
// macOS: whether the process runs under Rosetta 2 translation. std::nullopt when unknown or not
// applicable on this platform.
std::optional<bool> isProcessTranslated(pid_t pid);

// ---- Code signing ----------------------------------------------------------------------------

struct SignatureCheck {
    bool supported = false;   // false on platforms without code-signing APIs
    bool signedCode = false;  // a signature is present
    bool valid = false;       // signature validates (and satisfies the team requirement if given)
    std::string identifier;
    std::string teamIdentifier;
    uint32_t flags = 0;       // CodeDirectory flags (e.g. 0x10000 = hardened runtime)
    std::string error;        // validation failure description
};

// Validates the code signature of a bundle or binary with Security.framework. When
// `requiredTeamId` is non-empty the signature must be issued by Apple to that team.
SignatureCheck verifyCodeSignature(const std::string& path, const std::string& requiredTeamId = {});

// ---- Isolation for the execution helper ------------------------------------------------------

struct SandboxStatus {
    bool applied = false;
    std::string mechanism; // e.g. "macOS Seatbelt (pure-computation profile)"
    std::string detail;    // error or limitation description
};

// Irreversibly restricts the *current* process to computation only (no file system writes, no
// network, no process creation where supported). Called by nexus-runner after initialisation.
SandboxStatus applyComputeOnlySandbox();

struct ResourceLimitRequest {
    uint64_t cpuSeconds = 0;     // RLIMIT_CPU (0 = unchanged); SIGXCPU/SIGKILL when exceeded
    uint64_t maxFileBytes = 0;   // RLIMIT_FSIZE; 0 forbids creating/growing regular files
    uint64_t maxOpenFiles = 32;  // RLIMIT_NOFILE
    bool disableCoreDumps = true;
    bool forbidNewProcesses = true; // RLIMIT_NPROC = 0 (ignored for root)
};

// Applies rlimits to the current process; returns human-readable notes for each limit applied
// or failed.
std::vector<std::string> applyResourceLimits(const ResourceLimitRequest& request);

// ---- Paths -----------------------------------------------------------------------------------

std::string homeDirectory();
// ~/Library/Application Support/<app> on macOS, $XDG_DATA_HOME/<app> (or ~/.local/share) elsewhere.
std::string applicationSupportDirectory(const std::string& appName);
// Directory containing the running executable.
std::string executableDirectory();

// ---- Application bundles ---------------------------------------------------------------------

// macOS: application URLs registered with Launch Services for a bundle identifier. Empty elsewhere.
std::vector<std::string> applicationPathsForBundleIdentifier(const std::string& bundleId);

// macOS: converts any property list (XML or binary) to XML text using CoreFoundation.
// Elsewhere returns std::nullopt (callers fall back to the portable XML reader).
std::optional<std::string> propertyListAsXml(const std::string& path);

// Human-readable OS name and version, e.g. "macOS 15.1 (arm64)".
std::string operatingSystemDescription();
std::string machineArchitecture(); // "arm64", "x86_64", ...

} // namespace nexus::platform
