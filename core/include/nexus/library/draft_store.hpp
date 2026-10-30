#pragma once
// Crash-safe storage for unsaved editor buffers. The editor saves drafts shortly after each edit;
// on the next launch Nexus restores them (never executes them).

#include "nexus/common/result.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace nexus {

struct Draft {
    std::string id;          // editor tab id (alphanumeric, '-' and '_')
    std::string title;
    std::string content;
    std::string libraryPath; // associated library script, if any
    std::chrono::system_clock::time_point savedAt;
};

class DraftStore {
public:
    explicit DraftStore(std::filesystem::path directory);

    Result<void> save(const Draft& draft);
    std::vector<Draft> list() const; // newest first; corrupt drafts are skipped
    Result<void> remove(const std::string& id);
    void clear();

private:
    Result<std::filesystem::path> pathFor(const std::string& id) const;
    std::filesystem::path dir_;
};

} // namespace nexus
