#include "nexus/studio/mcp_client.hpp"

#include "nexus/common/strings.hpp"
#include "nexus/execution/runner_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>

#ifndef NEXUS_VERSION
#define NEXUS_VERSION "dev"
#endif

namespace nexus::studio {

McpClient::McpClient(Logger& logger) : logger_(logger) {}

McpClient::~McpClient() { stop(); }

void McpClient::setExitHandler(std::function<void(const std::string&)> handler) {
    std::lock_guard lock(handlerMutex_);
    exitHandler_ = std::move(handler);
}

bool McpClient::running() const { return running_.load(); }

Result<json::Value> McpClient::start(const Options& options) {
    stop();
    stopping_ = false;
    platform::SpawnOptions spawn;
    spawn.executable = options.executable;
    spawn.arguments = options.arguments;
    spawn.inheritEnvironment = true; // the Studio MCP server may rely on the user's environment
    std::string error;
    auto proc = platform::Subprocess::spawn(spawn, &error);
    if (!proc) return Error("mcp.spawn", "Could not start " + options.executable + ": " + error);
    process_ = std::move(proc);
    running_ = true;
    reader_ = std::thread([this] { readerLoop(); });
    logger_.info("studio", "Started MCP server " + options.executable + " (pid " + std::to_string(process_->pid()) + ")");

    json::Value params = json::Object{
        {"protocolVersion", kMcpProtocolVersion},
        {"capabilities", json::Object{}},
        {"clientInfo", json::Object{{"name", "Nexus"}, {"version", NEXUS_VERSION}}},
    };
    auto init = request("initialize", std::move(params), options.initializeTimeout);
    if (!init.ok()) {
        stop();
        return init.error();
    }
    const json::Value& r = init.value();
    protocolVersion_ = r.getString("protocolVersion");
    if (const json::Value* info = r.get("serverInfo")) {
        serverName_ = info->getString("name");
        serverVersion_ = info->getString("version");
    }
    notify("notifications/initialized", json::Object{});
    logger_.info("studio", "MCP session initialised", {{"server", serverName_}, {"version", serverVersion_}, {"protocol", protocolVersion_}});
    return init;
}

void McpClient::stop() {
    if (!process_) return;
    stopping_ = true;
    {
        std::lock_guard lock(writeMutex_);
        process_->closeStdin(); // polite shutdown for stdio servers
    }
    if (!process_->waitFor(std::chrono::milliseconds(500))) {
        process_->terminate();
        if (!process_->waitFor(std::chrono::milliseconds(1000))) process_->kill();
    }
    if (reader_.joinable()) reader_.join();
    {
        std::lock_guard lock(writeMutex_);
        running_ = false;
        process_.reset();
    }
    failAllPending("MCP server stopped");
}

bool McpClient::send(const json::Value& message) {
    std::lock_guard lock(writeMutex_);
    if (!process_ || !running_) return false;
    return process_->writeStdin(json::dump(message) + "\n");
}

void McpClient::notify(const std::string& method, json::Value params) {
    send(json::Object{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

Result<json::Value> McpClient::request(const std::string& method, json::Value params, std::chrono::milliseconds timeout,
                                       const CancellationToken& cancel) {
    if (!running_) return Error("mcp.not_running", "The MCP server is not running");
    int64_t id = nextId_++;
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard lock(pendingMutex_);
        pending_[id] = pending;
    }
    // Wake the waiter when cancellation is requested.
    cancel.onCancel([pending] { pending->cv.notify_all(); });

    if (!send(json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}})) {
        std::lock_guard lock(pendingMutex_);
        pending_.erase(id);
        return Error("mcp.io", "Could not write to the MCP server");
    }

    std::optional<json::Value> response;
    bool cancelled = false;
    {
        std::unique_lock lock(pending->mutex);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pending->response) {
            if (cancel.requested()) {
                cancelled = true;
                break;
            }
            if (pending->cv.wait_until(lock, deadline) == std::cv_status::timeout && !pending->response) break;
        }
        response = pending->response;
    }
    {
        std::lock_guard lock(pendingMutex_);
        pending_.erase(id);
    }
    if (!response) {
        notify("notifications/cancelled", json::Object{{"requestId", id}, {"reason", cancelled ? "cancelled by user" : "timed out"}});
        if (cancelled) return Error("mcp.cancelled", method + " was cancelled");
        if (!running_) return Error("mcp.disconnected", "The MCP server exited while handling " + method);
        return Error("mcp.timeout", method + " timed out after " + str::formatDuration(static_cast<double>(timeout.count())));
    }
    if (const json::Value* err = response->get("error")) {
        std::string code = err->get("code") ? std::to_string(err->getInt("code")) : "?";
        if (err->getString("message") == "__nexus_disconnected__") return Error("mcp.disconnected", "The MCP server exited");
        return Error("mcp.error", "MCP error " + code + ": " + err->getString("message"));
    }
    const json::Value* result = response->get("result");
    return result ? *result : json::Value(json::Object{});
}

Result<std::vector<McpTool>> McpClient::listTools(std::chrono::milliseconds timeout) {
    std::vector<McpTool> tools;
    std::string cursor;
    for (int page = 0; page < 20; ++page) {
        json::Object params;
        if (!cursor.empty()) params["cursor"] = cursor;
        auto r = request("tools/list", std::move(params), timeout);
        if (!r.ok()) return r.error();
        if (const json::Value* list = r.value().get("tools")) {
            for (const auto& t : list->asArray()) {
                McpTool tool;
                tool.name = t.getString("name");
                tool.description = t.getString("description");
                if (const json::Value* s = t.get("inputSchema")) tool.inputSchema = *s;
                if (!tool.name.empty()) tools.push_back(std::move(tool));
            }
        }
        cursor = r.value().getString("nextCursor");
        if (cursor.empty()) break;
    }
    return tools;
}

Result<McpToolResult> McpClient::callTool(const std::string& name, json::Object arguments, std::chrono::milliseconds timeout,
                                          const CancellationToken& cancel) {
    auto r = request("tools/call", json::Object{{"name", name}, {"arguments", std::move(arguments)}}, timeout, cancel);
    if (!r.ok()) return r.error();
    McpToolResult out;
    out.raw = r.value();
    out.isError = r.value().getBool("isError");
    if (const json::Value* content = r.value().get("content")) {
        for (const auto& item : content->asArray()) {
            if (item.getString("type") == "text") {
                if (!out.text.empty()) out.text += "\n";
                out.text += item.getString("text");
            }
        }
    }
    if (out.text.empty()) {
        if (const json::Value* sc = r.value().get("structuredContent")) out.text = json::dump(*sc, 2);
    }
    return out;
}

void McpClient::handleMessage(const json::Value& message) {
    const json::Value* id = message.get("id");
    if (message.get("method")) {
        std::string method = message.getString("method");
        if (id) {
            // Server -> client request.
            if (method == "ping") send(json::Object{{"jsonrpc", "2.0"}, {"id", *id}, {"result", json::Object{}}});
            else
                send(json::Object{{"jsonrpc", "2.0"}, {"id", *id},
                                  {"error", json::Object{{"code", -32601}, {"message", "Method not supported by Nexus: " + method}}}});
        } else if (method == "notifications/message") {
            const json::Value* p = message.get("params");
            logger_.info("studio", "Studio MCP: " + (p ? json::dump(p->get("data") ? *p->get("data") : *p) : std::string()));
        }
        return;
    }
    if (!id || !id->isNumber()) return;
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard lock(pendingMutex_);
        auto it = pending_.find(id->asInt());
        if (it != pending_.end()) pending = it->second;
    }
    if (!pending) return; // late reply to a cancelled/timed-out request
    {
        std::lock_guard lock(pending->mutex);
        pending->response = message;
    }
    pending->cv.notify_all();
}

void McpClient::failAllPending(const std::string& reason) {
    std::map<int64_t, std::shared_ptr<Pending>> pending;
    {
        std::lock_guard lock(pendingMutex_);
        pending.swap(pending_);
    }
    for (auto& [id, p] : pending) {
        {
            std::lock_guard lock(p->mutex);
            p->response = json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", json::Object{{"code", -32000}, {"message", "__nexus_disconnected__"}}}};
        }
        p->cv.notify_all();
    }
    (void)reason;
}

void McpClient::readerLoop() {
    runner::LineBuffer lines;
    std::string stderrTail;
    int outFd = process_->stdoutFd();
    int errFd = process_->stderrFd();
    bool outOpen = true, errOpen = true;
    while (outOpen) {
        pollfd fds[2] = {{outFd, POLLIN, 0}, {errOpen ? errFd : -1, POLLIN, 0}};
        int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (errOpen && (fds[1].revents & (POLLIN | POLLHUP))) {
            std::string chunk;
            if (!platform::readAvailable(errFd, chunk)) errOpen = false;
            stderrTail = (stderrTail + chunk);
            if (stderrTail.size() > 8192) stderrTail.erase(0, stderrTail.size() - 8192);
            for (std::string_view l : str::splitLines(chunk))
                if (!str::trim(l).empty()) logger_.debug("studio", "StudioMCP stderr: " + std::string(l));
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            std::string chunk;
            if (!platform::readAvailable(outFd, chunk)) outOpen = false;
            lines.append(chunk);
            std::string line;
            while (lines.next(line)) {
                if (str::trim(line).empty()) continue;
                auto msg = json::parse(line);
                if (!msg.ok()) {
                    logger_.warning("studio", "Ignoring malformed MCP message: " + msg.error().message);
                    continue;
                }
                handleMessage(msg.value());
            }
            if (lines.pendingBytes() > runner::kMaxMessageBytes) {
                logger_.error("studio", "MCP server sent an oversized message; disconnecting");
                break;
            }
        }
    }
    running_ = false;
    failAllPending("MCP server exited");
    if (!stopping_) {
        std::string reason = "The Studio MCP server exited unexpectedly";
        if (!str::trim(stderrTail).empty()) reason += ": " + std::string(str::trim(stderrTail)).substr(0, 300);
        logger_.warning("studio", reason);
        std::function<void(const std::string&)> handler;
        {
            std::lock_guard lock(handlerMutex_);
            handler = exitHandler_;
        }
        if (handler) handler(reason);
    }
}

} // namespace nexus::studio
