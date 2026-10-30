#pragma once
// User preferences, persisted as JSON. Every value is validated and clamped when loaded, so a
// hand-edited or corrupt settings file can never produce unsafe limits.

#include "nexus/common/json.hpp"
#include "nexus/common/result.hpp"
#include "nexus/runtime/types.hpp"

#include <filesystem>
#include <string>

namespace nexus {

struct Settings {
    // Appearance
    std::string appearance = "system"; // system | light | dark
    std::string accentColor = "#7C5CFF";
    double editorFontSize = 13.0;
    std::string editorFontName = "SF Mono";

    // Editor behaviour
    int indentWidth = 4;
    bool useTabs = false;
    bool autoCloseBrackets = true;
    bool autoIndent = true;
    bool highlightMatchingBrackets = true;
    bool liveDiagnostics = true;
    bool autocomplete = true;
    bool showMinimap = true;
    bool restoreDrafts = true;

    // Execution
    std::string defaultTarget = "luau-isolated";
    int64_t timeoutMs = 30000;
    uint64_t memoryLimitMB = 256;
    uint64_t maxOutputKB = 4096;
    bool runnerSandbox = true;
    std::string studioDataModel = "Edit"; // Edit | Server | Client

    // Integrations and privacy. Everything that touches the network or another program is opt-in.
    bool allowRobloxVersionCheck = false;   // one HTTPS request to Roblox's public client-version endpoint
    bool studioIntegrationEnabled = false;  // launch Roblox Studio's MCP server when connecting
    bool requireRobloxSignature = true;     // only launch StudioMCP if signed by Roblox's Apple team

    // Console
    int consoleCapacity = 20000;

    ExecutionLimits limits() const;
    json::Value toJson() const;
    static Settings fromJson(const json::Value& v); // validates and clamps
};

class SettingsStore {
public:
    explicit SettingsStore(std::filesystem::path file);
    Settings load() const; // defaults when missing or unreadable
    Result<void> save(const Settings& settings) const;
    const std::filesystem::path& path() const { return file_; }

private:
    std::filesystem::path file_;
};

} // namespace nexus
