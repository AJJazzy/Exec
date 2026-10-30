#include "nexus/library/draft_store.hpp"

#include "nexus/common/json.hpp"
#include "nexus/common/strings.hpp"
#include "nexus/library/script_library.hpp"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace nexus {

DraftStore::DraftStore(fs::path directory) : dir_(std::move(directory)) {}

Result<fs::path> DraftStore::pathFor(const std::string& id) const {
    if (id.empty() || id.size() > 128) return Error("draft.id", "invalid draft id");
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return Error("draft.id", "invalid draft id");
    }
    return dir_ / (id + ".json");
}

Result<void> DraftStore::save(const Draft& draft) {
    auto p = pathFor(draft.id);
    if (!p.ok()) return p.error();
    if (draft.content.size() > ScriptLibrary::kMaxScriptBytes) return Error("draft.size", "draft too large");
    json::Value v = json::Object{
        {"format", "nexus-draft-v1"},
        {"id", draft.id},
        {"title", draft.title},
        {"libraryPath", draft.libraryPath},
        {"savedAtMs", str::unixMillis(draft.savedAt.time_since_epoch().count() ? draft.savedAt : std::chrono::system_clock::now())},
        // Content is stored as a JSON string; bytes that are not valid UTF-8 are replaced.
        {"content", str::sanitizeUtf8(draft.content)},
    };
    return writeFileAtomic(p.value(), json::dump(v) + "\n");
}

std::vector<Draft> DraftStore::list() const {
    std::vector<Draft> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() != ".json") continue;
        auto data = readFile(entry.path(), ScriptLibrary::kMaxScriptBytes * 2);
        if (!data.ok()) continue;
        auto v = json::parse(data.value());
        if (!v.ok() || v.value().getString("format") != "nexus-draft-v1") continue;
        Draft d;
        d.id = v.value().getString("id");
        d.title = v.value().getString("title");
        d.libraryPath = v.value().getString("libraryPath");
        d.content = v.value().getString("content");
        d.savedAt = str::fromUnixMillis(v.value().getInt("savedAtMs"));
        if (!d.id.empty()) out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const Draft& a, const Draft& b) { return a.savedAt > b.savedAt; });
    return out;
}

Result<void> DraftStore::remove(const std::string& id) {
    auto p = pathFor(id);
    if (!p.ok()) return p.error();
    std::error_code ec;
    fs::remove(p.value(), ec);
    return success();
}

void DraftStore::clear() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() == ".json") fs::remove(entry.path(), ec);
    }
}

} // namespace nexus
