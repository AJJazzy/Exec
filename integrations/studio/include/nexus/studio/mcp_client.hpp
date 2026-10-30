#pragma once
// Minimal Model Context Protocol client (JSON-RPC 2.0 over newline-delimited stdio), used to talk
// to Roblox Studio's built-in MCP server (/Applications/RobloxStudio.app/Contents/MacOS/StudioMCP).
// Only the client side of `initialize`, `tools/list`, `tools/call`, `ping` and cancellation is
// implemented; server-initiated requests other than `ping` are answered with "method not found".

#include "nexus/common/cancellation.hpp"
#include "nexus/common/json.hpp"
#include "nexus/common/result.hpp"
#include "nexus/diagnostics/logger.hpp"
#include "nexus/platform/subprocess.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nexus::studio {

constexpr const char* kMcpProtocolVersion = "2025-06-18";

struct McpTool {
    std::string name;
    std::string description;
    json::Value inputSchema;
};

struct McpToolResult {
    bool isError = false;
    std::string text;  // concatenated text content
    json::Value raw;   // full result object
};

class McpClient {
public:
    struct Options {
        std::string executable;
        std::vector<std::string> arguments;
        std::chrono::milliseconds initializeTimeout{15000};
    };

    explicit McpClient(Logger& logger);
    ~McpClient();
    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    // Launches the server and performs the initialize handshake.
    Result<json::Value> start(const Options& options);
    void stop();
    bool running() const;

    Result<json::Value> request(const std::string& method, json::Value params, std::chrono::milliseconds timeout,
                                const CancellationToken& cancel = {});
    void notify(const std::string& method, json::Value params);

    Result<std::vector<McpTool>> listTools(std::chrono::milliseconds timeout = std::chrono::seconds(15));
    Result<McpToolResult> callTool(const std::string& name, json::Object arguments, std::chrono::milliseconds timeout,
                                   const CancellationToken& cancel = {});

    const std::string& serverName() const { return serverName_; }
    const std::string& serverVersion() const { return serverVersion_; }
    const std::string& protocolVersion() const { return protocolVersion_; }

    // Invoked (from the reader thread) when the server process exits unexpectedly.
    void setExitHandler(std::function<void(const std::string&)> handler);

private:
    struct Pending {
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<json::Value> response;
    };

    void readerLoop();
    void handleMessage(const json::Value& message);
    bool send(const json::Value& message);
    void failAllPending(const std::string& reason);

    Logger& logger_;
    std::optional<platform::Subprocess> process_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    std::mutex writeMutex_;
    std::mutex pendingMutex_;
    std::map<int64_t, std::shared_ptr<Pending>> pending_;
    std::atomic<int64_t> nextId_{1};

    std::mutex handlerMutex_;
    std::function<void(const std::string&)> exitHandler_;

    std::string serverName_;
    std::string serverVersion_;
    std::string protocolVersion_;
};

} // namespace nexus::studio
