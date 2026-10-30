#pragma once
// Roblox Studio integration through Studio's built-in, officially documented MCP server
// (https://create.roblox.com/docs/studio/mcp). This is the authorized way for an external tool to
// execute Luau with the full Roblox engine API (Edit, Server or Client data model).
//
// Consent and safety:
//   * nothing is launched until the user explicitly connects;
//   * before launch, StudioMCP's code signature is validated with Security.framework and must be
//     issued to Roblox's Apple team (configurable, on by default);
//   * the tool schema advertised by the server is inspected; if Nexus cannot map its parameters
//     unambiguously (e.g. after a Studio update changed them) the integration reports UNSUPPORTED
//     instead of guessing.

#include "nexus/diagnostics/logger.hpp"
#include "nexus/execution/target.hpp"
#include "nexus/roblox/client.hpp"
#include "nexus/roblox/connection.hpp"
#include "nexus/studio/mcp_client.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nexus::studio {

struct StudioInstance {
    std::string id;
    std::string name;
    std::string placeId;
    json::Value raw;
};

// How Nexus maps its execution request onto the server's execution tool.
struct ToolMapping {
    std::string tool;           // e.g. execute_luau (built-in server) or run_code (legacy server)
    std::string codeParam;      // parameter receiving the source
    std::string dataModelParam; // optional
    std::vector<std::string> dataModelValues; // enum values from the schema, if any
    std::string studioIdParam;  // optional
    bool studioIdRequired = false;
    std::vector<std::string> unmappedRequired; // required parameters Nexus cannot fill

    bool usable() const { return !tool.empty() && !codeParam.empty() && unmappedRequired.empty(); }
    json::Value toJson() const;
};

// Derives a ToolMapping from the advertised tools (exposed for tests).
ToolMapping mapExecutionTool(const std::vector<McpTool>& tools);
// Extracts Studio instances from a list_roblox_studios result (JSON text or structured content).
std::vector<StudioInstance> parseStudioList(const McpToolResult& result);

struct StudioStatus {
    roblox::ConnectionState state = roblox::ConnectionState::Disconnected;
    std::optional<roblox::InstalledClient> installed;
    std::vector<roblox::RunningClient> running;
    std::string mcpPath;
    bool usingOverride = false;
    platform::SignatureCheck mcpSignature;
    std::string serverName, serverVersion, protocolVersion;
    std::vector<std::string> tools;
    ToolMapping mapping;
    std::vector<StudioInstance> studios;
    std::string selectedStudioId;
    std::string message;
    std::optional<std::string> previousVersion;
    uint64_t generation = 0;
    std::vector<roblox::StateTransition> history;

    json::Value toJson() const;
};

struct StudioOptions {
    bool requireRobloxSignature = true;
    std::string dataModel = "Edit";
    // Overrides the MCP server executable (tests use a mock server; never set by the UI).
    std::string serverOverride;
    std::vector<std::string> serverOverrideArgs;
};

class StudioIntegration final : public ExecutionTarget {
public:
    static constexpr const char* kId = "roblox-studio";

    StudioIntegration(Logger& logger, roblox::ClientEnvironment& env, roblox::VersionHistory& versions);
    ~StudioIntegration() override;

    void setOptions(StudioOptions options);
    StudioOptions options() const;

    StudioStatus refresh();    // discovery only; never launches anything
    StudioStatus connect();    // explicit user action: launch StudioMCP and validate it
    void disconnect(const std::string& reason = "Disconnected by user");
    StudioStatus status() const;
    void selectStudio(const std::string& studioId);

    // Called when the connection is lost; receives the reason.
    void setDisconnectHandler(std::function<void(const std::string&)> handler);

    TargetInfo info() const override;
    TargetAvailability availability() const override;
    ExecutionResult execute(const ExecutionRequest& request, const OutputCallback& output, const CancellationToken& cancel) override;
    uint64_t generation() const override { return generation_.load(); }

private:
    void discoverLocked(StudioStatus& st);
    void transition(roblox::ConnectionState to, const std::string& reason);
    void onServerExit(const std::string& reason);

    Logger& logger_;
    roblox::ClientEnvironment& env_;
    roblox::VersionHistory& versions_;
    mutable std::mutex mutex_;
    StudioOptions options_;
    roblox::ConnectionStateMachine machine_{true};
    StudioStatus status_;
    std::shared_ptr<McpClient> client_; // shared so an in-flight execute() keeps it alive
    std::atomic<uint64_t> generation_{0};
    std::mutex handlerMutex_;
    std::function<void(const std::string&)> disconnectHandler_;
};

} // namespace nexus::studio
