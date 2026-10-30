#include "nexus/settings/settings.hpp"

#include "nexus/library/script_library.hpp"

#include <algorithm>
#include <cctype>

namespace nexus {

namespace {

template <class T>
T clampValue(T v, T lo, T hi) {
    return std::min(std::max(v, lo), hi);
}

bool isHexColor(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    return std::all_of(s.begin() + 1, s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string oneOf(const std::string& value, std::initializer_list<const char*> allowed, const char* fallback) {
    for (const char* a : allowed)
        if (value == a) return value;
    return fallback;
}

} // namespace

ExecutionLimits Settings::limits() const {
    ExecutionLimits l;
    l.timeout = std::chrono::milliseconds(timeoutMs);
    l.memoryLimitBytes = memoryLimitMB << 20;
    l.maxOutputBytes = maxOutputKB << 10;
    return l;
}

json::Value Settings::toJson() const {
    return json::Object{
        {"format", "nexus-settings-v1"},
        {"appearance", appearance},
        {"accentColor", accentColor},
        {"editorFontSize", editorFontSize},
        {"editorFontName", editorFontName},
        {"indentWidth", indentWidth},
        {"useTabs", useTabs},
        {"autoCloseBrackets", autoCloseBrackets},
        {"autoIndent", autoIndent},
        {"highlightMatchingBrackets", highlightMatchingBrackets},
        {"liveDiagnostics", liveDiagnostics},
        {"autocomplete", autocomplete},
        {"showMinimap", showMinimap},
        {"restoreDrafts", restoreDrafts},
        {"defaultTarget", defaultTarget},
        {"timeoutMs", timeoutMs},
        {"memoryLimitMB", memoryLimitMB},
        {"maxOutputKB", maxOutputKB},
        {"runnerSandbox", runnerSandbox},
        {"studioDataModel", studioDataModel},
        {"allowRobloxVersionCheck", allowRobloxVersionCheck},
        {"studioIntegrationEnabled", studioIntegrationEnabled},
        {"requireRobloxSignature", requireRobloxSignature},
        {"consoleCapacity", consoleCapacity},
    };
}

Settings Settings::fromJson(const json::Value& v) {
    Settings d;
    Settings s;
    s.appearance = oneOf(v.getString("appearance", d.appearance), {"system", "light", "dark"}, "system");
    s.accentColor = v.getString("accentColor", d.accentColor);
    if (!isHexColor(s.accentColor)) s.accentColor = d.accentColor;
    s.editorFontSize = clampValue(v.getNumber("editorFontSize", d.editorFontSize), 8.0, 40.0);
    s.editorFontName = v.getString("editorFontName", d.editorFontName);
    if (s.editorFontName.empty() || s.editorFontName.size() > 100) s.editorFontName = d.editorFontName;
    s.indentWidth = static_cast<int>(clampValue<int64_t>(v.getInt("indentWidth", d.indentWidth), 1, 8));
    s.useTabs = v.getBool("useTabs", d.useTabs);
    s.autoCloseBrackets = v.getBool("autoCloseBrackets", d.autoCloseBrackets);
    s.autoIndent = v.getBool("autoIndent", d.autoIndent);
    s.highlightMatchingBrackets = v.getBool("highlightMatchingBrackets", d.highlightMatchingBrackets);
    s.liveDiagnostics = v.getBool("liveDiagnostics", d.liveDiagnostics);
    s.autocomplete = v.getBool("autocomplete", d.autocomplete);
    s.showMinimap = v.getBool("showMinimap", d.showMinimap);
    s.restoreDrafts = v.getBool("restoreDrafts", d.restoreDrafts);
    s.defaultTarget = oneOf(v.getString("defaultTarget", d.defaultTarget), {"luau-isolated", "luau-inprocess", "roblox-studio"}, "luau-isolated");
    s.timeoutMs = clampValue<int64_t>(v.getInt("timeoutMs", d.timeoutMs), 100, 3600 * 1000);
    s.memoryLimitMB = static_cast<uint64_t>(clampValue<int64_t>(v.getInt("memoryLimitMB", static_cast<int64_t>(d.memoryLimitMB)), 8, 8192));
    s.maxOutputKB = static_cast<uint64_t>(clampValue<int64_t>(v.getInt("maxOutputKB", static_cast<int64_t>(d.maxOutputKB)), 16, 262144));
    s.runnerSandbox = v.getBool("runnerSandbox", d.runnerSandbox);
    s.studioDataModel = oneOf(v.getString("studioDataModel", d.studioDataModel), {"Edit", "Server", "Client"}, "Edit");
    s.allowRobloxVersionCheck = v.getBool("allowRobloxVersionCheck", d.allowRobloxVersionCheck);
    s.studioIntegrationEnabled = v.getBool("studioIntegrationEnabled", d.studioIntegrationEnabled);
    s.requireRobloxSignature = v.getBool("requireRobloxSignature", d.requireRobloxSignature);
    s.consoleCapacity = static_cast<int>(clampValue<int64_t>(v.getInt("consoleCapacity", d.consoleCapacity), 1000, 200000));
    return s;
}

SettingsStore::SettingsStore(std::filesystem::path file) : file_(std::move(file)) {}

Settings SettingsStore::load() const {
    auto data = readFile(file_, 1 << 20);
    if (!data.ok()) return Settings{};
    auto parsed = json::parse(data.value());
    if (!parsed.ok()) return Settings{};
    return Settings::fromJson(parsed.value());
}

Result<void> SettingsStore::save(const Settings& settings) const {
    return writeFileAtomic(file_, json::dump(settings.toJson(), 2) + "\n");
}

} // namespace nexus
