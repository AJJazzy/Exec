#pragma once
// Local script library rooted at a directory (by default ~/Library/Application Support/Nexus/Library).
//
// Safety properties:
//   * every path is resolved relative to the library root; absolute paths, ".." and hidden names
//     are rejected, so library operations can never touch files outside the root;
//   * writes are atomic (temporary file + rename);
//   * deletions move items to <root>/.trash, from where they can be recovered;
//   * imports copy bytes exactly (no newline or encoding normalisation) and never execute code.

#include "nexus/common/json.hpp"
#include "nexus/common/result.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace nexus {

struct LibraryEntry {
    std::string path; // relative to the library root ("" for the root itself)
    std::string name;
    bool isFolder = false;
    bool isProject = false;
    bool favorite = false;
    uint64_t sizeBytes = 0;
    std::chrono::system_clock::time_point modified;
    std::vector<LibraryEntry> children;

    json::Value toJson() const;
};

struct LibrarySearchHit {
    std::string path;
    bool nameMatch = false;
    uint32_t line = 0; // 1-based line of the first content match (0 for name matches)
    std::string preview;
};

class ScriptLibrary {
public:
    static constexpr uint64_t kMaxScriptBytes = 8ull << 20;

    explicit ScriptLibrary(std::filesystem::path root);

    Result<void> initialize();
    const std::filesystem::path& root() const { return root_; }

    LibraryEntry tree() const;
    Result<std::string> read(const std::string& path) const;
    Result<void> write(const std::string& path, const std::string& content);

    Result<std::string> createScript(const std::string& folder, const std::string& name, const std::string& content = {});
    Result<std::string> createFolder(const std::string& parent, const std::string& name, bool project = false);
    Result<std::string> rename(const std::string& path, const std::string& newName);
    Result<std::string> duplicate(const std::string& path);
    Result<std::string> move(const std::string& path, const std::string& destinationFolder);
    Result<std::string> remove(const std::string& path); // returns the trash location

    Result<void> setFavorite(const std::string& path, bool favorite);
    Result<void> setProject(const std::string& folder, bool project);
    void markOpened(const std::string& path);
    std::vector<std::string> favorites() const;
    std::vector<std::string> recent(size_t limit = 20) const;
    std::vector<std::string> projects() const;

    std::vector<LibrarySearchHit> search(const std::string& query, size_t limit = 200) const;

    // Copies an external .lua/.luau/.txt file into `folder` byte-for-byte. Never executes it.
    Result<std::string> importFile(const std::filesystem::path& external, const std::string& folder);
    // Copies a library script or folder to `destination` byte-for-byte.
    Result<void> exportItem(const std::string& path, const std::filesystem::path& destination) const;

    static bool isScriptFileName(const std::string& name);
    static Result<void> validateName(const std::string& name);

private:
    Result<std::filesystem::path> resolve(const std::string& relative) const;
    std::string relativeOf(const std::filesystem::path& absolute) const;
    std::string uniqueName(const std::filesystem::path& folder, const std::string& desired) const;
    void loadMetadata();
    void saveMetadata() const;
    void renameInMetadata(const std::string& from, const std::string& to);
    void removeFromMetadata(const std::string& path);
    LibraryEntry buildEntry(const std::filesystem::path& p) const;

    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::vector<std::string> favorites_;
    std::vector<std::pair<std::string, int64_t>> recent_; // path, unix ms
    std::vector<std::string> projects_;
};

// Atomically replaces `path` with `content` (write to a temporary file, fsync, rename).
Result<void> writeFileAtomic(const std::filesystem::path& path, const std::string& content);
Result<std::string> readFile(const std::filesystem::path& path, uint64_t maxBytes = 64ull << 20);

} // namespace nexus
