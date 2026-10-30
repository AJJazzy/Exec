#include "nexus/library/script_library.hpp"

#include "nexus/common/strings.hpp"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace nexus {

namespace {
constexpr const char* kMetadataFile = ".nexus-library.json";
constexpr const char* kTrashDir = ".trash";

std::chrono::system_clock::time_point toSystemTime(fs::file_time_type t) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
}
} // namespace

Result<void> writeFileAtomic(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp-" + str::randomHex(4);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return Error("io.write", "cannot create " + tmp.string());
    size_t done = 0;
    while (done < content.size()) {
        ssize_t n = ::write(fd, content.data() + done, content.size() - done);
        if (n < 0) {
            ::close(fd);
            fs::remove(tmp, ec);
            return Error("io.write", "write failed for " + path.string());
        }
        done += static_cast<size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return Error("io.write", "cannot replace " + path.string() + ": " + ec.message());
    }
    return success();
}

Result<std::string> readFile(const fs::path& path, uint64_t maxBytes) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return Error("io.not_found", "file not found: " + path.string());
    uint64_t size = fs::file_size(path, ec);
    if (ec) return Error("io.read", "cannot read " + path.string());
    if (size > maxBytes) return Error("io.too_large", path.filename().string() + " is larger than " + str::formatBytes(maxBytes));
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error("io.read", "cannot open " + path.string());
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(size));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

json::Value LibraryEntry::toJson() const {
    json::Array kids;
    for (const auto& c : children) kids.push_back(c.toJson());
    return json::Object{
        {"path", path}, {"name", name}, {"isFolder", isFolder}, {"isProject", isProject}, {"favorite", favorite},
        {"sizeBytes", sizeBytes}, {"modified", str::isoTimestamp(modified)}, {"children", std::move(kids)},
    };
}

ScriptLibrary::ScriptLibrary(fs::path root) : root_(std::move(root)) {}

Result<void> ScriptLibrary::initialize() {
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) return Error("library.init", "cannot create library folder " + root_.string() + ": " + ec.message());
    root_ = fs::weakly_canonical(root_, ec);
    std::lock_guard lock(mutex_);
    loadMetadata();
    return success();
}

bool ScriptLibrary::isScriptFileName(const std::string& name) {
    std::string lower = str::toLower(name);
    return str::endsWith(lower, ".luau") || str::endsWith(lower, ".lua") || str::endsWith(lower, ".txt");
}

Result<void> ScriptLibrary::validateName(const std::string& name) {
    if (name.empty() || str::trim(name).empty()) return Error("library.name", "The name cannot be empty");
    if (name.size() > 200) return Error("library.name", "The name is too long");
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
        return Error("library.name", "Names cannot contain '/', '\\' or ':'");
    if (name == "." || name == ".." || name[0] == '.') return Error("library.name", "Names cannot start with '.'");
    if (name.find('\0') != std::string::npos) return Error("library.name", "Invalid character in name");
    return success();
}

Result<fs::path> ScriptLibrary::resolve(const std::string& relative) const {
    fs::path rel(relative);
    if (rel.is_absolute()) return Error("library.path", "Absolute paths are not allowed: " + relative);
    for (const auto& part : rel) {
        std::string s = part.string();
        if (s == "..") return Error("library.path", "Path escapes the library: " + relative);
        if (!s.empty() && s[0] == '.' && s != ".") return Error("library.path", "Hidden items are not accessible: " + relative);
    }
    fs::path full = (root_ / rel).lexically_normal();
    std::string rootStr = root_.lexically_normal().string();
    std::string fullStr = full.string();
    if (fullStr.compare(0, rootStr.size(), rootStr) != 0) return Error("library.path", "Path escapes the library: " + relative);
    // Reject symlinks that point outside the root.
    std::error_code ec;
    if (fs::exists(full, ec)) {
        fs::path canonical = fs::canonical(full, ec);
        if (!ec && canonical.string().compare(0, rootStr.size(), rootStr) != 0)
            return Error("library.path", "Item links outside the library: " + relative);
    }
    return full;
}

std::string ScriptLibrary::relativeOf(const fs::path& absolute) const {
    return absolute.lexically_relative(root_).generic_string();
}

std::string ScriptLibrary::uniqueName(const fs::path& folder, const std::string& desired) const {
    std::error_code ec;
    if (!fs::exists(folder / desired, ec)) return desired;
    fs::path p(desired);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    for (int i = 2; i < 10000; ++i) {
        std::string candidate = stem + " " + std::to_string(i) + ext;
        if (!fs::exists(folder / candidate, ec)) return candidate;
    }
    return stem + " " + str::randomHex(3) + ext;
}

void ScriptLibrary::loadMetadata() {
    favorites_.clear();
    recent_.clear();
    projects_.clear();
    auto data = readFile(root_ / kMetadataFile, 4 << 20);
    if (!data.ok()) return;
    auto parsed = json::parse(data.value());
    if (!parsed.ok()) return;
    const json::Value& m = parsed.value();
    if (const json::Value* f = m.get("favorites"))
        for (const auto& v : f->asArray()) favorites_.push_back(v.asString());
    if (const json::Value* r = m.get("recent"))
        for (const auto& v : r->asArray()) recent_.emplace_back(v.getString("path"), v.getInt("openedAtMs"));
    if (const json::Value* p = m.get("projects"))
        for (const auto& v : p->asArray()) projects_.push_back(v.asString());
}

void ScriptLibrary::saveMetadata() const {
    json::Array fav, rec, proj;
    for (const auto& f : favorites_) fav.push_back(f);
    for (const auto& [p, t] : recent_) rec.push_back(json::Object{{"path", p}, {"openedAtMs", t}});
    for (const auto& p : projects_) proj.push_back(p);
    json::Value v = json::Object{{"format", "nexus-library-v1"}, {"favorites", std::move(fav)}, {"recent", std::move(rec)}, {"projects", std::move(proj)}};
    (void)writeFileAtomic(root_ / kMetadataFile, json::dump(v, 2) + "\n");
}

void ScriptLibrary::renameInMetadata(const std::string& from, const std::string& to) {
    auto remap = [&](std::string& p) {
        if (p == from) p = to;
        else if (str::startsWith(p, from + "/")) p = to + p.substr(from.size());
    };
    for (auto& f : favorites_) remap(f);
    for (auto& [p, _] : recent_) remap(p);
    for (auto& p : projects_) remap(p);
}

void ScriptLibrary::removeFromMetadata(const std::string& path) {
    auto gone = [&](const std::string& p) { return p == path || str::startsWith(p, path + "/"); };
    favorites_.erase(std::remove_if(favorites_.begin(), favorites_.end(), gone), favorites_.end());
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const auto& r) { return gone(r.first); }), recent_.end());
    projects_.erase(std::remove_if(projects_.begin(), projects_.end(), gone), projects_.end());
}

LibraryEntry ScriptLibrary::buildEntry(const fs::path& p) const {
    LibraryEntry e;
    std::error_code ec;
    e.path = p == root_ ? std::string() : relativeOf(p);
    e.name = p == root_ ? std::string("Library") : p.filename().string();
    e.isFolder = fs::is_directory(p, ec);
    e.modified = toSystemTime(fs::last_write_time(p, ec));
    e.favorite = std::find(favorites_.begin(), favorites_.end(), e.path) != favorites_.end();
    e.isProject = std::find(projects_.begin(), projects_.end(), e.path) != projects_.end();
    if (e.isFolder) {
        std::vector<fs::path> kids;
        for (const auto& child : fs::directory_iterator(p, ec)) {
            std::string name = child.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            if (child.is_directory(ec) || (child.is_regular_file(ec) && isScriptFileName(name))) kids.push_back(child.path());
        }
        std::sort(kids.begin(), kids.end(), [](const fs::path& a, const fs::path& b) {
            std::error_code e1;
            bool da = fs::is_directory(a, e1), db = fs::is_directory(b, e1);
            if (da != db) return da;
            return str::toLower(a.filename().string()) < str::toLower(b.filename().string());
        });
        for (const auto& k : kids) e.children.push_back(buildEntry(k));
    } else {
        e.sizeBytes = fs::file_size(p, ec);
    }
    return e;
}

LibraryEntry ScriptLibrary::tree() const {
    std::lock_guard lock(mutex_);
    return buildEntry(root_);
}

Result<std::string> ScriptLibrary::read(const std::string& path) const {
    auto full = resolve(path);
    if (!full.ok()) return full.error();
    return readFile(full.value(), kMaxScriptBytes);
}

Result<void> ScriptLibrary::write(const std::string& path, const std::string& content) {
    auto full = resolve(path);
    if (!full.ok()) return full.error();
    if (!isScriptFileName(full.value().filename().string())) return Error("library.type", "Only .lua, .luau and .txt files can be saved");
    if (content.size() > kMaxScriptBytes) return Error("library.size", "Script exceeds " + str::formatBytes(kMaxScriptBytes));
    return writeFileAtomic(full.value(), content);
}

Result<std::string> ScriptLibrary::createScript(const std::string& folder, const std::string& name, const std::string& content) {
    std::string fileName = name;
    if (!isScriptFileName(fileName)) fileName += ".luau";
    if (auto v = validateName(fileName); !v.ok()) return v.error();
    auto dir = resolve(folder);
    if (!dir.ok()) return dir.error();
    std::error_code ec;
    if (!fs::is_directory(dir.value(), ec)) return Error("library.not_found", "Folder does not exist: " + folder);
    std::string unique = uniqueName(dir.value(), fileName);
    if (auto w = writeFileAtomic(dir.value() / unique, content); !w.ok()) return w.error();
    return relativeOf(dir.value() / unique);
}

Result<std::string> ScriptLibrary::createFolder(const std::string& parent, const std::string& name, bool project) {
    if (auto v = validateName(name); !v.ok()) return v.error();
    auto dir = resolve(parent);
    if (!dir.ok()) return dir.error();
    std::string unique = uniqueName(dir.value(), name);
    std::error_code ec;
    fs::create_directories(dir.value() / unique, ec);
    if (ec) return Error("library.io", "Cannot create folder: " + ec.message());
    std::string rel = relativeOf(dir.value() / unique);
    if (project) {
        std::lock_guard lock(mutex_);
        projects_.push_back(rel);
        saveMetadata();
    }
    return rel;
}

Result<std::string> ScriptLibrary::rename(const std::string& path, const std::string& newName) {
    if (path.empty()) return Error("library.path", "The library root cannot be renamed");
    if (auto v = validateName(newName); !v.ok()) return v.error();
    auto full = resolve(path);
    if (!full.ok()) return full.error();
    std::error_code ec;
    if (!fs::exists(full.value(), ec)) return Error("library.not_found", "Item not found: " + path);
    std::string name = newName;
    if (fs::is_regular_file(full.value(), ec) && !isScriptFileName(name)) name += full.value().extension().string();
    fs::path target = full.value().parent_path() / name;
    if (fs::exists(target, ec)) return Error("library.exists", "An item named '" + name + "' already exists");
    fs::rename(full.value(), target, ec);
    if (ec) return Error("library.io", "Rename failed: " + ec.message());
    std::string rel = relativeOf(target);
    std::lock_guard lock(mutex_);
    renameInMetadata(path, rel);
    saveMetadata();
    return rel;
}

Result<std::string> ScriptLibrary::duplicate(const std::string& path) {
    auto full = resolve(path);
    if (!full.ok()) return full.error();
    std::error_code ec;
    if (!fs::exists(full.value(), ec) || path.empty()) return Error("library.not_found", "Item not found: " + path);
    fs::path p = full.value();
    std::string desired = p.stem().string() + " copy" + p.extension().string();
    fs::path target = p.parent_path() / uniqueName(p.parent_path(), desired);
    fs::copy(p, target, fs::copy_options::recursive, ec);
    if (ec) return Error("library.io", "Duplicate failed: " + ec.message());
    return relativeOf(target);
}

Result<std::string> ScriptLibrary::move(const std::string& path, const std::string& destinationFolder) {
    auto full = resolve(path);
    auto dest = resolve(destinationFolder);
    if (!full.ok()) return full.error();
    if (!dest.ok()) return dest.error();
    std::error_code ec;
    if (!fs::is_directory(dest.value(), ec)) return Error("library.not_found", "Destination folder not found");
    if (str::startsWith(dest.value().string() + "/", full.value().string() + "/")) return Error("library.path", "Cannot move a folder into itself");
    fs::path target = dest.value() / uniqueName(dest.value(), full.value().filename().string());
    fs::rename(full.value(), target, ec);
    if (ec) return Error("library.io", "Move failed: " + ec.message());
    std::string rel = relativeOf(target);
    std::lock_guard lock(mutex_);
    renameInMetadata(path, rel);
    saveMetadata();
    return rel;
}

Result<std::string> ScriptLibrary::remove(const std::string& path) {
    if (path.empty()) return Error("library.path", "The library root cannot be deleted");
    auto full = resolve(path);
    if (!full.ok()) return full.error();
    std::error_code ec;
    if (!fs::exists(full.value(), ec)) return Error("library.not_found", "Item not found: " + path);
    fs::path trash = root_ / kTrashDir;
    fs::create_directories(trash, ec);
    auto stamp = std::to_string(str::unixMillis(std::chrono::system_clock::now()));
    fs::path target = trash / (stamp + "-" + full.value().filename().string());
    fs::rename(full.value(), target, ec);
    if (ec) return Error("library.io", "Delete failed: " + ec.message());
    std::lock_guard lock(mutex_);
    removeFromMetadata(path);
    saveMetadata();
    return target.string();
}

Result<void> ScriptLibrary::setFavorite(const std::string& path, bool favorite) {
    auto full = resolve(path);
    if (!full.ok()) return full.error();
    std::lock_guard lock(mutex_);
    auto it = std::find(favorites_.begin(), favorites_.end(), path);
    if (favorite && it == favorites_.end()) favorites_.push_back(path);
    if (!favorite && it != favorites_.end()) favorites_.erase(it);
    saveMetadata();
    return success();
}

Result<void> ScriptLibrary::setProject(const std::string& folder, bool project) {
    auto full = resolve(folder);
    if (!full.ok()) return full.error();
    std::error_code ec;
    if (!fs::is_directory(full.value(), ec)) return Error("library.type", "Only folders can be projects");
    std::lock_guard lock(mutex_);
    auto it = std::find(projects_.begin(), projects_.end(), folder);
    if (project && it == projects_.end()) projects_.push_back(folder);
    if (!project && it != projects_.end()) projects_.erase(it);
    saveMetadata();
    return success();
}

void ScriptLibrary::markOpened(const std::string& path) {
    std::lock_guard lock(mutex_);
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const auto& r) { return r.first == path; }), recent_.end());
    recent_.insert(recent_.begin(), {path, str::unixMillis(std::chrono::system_clock::now())});
    if (recent_.size() > 50) recent_.resize(50);
    saveMetadata();
}

std::vector<std::string> ScriptLibrary::favorites() const {
    std::lock_guard lock(mutex_);
    return favorites_;
}

std::vector<std::string> ScriptLibrary::recent(size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& [p, _] : recent_) {
        if (out.size() >= limit) break;
        if (fs::exists(root_ / p, ec)) out.push_back(p);
    }
    return out;
}

std::vector<std::string> ScriptLibrary::projects() const {
    std::lock_guard lock(mutex_);
    return projects_;
}

std::vector<LibrarySearchHit> ScriptLibrary::search(const std::string& query, size_t limit) const {
    std::vector<LibrarySearchHit> hits;
    if (str::trim(query).empty()) return hits;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && hits.size() < limit; it.increment(ec)) {
        if (ec) break;
        std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        std::string rel = relativeOf(it->path());
        if (str::containsIgnoreCase(name, query)) {
            hits.push_back({rel, true, 0, name});
            continue;
        }
        if (!it->is_regular_file(ec) || !isScriptFileName(name)) continue;
        auto content = readFile(it->path(), kMaxScriptBytes);
        if (!content.ok()) continue;
        uint32_t lineNo = 0;
        for (std::string_view line : str::splitLines(content.value())) {
            ++lineNo;
            if (str::containsIgnoreCase(line, query)) {
                hits.push_back({rel, false, lineNo, std::string(str::trim(line)).substr(0, 160)});
                break;
            }
        }
    }
    return hits;
}

Result<std::string> ScriptLibrary::importFile(const fs::path& external, const std::string& folder) {
    std::string name = external.filename().string();
    if (!isScriptFileName(name)) return Error("library.type", "Only .lua, .luau and .txt files can be imported");
    auto content = readFile(external, kMaxScriptBytes);
    if (!content.ok()) return content.error();
    // Preserve the original bytes exactly; invalid UTF-8 is allowed (the editor shows a warning).
    auto dir = resolve(folder);
    if (!dir.ok()) return dir.error();
    std::string safeName = name;
    if (!safeName.empty() && safeName[0] == '.') safeName = "imported" + safeName;
    std::string unique = uniqueName(dir.value(), safeName);
    if (auto w = writeFileAtomic(dir.value() / unique, content.value()); !w.ok()) return w.error();
    return relativeOf(dir.value() / unique);
}

Result<void> ScriptLibrary::exportItem(const std::string& path, const fs::path& destination) const {
    auto full = resolve(path);
    if (!full.ok()) return full.error();
    std::error_code ec;
    if (fs::is_directory(full.value(), ec)) {
        fs::copy(full.value(), destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) return Error("library.export", "Export failed: " + ec.message());
        return success();
    }
    auto content = readFile(full.value(), kMaxScriptBytes);
    if (!content.ok()) return content.error();
    return writeFileAtomic(destination, content.value());
}

} // namespace nexus
