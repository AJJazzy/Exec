#pragma once
// RAII child process with stdio pipes (posix_spawn). The destructor terminates and reaps the child,
// so no execution helper can outlive the object that started it.

#include <chrono>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace nexus::platform {

struct SpawnOptions {
    std::string executable;               // absolute path
    std::vector<std::string> arguments;   // argv[1..]
    std::vector<std::string> environment; // "KEY=VALUE"; empty = minimal environment (see below)
    bool inheritEnvironment = false;      // when false only PATH/HOME/TMPDIR/LANG are passed through
    std::string workingDirectory;         // empty = inherit
    bool newProcessGroup = true;          // allows killing the child and its descendants together
};

struct ExitStatus {
    bool exited = false;   // terminated normally
    int code = -1;         // exit code when exited
    bool signaled = false; // terminated by a signal
    int signal = 0;
    std::string describe() const;
};

class Subprocess {
public:
    Subprocess() = default;
    ~Subprocess();
    Subprocess(Subprocess&& other) noexcept;
    Subprocess& operator=(Subprocess&& other) noexcept;
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    // Returns std::nullopt and fills `error` on failure.
    static std::optional<Subprocess> spawn(const SpawnOptions& options, std::string* error);

    pid_t pid() const { return pid_; }
    bool running() const { return pid_ > 0 && !status_; }
    int stdinFd() const { return stdin_; }
    int stdoutFd() const { return stdout_; }
    int stderrFd() const { return stderr_; }

    // Writes all bytes to the child's stdin; returns false if the pipe is closed.
    bool writeStdin(const std::string& data);
    void closeStdin();

    // Sends a signal to the child (or its process group).
    void signal(int sig);
    void terminate();
    void kill();

    // Non-blocking reap; returns the exit status once the child has exited.
    std::optional<ExitStatus> tryWait();
    // Waits up to `timeout` for the child to exit.
    std::optional<ExitStatus> waitFor(std::chrono::milliseconds timeout);
    // Blocks until the child exits.
    ExitStatus wait();

private:
    void closeAll();
    void reset();

    pid_t pid_ = -1;
    bool processGroup_ = false;
    int stdin_ = -1;
    int stdout_ = -1;
    int stderr_ = -1;
    std::optional<ExitStatus> status_;
};

// Reads available bytes from fd into `out`; returns false on EOF or error.
bool readAvailable(int fd, std::string& out);

} // namespace nexus::platform
