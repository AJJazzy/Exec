// MOCK MCP server used by the Studio integration tests.
//
// It imitates the *documented* surface of Roblox Studio's built-in MCP server (tool names
// execute_luau and list_roblox_studios, a datamodel_type parameter, a studio_id parameter) so the
// Nexus MCP client and schema mapping can be tested without Roblox Studio. It does NOT emulate
// Roblox: execute_luau only echoes what it received. Passing tests against this mock say nothing
// about compatibility with a real Studio installation.
//
// MOCK_MCP_MODE: normal | no-exec | changed-schema | slow | crash-on-exec | no-studios

#include "nexus/common/json.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using nexus::json::Array;
using nexus::json::Object;
using nexus::json::Value;

namespace {

void send(const Value& v) {
    std::cout << nexus::json::dump(v) << "\n" << std::flush;
}

Value tools(const std::string& mode) {
    Array list;
    if (mode != "no-exec") {
        Object props{
            {"code", Object{{"type", "string"}, {"description", "Luau code"}}},
            {"datamodel_type", Object{{"type", "string"}, {"enum", Array{"Edit", "Client", "Server"}}}},
            {"studio_id", Object{{"type", "string"}}},
        };
        Array required{"code", "datamodel_type", "studio_id"};
        if (mode == "changed-schema") {
            props["sandbox_token"] = Object{{"type", "string"}};
            required.push_back("sandbox_token");
        }
        list.push_back(Object{{"name", "execute_luau"}, {"description", "Runs Luau in Studio (mock)"},
                              {"inputSchema", Object{{"type", "object"}, {"properties", props}, {"required", required}}}});
    }
    list.push_back(Object{{"name", "list_roblox_studios"}, {"description", "Lists Studio instances (mock)"},
                          {"inputSchema", Object{{"type", "object"}, {"properties", Object{}}}}});
    list.push_back(Object{{"name", "script_read"}, {"description", "unused"}, {"inputSchema", Object{{"type", "object"}}}});
    return Object{{"tools", list}};
}

} // namespace

int main() {
    const char* m = std::getenv("MOCK_MCP_MODE");
    std::string mode = m ? m : "normal";
    std::fprintf(stderr, "mock MCP server starting in mode %s\n", mode.c_str());
    std::string line;
    while (std::getline(std::cin, line)) {
        auto parsed = nexus::json::parse(line);
        if (!parsed.ok()) continue;
        const Value& msg = parsed.value();
        const Value* id = msg.get("id");
        std::string method = msg.getString("method");
        if (!id) continue; // notifications (initialized, cancelled)
        if (method == "initialize") {
            send(Object{{"jsonrpc", "2.0"}, {"id", *id},
                        {"result", Object{{"protocolVersion", "2025-06-18"}, {"capabilities", Object{{"tools", Object{}}}},
                                          {"serverInfo", Object{{"name", "MockStudioMCP"}, {"version", "0.0-test"}}}}}});
        } else if (method == "tools/list") {
            send(Object{{"jsonrpc", "2.0"}, {"id", *id}, {"result", tools(mode)}});
        } else if (method == "tools/call") {
            const Value* params = msg.get("params");
            std::string name = params ? params->getString("name") : "";
            const Value* args = params ? params->get("arguments") : nullptr;
            if (name == "list_roblox_studios") {
                std::string text = mode == "no-studios" ? "[]" : R"([{"studio_id":"studio-1","name":"MockPlace","place_id":"123"}])";
                send(Object{{"jsonrpc", "2.0"}, {"id", *id}, {"result", Object{{"content", Array{Object{{"type", "text"}, {"text", text}}}}}}});
            } else if (name == "execute_luau") {
                if (mode == "crash-on-exec") return 3;
                if (mode == "slow") std::this_thread::sleep_for(std::chrono::seconds(30));
                std::string code = args ? args->getString("code") : "";
                std::string dm = args ? args->getString("datamodel_type") : "";
                std::string studio = args ? args->getString("studio_id") : "";
                bool isError = code.find("error(") != std::string::npos;
                std::string text = "MOCK received " + std::to_string(code.size()) + " bytes for " + dm + " on " + studio;
                if (isError) text = "MOCK error: script raised an error";
                send(Object{{"jsonrpc", "2.0"}, {"id", *id},
                            {"result", Object{{"content", Array{Object{{"type", "text"}, {"text", text}}}}, {"isError", isError}}}});
            } else {
                send(Object{{"jsonrpc", "2.0"}, {"id", *id}, {"error", Object{{"code", -32602}, {"message", "unknown tool"}}}});
            }
        } else if (method == "ping") {
            send(Object{{"jsonrpc", "2.0"}, {"id", *id}, {"result", Object{}}});
        } else {
            send(Object{{"jsonrpc", "2.0"}, {"id", *id}, {"error", Object{{"code", -32601}, {"message", "method not found"}}}});
        }
    }
    return 0;
}
