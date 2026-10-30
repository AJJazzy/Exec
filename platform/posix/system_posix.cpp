// POSIX implementations shared by macOS and Linux.
#include "nexus/platform/system.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace nexus::platform {

bool processExists(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true; // signal 0 only checks existence/permission
    return errno == EPERM;
}

std::vector<std::string> applyResourceLimits(const ResourceLimitRequest& request) {
    std::vector<std::string> notes;
    auto apply = [&](int resource, rlim_t soft, rlim_t hard, const char* name) {
        rlimit current{};
        getrlimit(resource, &current);
        rlimit lim{soft, hard};
        // Never try to raise a hard limit (that requires privileges).
        if (current.rlim_max != RLIM_INFINITY && lim.rlim_max > current.rlim_max) lim.rlim_max = current.rlim_max;
        if (lim.rlim_cur > lim.rlim_max) lim.rlim_cur = lim.rlim_max;
        if (setrlimit(resource, &lim) == 0) notes.push_back(std::string(name) + "=" + std::to_string(static_cast<unsigned long long>(lim.rlim_cur)));
        else notes.push_back(std::string(name) + " failed: " + std::strerror(errno));
    };
    if (request.cpuSeconds) apply(RLIMIT_CPU, request.cpuSeconds, request.cpuSeconds + 2, "RLIMIT_CPU");
    apply(RLIMIT_FSIZE, request.maxFileBytes, request.maxFileBytes, "RLIMIT_FSIZE");
    if (request.maxOpenFiles) apply(RLIMIT_NOFILE, request.maxOpenFiles, request.maxOpenFiles, "RLIMIT_NOFILE");
    if (request.disableCoreDumps) apply(RLIMIT_CORE, 0, 0, "RLIMIT_CORE");
    if (request.forbidNewProcesses) apply(RLIMIT_NPROC, 0, 0, "RLIMIT_NPROC");
    // RLIMIT_FSIZE raises SIGXFSZ on violation; turn it into an EFBIG error instead.
    std::signal(SIGXFSZ, SIG_IGN);
    return notes;
}

std::string homeDirectory() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/tmp";
}

std::string applicationSupportDirectory(const std::string& appName) {
#ifdef __APPLE__
    return homeDirectory() + "/Library/Application Support/" + appName;
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/" + appName;
    return homeDirectory() + "/.local/share/" + appName;
#endif
}

std::string executableDirectory() {
    std::string path;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(buf.c_str(), resolved)) path = resolved;
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) path.assign(buf, static_cast<size_t>(n));
#endif
    auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string machineArchitecture() {
    utsname u{};
    if (uname(&u) != 0) return "unknown";
    return u.machine;
}

} // namespace nexus::platform
