#include "nexus/platform/subprocess.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace nexus::platform {

namespace {

void setCloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void ignoreSigpipe(int fd) {
#ifdef F_SETNOSIGPIPE
    fcntl(fd, F_SETNOSIGPIPE, 1); // macOS: per-descriptor, no global side effects
#else
    (void)fd;
    // Elsewhere SIGPIPE must be ignored process-wide, otherwise writing to an exited child kills
    // the host. Writes then fail with EPIPE, which writeStdin reports.
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
#endif
}

bool makePipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    setCloexec(fds[0]);
    setCloexec(fds[1]);
    return true;
}

ExitStatus decode(int status) {
    ExitStatus s;
    if (WIFEXITED(status)) {
        s.exited = true;
        s.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        s.signaled = true;
        s.signal = WTERMSIG(status);
    }
    return s;
}

} // namespace

std::string ExitStatus::describe() const {
    if (exited) return "exited with code " + std::to_string(code);
    if (signaled) {
        const char* name = strsignal(signal);
        return "terminated by signal " + std::to_string(signal) + (name ? std::string(" (") + name + ")" : std::string());
    }
    return "unknown exit status";
}

Subprocess::~Subprocess() { reset(); }

Subprocess::Subprocess(Subprocess&& other) noexcept { *this = std::move(other); }

Subprocess& Subprocess::operator=(Subprocess&& other) noexcept {
    if (this != &other) {
        reset();
        pid_ = other.pid_;
        processGroup_ = other.processGroup_;
        stdin_ = other.stdin_;
        stdout_ = other.stdout_;
        stderr_ = other.stderr_;
        status_ = other.status_;
        other.pid_ = -1;
        other.stdin_ = other.stdout_ = other.stderr_ = -1;
        other.status_.reset();
    }
    return *this;
}

void Subprocess::reset() {
    if (pid_ > 0 && !status_) {
        kill();
        wait();
    }
    closeAll();
    pid_ = -1;
}

void Subprocess::closeAll() {
    for (int* fd : {&stdin_, &stdout_, &stderr_}) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
    }
}

std::optional<Subprocess> Subprocess::spawn(const SpawnOptions& options, std::string* error) {
    auto fail = [&](const std::string& what, int err) -> std::optional<Subprocess> {
        if (error) *error = what + ": " + std::strerror(err);
        return std::nullopt;
    };

    int in[2], out[2], err[2];
    if (!makePipe(in)) return fail("pipe", errno);
    if (!makePipe(out)) {
        close(in[0]); close(in[1]);
        return fail("pipe", errno);
    }
    if (!makePipe(err)) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        return fail("pipe", errno);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    if (!options.workingDirectory.empty()) {
#if defined(__APPLE__) || (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29)))
        posix_spawn_file_actions_addchdir_np(&actions, options.workingDirectory.c_str());
#endif
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    if (options.newProcessGroup) {
        flags |= POSIX_SPAWN_SETPGROUP;
        posix_spawnattr_setpgroup(&attr, 0);
    }
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; // macOS: only the three stdio descriptors are inherited
#endif
    posix_spawnattr_setflags(&attr, flags);
    sigset_t defaults, mask;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setsigmask(&attr, &mask);

    std::vector<std::string> argStorage;
    argStorage.push_back(options.executable);
    argStorage.insert(argStorage.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    for (auto& a : argStorage) argv.push_back(a.data());
    argv.push_back(nullptr);

    std::vector<std::string> envStorage = options.environment;
    if (options.inheritEnvironment) {
        for (char** e = environ; e && *e; ++e) envStorage.emplace_back(*e);
    } else {
        for (const char* key : {"PATH", "HOME", "TMPDIR", "LANG", "USER"}) {
            if (const char* v = std::getenv(key)) envStorage.push_back(std::string(key) + "=" + v);
        }
    }
    std::vector<char*> envp;
    for (auto& e : envStorage) envp.push_back(e.data());
    envp.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, options.executable.c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (rc != 0) {
        close(in[1]); close(out[0]); close(err[0]);
        return fail("posix_spawn " + options.executable, rc);
    }

    Subprocess p;
    p.pid_ = pid;
    p.processGroup_ = options.newProcessGroup;
    p.stdin_ = in[1];
    p.stdout_ = out[0];
    p.stderr_ = err[0];
    ignoreSigpipe(p.stdin_);
    return p;
}

bool Subprocess::writeStdin(const std::string& data) {
    if (stdin_ < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(stdin_, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

void Subprocess::closeStdin() {
    if (stdin_ >= 0) close(stdin_);
    stdin_ = -1;
}

void Subprocess::signal(int sig) {
    if (pid_ <= 0 || status_) return;
    if (processGroup_) ::kill(-pid_, sig);
    ::kill(pid_, sig);
}

void Subprocess::terminate() { signal(SIGTERM); }
void Subprocess::kill() { signal(SIGKILL); }

std::optional<ExitStatus> Subprocess::tryWait() {
    if (status_) return status_;
    if (pid_ <= 0) return std::nullopt;
    int st = 0;
    pid_t r = waitpid(pid_, &st, WNOHANG);
    if (r == pid_) status_ = decode(st);
    return status_;
}

std::optional<ExitStatus> Subprocess::waitFor(std::chrono::milliseconds timeout) {
    // Short back-off loop; only used for bounded waits during shutdown, never in steady state.
    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto delay = std::chrono::microseconds(500);
    while (true) {
        if (auto s = tryWait()) return s;
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        std::this_thread::sleep_for(delay);
        delay = std::min<std::chrono::microseconds>(delay * 2, std::chrono::milliseconds(20));
    }
}

ExitStatus Subprocess::wait() {
    if (status_) return *status_;
    if (pid_ <= 0) return {};
    int st = 0;
    while (waitpid(pid_, &st, 0) < 0) {
        if (errno != EINTR) return {};
    }
    status_ = decode(st);
    return *status_;
}

bool readAvailable(int fd, std::string& out) {
    char buf[16384];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            return true;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
}

} // namespace nexus::platform
