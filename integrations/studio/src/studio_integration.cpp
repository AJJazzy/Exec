#include "nexus/studio/studio_integration.hpp"

#include "nexus/common/strings.hpp"

#include <algorithm>
#include <filesystem>

namespace nexus::studio {

using roblox::ConnectionState;

namespace {

const json::Value* findPropertyCaseInsensitive(const json::Object& props, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        for (const auto& [k, v] : props)
            if (str::toLower(k) == n) return &v;
    }
    return nullptr;
}

std::string keyOf(const json::Object& props, const json::Value* value) {
    for (const auto& [k, v] : props)
        if (&v == value) return k;
    return {};
}

std::string firstString(const json::Value& v, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (const json::Value* x = v.get(k)) {
            if (x->isString()) return x->asString();
            if (x->isNumber()) return std::to_string(x->asInt());
        }
    }
    return {};
}

} // namespace

json::Value ToolMapping::toJson() const {
    json::Array values, unmapped;
    for (const auto& v : dataModelValues) values.push_back(v);
    for (const auto& u : unmappedRequired) unmapped.push_back(u);
    return json::Object{
        {"tool", tool}, {"codeParam", codeParam}, {"dataModelParam", dataModelParam}, {"dataModelValues", std::move(values)},
        {"studioIdParam", studioIdParam}, {"studioIdRequired", studioIdRequired}, {"unmappedRequired", std::move(unmapped)},
        {"usable", usable()},
    };
}

ToolMapping mapExecutionTool(const std::vector<McpTool>& tools) {
    ToolMapping m;
    const McpTool* tool = nullptr;
    for (const char* preferred : {"execute_luau", "run_code"}) {
        auto it = std::find_if(tools.begin(), tools.end(), [&](const McpTool& t) { return t.name == preferred; });
        if (it != tools.end()) {
            tool = &*it;
            break;
        }
    }
    if (!tool) return m;
    m.tool = tool->name;

    const json::Value* propsValue = tool->inputSchema.get("properties");
    json::Object props = propsValue ? propsValue->asObject() : json::Object{};
    std::vector<std::string> required;
    if (const json::Value* r = tool->inputSchema.get("required"))
        for (const auto& x : r->asArray()) required.push_back(x.asString());

    if (const json::Value* code = findPropertyCaseInsensitive(props, {"code", "luau", "luau_code", "source", "script", "command"}))
        m.codeParam = keyOf(props, code);
    for (const auto& [k, v] : props) {
        std::string lower = str::toLower(k);
        if (m.dataModelParam.empty() && (lower.find("datamodel") != std::string::npos || lower.find("data_model") != std::string::npos)) {
            m.dataModelParam = k;
            if (const json::Value* e = v.get("enum"))
                for (const auto& x : e->asArray()) m.dataModelValues.push_back(x.asString());
        }
        if (m.studioIdParam.empty() && lower.find("studio") != std::string::npos && lower.find("id") != std::string::npos) m.studioIdParam = k;
    }
    if (m.codeParam.empty()) {
        // Fall back to the single remaining string parameter, if there is exactly one.
        std::vector<std::string> candidates;
        for (const auto& [k, v] : props)
            if (k != m.dataModelParam && k != m.studioIdParam && v.getString("type", "string") == "string") candidates.push_back(k);
        if (candidates.size() == 1) m.codeParam = candidates.front();
    }
    for (const auto& r : required) {
        if (r == m.codeParam || r == m.dataModelParam) continue;
        if (r == m.studioIdParam) {
            m.studioIdRequired = true;
            continue;
        }
        m.unmappedRequired.push_back(r);
    }
    if (m.codeParam.empty()) m.unmappedRequired.push_back("<code parameter>");
    return m;
}

std::vector<StudioInstance> parseStudioList(const McpToolResult& result) {
    std::vector<StudioInstance> out;
    json::Value data;
    if (const json::Value* sc = result.raw.get("structuredContent")) data = *sc;
    else {
        auto parsed = json::parse(str::trim(result.text));
        if (!parsed.ok()) return out;
        data = parsed.value();
    }
    const json::Array* list = nullptr;
    if (data.isArray()) list = &data.asArray();
    else {
        for (const char* key : {"studios", "instances", "result", "items"})
            if (const json::Value* v = data.get(key); v && v->isArray()) {
                list = &v->asArray();
                break;
            }
    }
    if (!list) return out;
    for (const auto& item : *list) {
        StudioInstance s;
        s.id = firstString(item, {"studio_id", "studioId", "id"});
        s.name = firstString(item, {"name", "place_name", "placeName", "title"});
        s.placeId = firstString(item, {"place_id", "placeId"});
        s.raw = item;
        if (!s.id.empty()) out.push_back(std::move(s));
    }
    return out;
}

json::Value StudioStatus::toJson() const {
    json::Array run, tl, st, hist;
    for (const auto& r : running) run.push_back(r.toJson());
    for (const auto& t : tools) tl.push_back(t);
    for (const auto& s : studios) st.push_back(json::Object{{"id", s.id}, {"name", s.name}, {"placeId", s.placeId}});
    for (const auto& h : history) hist.push_back(h.toJson());
    json::Object o{
        {"state", roblox::toString(state)},
        {"running", std::move(run)},
        {"mcpPath", mcpPath},
        {"usingOverride", usingOverride},
        {"mcpSignature", json::Object{{"supported", mcpSignature.supported}, {"valid", mcpSignature.valid},
                                      {"teamIdentifier", mcpSignature.teamIdentifier}, {"error", mcpSignature.error}}},
        {"serverName", serverName},
        {"serverVersion", serverVersion},
        {"protocolVersion", protocolVersion},
        {"tools", std::move(tl)},
        {"mapping", mapping.toJson()},
        {"studios", std::move(st)},
        {"selectedStudioId", selectedStudioId},
        {"message", message},
        {"generation", generation},
        {"history", std::move(hist)},
    };
    if (installed) o["installed"] = installed->toJson();
    if (previousVersion) o["previousVersion"] = *previousVersion;
    return o;
}

StudioIntegration::StudioIntegration(Logger& logger, roblox::ClientEnvironment& env, roblox::VersionHistory& versions)
    : logger_(logger), env_(env), versions_(versions) {
    status_.message = "Not checked yet";
}

StudioIntegration::~StudioIntegration() {
    std::shared_ptr<McpClient> client;
    {
        std::lock_guard lock(mutex_);
        client = std::move(client_);
    }
    if (client) client->stop();
}

void StudioIntegration::setOptions(StudioOptions options) {
    std::lock_guard lock(mutex_);
    options_ = std::move(options);
}

StudioOptions StudioIntegration::options() const {
    std::lock_guard lock(mutex_);
    return options_;
}

void StudioIntegration::setDisconnectHandler(std::function<void(const std::string&)> handler) {
    std::lock_guard lock(handlerMutex_);
    disconnectHandler_ = std::move(handler);
}

void StudioIntegration::transition(ConnectionState to, const std::string& reason) {
    if (!machine_.transition(to, reason)) {
        // Re-entering discovery is always legal; anything still illegal is a programming error and
        // is logged rather than silently reported as a different state.
        machine_.transition(ConnectionState::Discovering, "Re-evaluating");
        if (!machine_.transition(to, reason))
            logger_.error("studio", std::string("Illegal connection state transition to ") + roblox::toString(to) + " (" + reason + ")");
    }
    status_.state = machine_.state();
    status_.history = machine_.history();
    status_.message = reason;
}

void StudioIntegration::discoverLocked(StudioStatus& st) {
    std::vector<std::string> notes;
    st.installed = roblox::findInstalled(roblox::Product::Studio, env_, &notes, false);
    st.running = roblox::findRunning(roblox::Product::Studio, env_);
    st.usingOverride = !options_.serverOverride.empty();
    st.mcpPath = st.usingOverride ? options_.serverOverride : (st.installed ? st.installed->mcpServerPath : std::string());
    if (st.installed) {
        if (auto prev = versions_.record(roblox::Product::Studio, st.installed->version)) {
            st.previousVersion = prev;
            logger_.info("studio", "Roblox Studio updated from " + *prev + " to " + st.installed->version +
                                       "; the MCP tool schema will be re-validated on the next connection");
        }
    }
}

StudioStatus StudioIntegration::refresh() {
    std::lock_guard lock(mutex_);
    bool connected = client_ && client_->running() &&
                     (status_.state == ConnectionState::Connected || status_.state == ConnectionState::IntegrationAvailable);
    StudioStatus st = status_;
    discoverLocked(st);
    status_.installed = st.installed;
    status_.running = st.running;
    status_.mcpPath = st.mcpPath;
    status_.usingOverride = st.usingOverride;
    if (st.previousVersion) status_.previousVersion = st.previousVersion;
    if (connected) return status_;

    transition(ConnectionState::Discovering, "Scanning for Roblox Studio");
    if (!st.installed && !st.usingOverride) {
        transition(ConnectionState::Disconnected, "Roblox Studio is not installed");
        return status_;
    }
    transition(ConnectionState::ClientFound, st.running.empty() ? "Roblox Studio installed (not running)" : "Roblox Studio is running");
    transition(ConnectionState::CheckingCompatibility, "Looking for Studio's MCP server");
    std::error_code ec;
    if (st.mcpPath.empty() || !std::filesystem::exists(st.mcpPath, ec)) {
        transition(ConnectionState::Unsupported, "This Roblox Studio installation does not include StudioMCP; update Studio to use the integration");
    } else {
        transition(ConnectionState::IntegrationAvailable, "StudioMCP found. Connect to launch it (in Studio: Assistant → … → Manage MCP Servers → "
                                                          "enable \"Studio as MCP server\")");
    }
    return status_;
}

StudioStatus StudioIntegration::connect() {
    refresh();
    std::shared_ptr<McpClient> old;
    StudioOptions opts;
    std::string mcpPath;
    {
        std::lock_guard lock(mutex_);
        if (status_.state != ConnectionState::IntegrationAvailable && status_.state != ConnectionState::Connected) return status_;
        old = std::move(client_);
        opts = options_;
        mcpPath = status_.mcpPath;
    }
    if (old) old->stop();

    platform::SignatureCheck signature;
    if (opts.requireRobloxSignature && opts.serverOverride.empty()) {
        signature = env_.verifySignature(mcpPath, roblox::kRobloxTeamId);
        if (!signature.valid) {
            std::lock_guard lock(mutex_);
            status_.mcpSignature = signature;
            transition(ConnectionState::Error, "Refusing to launch StudioMCP: its code signature could not be verified as Roblox's (" +
                                                   (signature.error.empty() ? std::string("unknown error") : signature.error) + ")");
            logger_.error("studio", status_.message);
            return status_;
        }
    }

    {
        std::lock_guard lock(mutex_);
        status_.mcpSignature = signature;
        transition(ConnectionState::CheckingCompatibility, "Launching StudioMCP and validating its tools");
    }

    auto client = std::make_shared<McpClient>(logger_);
    client->setExitHandler([this](const std::string& reason) { onServerExit(reason); });
    McpClient::Options mo;
    mo.executable = mcpPath;
    mo.arguments = opts.serverOverrideArgs;
    auto started = client->start(mo);
    if (!started.ok()) {
        std::lock_guard lock(mutex_);
        transition(ConnectionState::Error, "Could not start StudioMCP: " + started.error().message);
        return status_;
    }
    auto tools = client->listTools();
    if (!tools.ok()) {
        client->stop(); // not holding mutex_ here
        std::lock_guard lock(mutex_);
        transition(ConnectionState::Error, "StudioMCP did not list its tools: " + tools.error().message);
        return status_;
    }
    ToolMapping mapping = mapExecutionTool(tools.value());
    std::vector<std::string> names;
    for (const auto& t : tools.value()) names.push_back(t.name);

    std::vector<StudioInstance> studios;
    bool hasList = std::find(names.begin(), names.end(), "list_roblox_studios") != names.end();
    std::string listError;
    if (mapping.usable() && hasList) {
        auto listed = client->callTool("list_roblox_studios", json::Object{}, std::chrono::seconds(10));
        if (listed.ok() && !listed.value().isError) studios = parseStudioList(listed.value());
        else listError = listed.ok() ? listed.value().text : listed.error().message;
    }

    // Never stop the client while holding mutex_: its reader thread may need the lock to report exit.
    if (!mapping.usable()) client->stop();

    std::lock_guard lock(mutex_);
    status_.serverName = client->serverName();
    status_.serverVersion = client->serverVersion();
    status_.protocolVersion = client->protocolVersion();
    status_.tools = names;
    status_.mapping = mapping;
    status_.studios = studios;
    if (!mapping.usable()) {
        std::string why = mapping.tool.empty() ? "it does not offer an execute_luau (or run_code) tool"
                                               : "its " + mapping.tool + " tool requires parameters Nexus cannot fill: " + str::join(mapping.unmappedRequired, ", ");
        transition(ConnectionState::Unsupported, "StudioMCP is not compatible with this version of Nexus: " + why);
        logger_.warning("studio", status_.message);
        return status_;
    }
    client_ = client;
    ++generation_;
    status_.generation = generation_;
    if (status_.selectedStudioId.empty() || std::none_of(studios.begin(), studios.end(), [&](const StudioInstance& s) { return s.id == status_.selectedStudioId; }))
        status_.selectedStudioId = studios.empty() ? std::string() : studios.front().id;

    if (mapping.studioIdRequired && studios.empty()) {
        transition(ConnectionState::IntegrationAvailable,
                   "StudioMCP is running but reports no open Studio instance" + (listError.empty() ? std::string() : " (" + listError + ")") +
                       ". Open a place in Roblox Studio, enable \"Studio as MCP server\", then connect again.");
        return status_;
    }
    std::string target = studios.empty() ? std::string("Roblox Studio") : (studios.front().name.empty() ? studios.front().id : studios.front().name);
    transition(ConnectionState::IntegrationAvailable, "StudioMCP tool schema validated");
    transition(ConnectionState::Connected, "Connected to " + target + " via " +
                                               (status_.serverName.empty() ? std::string("StudioMCP") : status_.serverName) + " (" + mapping.tool + ")");
    logger_.info("studio", status_.message);
    return status_;
}

void StudioIntegration::disconnect(const std::string& reason) {
    std::shared_ptr<McpClient> client;
    {
        std::lock_guard lock(mutex_);
        client = std::move(client_);
        ++generation_;
        status_.generation = generation_;
        transition(ConnectionState::Disconnected, reason);
    }
    if (client) client->stop();
    logger_.info("studio", "Studio integration disconnected: " + reason);
}

void StudioIntegration::onServerExit(const std::string& reason) {
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        status_.generation = generation_;
        transition(ConnectionState::Error, reason);
    }
    std::function<void(const std::string&)> handler;
    {
        std::lock_guard lock(handlerMutex_);
        handler = disconnectHandler_;
    }
    if (handler) handler(reason);
}

StudioStatus StudioIntegration::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void StudioIntegration::selectStudio(const std::string& studioId) {
    std::lock_guard lock(mutex_);
    status_.selectedStudioId = studioId;
}

TargetInfo StudioIntegration::info() const {
    TargetInfo i;
    i.id = kId;
    i.displayName = "Roblox Studio (MCP)";
    i.description = "Runs scripts inside Roblox Studio through Studio's built-in, officially documented MCP server. Full Roblox "
                    "engine API (Edit, Server or Client data model). Not the live Roblox Player.";
    i.kind = TargetKind::RobloxStudio;
    i.capabilities.robloxEngineApis = true;
    i.capabilities.cancellation = true;
    i.capabilities.guaranteedStop = false;
    i.capabilities.resourceLimits = false;
    i.capabilities.outputCapture = true;
    i.capabilities.processIsolation = true;
    i.capabilities.taskScheduler = true;
    return i;
}

TargetAvailability StudioIntegration::availability() const {
    std::lock_guard lock(mutex_);
    if (status_.state == ConnectionState::Connected && client_ && client_->running()) return {true, status_.message};
    return {false, status_.state == ConnectionState::Connected ? std::string("The Studio MCP server is not running")
                                                               : "Roblox Studio is not connected (" + std::string(roblox::toString(status_.state)) +
                                                                     "): " + status_.message};
}

ExecutionResult StudioIntegration::execute(const ExecutionRequest& request, const OutputCallback& output, const CancellationToken& cancel) {
    auto start = std::chrono::steady_clock::now();
    std::shared_ptr<McpClient> client;
    ToolMapping mapping;
    std::string studioId;
    std::string dataModel;
    {
        std::lock_guard lock(mutex_);
        if (status_.state != ConnectionState::Connected || !client_ || !client_->running())
            return makeUnavailableResult(kId, "Roblox Studio is not connected");
        client = client_;
        mapping = status_.mapping;
        studioId = status_.selectedStudioId;
        dataModel = options_.dataModel;
    }
    if (const json::Value* dm = request.options.find("dataModel"); dm && dm->isString()) dataModel = dm->asString();

    json::Object args;
    args[mapping.codeParam] = request.source;
    if (!mapping.dataModelParam.empty()) {
        std::string value = dataModel;
        for (const auto& allowed : mapping.dataModelValues)
            if (str::toLower(allowed) == str::toLower(dataModel)) value = allowed;
        args[mapping.dataModelParam] = value;
    }
    if (!mapping.studioIdParam.empty() && !studioId.empty()) args[mapping.studioIdParam] = studioId;

    auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    if (output)
        output(OutputEvent{OutputStream::Info,
                           "Sending script to Roblox Studio (" + dataModel + " data model) through StudioMCP tool '" + mapping.tool + "'", 0});

    auto called = client->callTool(mapping.tool, std::move(args), request.limits.timeout, cancel);
    ExecutionResult r;
    r.target = kId;
    r.stats.wallMs = elapsed();
    if (!called.ok()) {
        const std::string& code = called.error().code;
        if (code == "mcp.cancelled") {
            r.status = ExecutionStatus::Cancelled;
            r.summary = "Cancellation sent to Roblox Studio. Studio may keep running the script: the MCP protocol does not guarantee termination.";
        } else if (code == "mcp.timeout") {
            r.status = ExecutionStatus::TimedOut;
            r.summary = "No response from Roblox Studio within the time limit; the script may still be running in Studio.";
        } else {
            r.status = ExecutionStatus::InternalError;
            r.summary = "Studio integration error: " + called.error().message;
        }
        return r;
    }
    const McpToolResult& res = called.value();
    if (output) {
        for (std::string_view line : str::splitLines(res.text))
            output(OutputEvent{res.isError ? OutputStream::Error : OutputStream::Output, std::string(line), elapsed()});
    }
    if (res.isError) {
        r.status = ExecutionStatus::RuntimeError;
        ScriptError err;
        err.raw = res.text;
        err.message = res.text.empty() ? "Roblox Studio reported an error without details" : std::string(str::splitLines(res.text).front());
        r.error = err;
        r.summary = "Roblox Studio reported an error: " + err.message;
    } else {
        r.status = ExecutionStatus::Succeeded;
        r.summary = "Executed in Roblox Studio (" + dataModel + " data model) in " + str::formatDuration(r.stats.wallMs);
    }
    return r;
}

} // namespace nexus::studio
