// Non-Apple implementations. Linux is a development/test platform for the portable core; the
// Apple-specific facilities (Security.framework, Seatbelt, Launch Services) report themselves as
// unsupported instead of pretending to work.
#include "nexus/platform/system.hpp"

#include <dirent.h>
#include <fstream>
#include <sys/utsname.h>
#include <unistd.h>

namespace nexus::platform {

std::vector<ProcessEntry> listProcesses() {
    std::vector<ProcessEntry> out;
    DIR* dir = opendir("/proc");
    if (!dir) return out;
    while (dirent* e = readdir(dir)) {
        char* end = nullptr;
        long pid = std::strtol(e->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0) continue;
        char buf[4096];
        std::string link = std::string("/proc/") + e->d_name + "/exe";
        ssize_t n = readlink(link.c_str(), buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        out.push_back({static_cast<pid_t>(pid), std::string(buf, static_cast<size_t>(n))});
    }
    closedir(dir);
    return out;
}

std::optional<bool> isProcessTranslated(pid_t) { return std::nullopt; }

SignatureCheck verifyCodeSignature(const std::string&, const std::string&) {
    SignatureCheck c;
    c.supported = false;
    c.error = "code signature verification requires macOS Security.framework";
    return c;
}

SandboxStatus applyComputeOnlySandbox() {
    SandboxStatus s;
    s.applied = false;
    s.mechanism = "none";
    s.detail = "OS sandbox not implemented on this platform; only resource limits apply";
    return s;
}

std::vector<std::string> applicationPathsForBundleIdentifier(const std::string&) { return {}; }

std::optional<std::string> propertyListAsXml(const std::string&) { return std::nullopt; }

std::string operatingSystemDescription() {
    std::string name = "Linux";
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            name = line.substr(12);
            if (name.size() >= 2 && name.front() == '"') name = name.substr(1, name.size() - 2);
        }
    }
    return name + " (" + machineArchitecture() + ")";
}

} // namespace nexus::platform
