#include "nexus_test.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace nexus_test {

namespace {
int gFailures = 0;
std::string gCurrent;
std::string gTempDir;
} // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void recordFailure(const char* file, int line, const std::string& message) {
    ++gFailures;
    std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, message.c_str());
}

void skip(const std::string& reason) { throw Skipped(reason); }

std::string tempDir() {
    if (gTempDir.empty()) {
        std::string base = std::filesystem::temp_directory_path().string();
        std::string pattern = base + "/nexus-test-XXXXXX";
        std::vector<char> buf(pattern.begin(), pattern.end());
        buf.push_back('\0');
        if (!mkdtemp(buf.data())) throw std::runtime_error("mkdtemp failed");
        gTempDir = buf.data();
    }
    return gTempDir;
}

std::string binDir() {
    char path[4096] = {0};
#ifdef __APPLE__
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return ".";
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return ".";
#endif
    return std::filesystem::weakly_canonical(path).parent_path().string();
}

} // namespace nexus_test

int main(int argc, char** argv) {
    using namespace nexus_test;
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0, skipped = 0;
    for (const auto& c : registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        gCurrent = c.name;
        int before = gFailures;
        auto start = std::chrono::steady_clock::now();
        std::string note;
        bool wasSkipped = false;
        try {
            c.fn();
        } catch (const RequireFailed&) {
        } catch (const Skipped& s) {
            wasSkipped = true;
            note = s.what();
        } catch (const std::exception& e) {
            recordFailure("<exception>", 0, e.what());
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (wasSkipped) {
            ++skipped;
            std::printf("[ SKIP ] %s (%s)\n", c.name, note.c_str());
        } else if (gFailures == before) {
            ++passed;
            std::printf("[  OK  ] %s (%.1f ms)\n", c.name, ms);
        } else {
            ++failed;
            std::printf("[ FAIL ] %s (%.1f ms)\n", c.name, ms);
        }
        std::fflush(stdout);
    }
    std::printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    if (!gTempDir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(gTempDir, ec);
    }
    return failed == 0 ? 0 : 1;
}
