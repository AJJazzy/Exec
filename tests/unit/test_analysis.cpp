// Unit tests: compatibility analyzer and editor services (highlighter, formatter, completion).
#include "nexus_test.hpp"

#include "nexus/compatibility/analyzer.hpp"
#include "nexus/compiler/compiler.hpp"
#include "nexus/editor/editor_services.hpp"

#include <algorithm>

using namespace nexus;
using compat::Availability;
using compat::FindingKind;

namespace {

const compat::Finding* find(const compat::CompatibilityReport& r, const std::string& symbol) {
    for (const auto& f : r.findings)
        if (f.symbol == symbol) return &f;
    return nullptr;
}

const compat::TargetVerdict& verdict(const compat::CompatibilityReport& r, const std::string& id) {
    for (const auto& v : r.verdicts)
        if (v.targetId == id) return v;
    throw std::runtime_error("no verdict for " + id);
}

std::string kindAt(const std::string& src, const std::vector<editor::HighlightToken>& toks, const std::string& text) {
    size_t pos = src.find(text);
    for (const auto& t : toks)
        if (t.offset == pos && t.length == text.size()) return editor::toString(t.kind);
    return "none";
}

} // namespace

TEST_CASE(analyzer_pure_luau_runs_standalone) {
    auto r = compat::analyzeCompatibility("local t = {}\nfor i = 1, 10 do t[i] = math.sqrt(i) end\nprint(#t)\ntask.wait(0.1)");
    CHECK(r.syntaxOk);
    CHECK(r.findings.empty());
    CHECK(verdict(r, "luau-isolated").verdict == Availability::Available);
    CHECK(verdict(r, "roblox-studio").verdict == Availability::Available);
    CHECK(verdict(r, "roblox-player").verdict == Availability::Unavailable); // always
}

TEST_CASE(analyzer_detects_roblox_dependencies_and_services) {
    auto r = compat::analyzeCompatibility(R"(
local Players = game:GetService("Players")
local RS = game:GetService("RunService")
local rep = game.ReplicatedStorage
local part = Instance.new("Part")
part.Position = Vector3.new(0, 5, 0) * CFrame.new()
local path = game:GetService("PathfindingService")
local uis = game:GetService("UserInputService")
print(workspace, Players.LocalPlayer)
)");
    CHECK(r.syntaxOk);
    for (const char* g : {"game", "Instance", "Vector3", "CFrame", "workspace"}) {
        const auto* f = find(r, g);
        CHECK(f != nullptr);
        if (f) CHECK(f->standalone == Availability::Unavailable);
    }
    for (const char* s : {"Players", "RunService", "ReplicatedStorage", "PathfindingService", "UserInputService"})
        CHECK(std::find(r.services.begin(), r.services.end(), s) != r.services.end());
    const auto* uis = find(r, "UserInputService");
    REQUIRE(uis);
    CHECK(uis->kind == FindingKind::ContextRestriction);
    CHECK(uis->studio == Availability::Partial);
    CHECK(find(r, "Players.LocalPlayer") != nullptr);
    CHECK(verdict(r, "luau-isolated").verdict == Availability::Unavailable);
    CHECK(verdict(r, "roblox-studio").verdict == Availability::Partial);
}

TEST_CASE(analyzer_flags_executor_apis_everywhere) {
    auto r = compat::analyzeCompatibility("getgenv().x = 1\nhookfunction(print, function() end)\nlocal s = debug.getupvalue(print, 1)\nwritefile('a.txt', 'x')");
    for (const char* sym : {"getgenv", "hookfunction", "debug.getupvalue", "writefile"}) {
        const auto* f = find(r, sym);
        CHECK(f != nullptr);
        if (!f) continue;
        CHECK(f->kind == FindingKind::ExecutorApi);
        CHECK(f->severity == Severity::Error);
        CHECK(f->studio == Availability::Unavailable);
    }
    CHECK(!find(r, "writefile")->risk.empty());
    CHECK(verdict(r, "roblox-studio").verdict == Availability::Unavailable);
}

TEST_CASE(analyzer_flags_remote_code_execution) {
    auto r = compat::analyzeCompatibility("loadstring(game:HttpGet('https://example.com/x.lua'))()");
    const auto* f = find(r, "loadstring");
    REQUIRE(f);
    CHECK(f->kind == FindingKind::DangerousOperation);
    CHECK(f->severity == Severity::Error);
    CHECK(f->risk.find("remote") != std::string::npos);
    auto plain = compat::analyzeCompatibility("local f = loadstring('return 1')");
    const auto* p = find(plain, "loadstring");
    REQUIRE(p);
    CHECK(p->severity == Severity::Warning);
    CHECK(verdict(plain, "luau-isolated").verdict == Availability::Available);
    auto req = compat::analyzeCompatibility("local m = require(123456789)");
    CHECK(find(req, "require(assetId)") != nullptr);
}

TEST_CASE(analyzer_respects_scopes_and_user_globals) {
    auto r = compat::analyzeCompatibility(R"(
local game = { name = "not roblox" }
print(game.name)
function helper() return 1 end
counter = 0
counter = counter + helper()
print(totallyUnknown)
)");
    CHECK(find(r, "game") == nullptr);    // local shadows the Roblox global
    CHECK(find(r, "helper") == nullptr);  // defined by the script
    CHECK(find(r, "counter") == nullptr);
    const auto* u = find(r, "totallyUnknown");
    REQUIRE(u);
    CHECK(u->kind == FindingKind::UnknownGlobal);
    CHECK(verdict(r, "luau-isolated").verdict == Availability::Unknown);
}

TEST_CASE(analyzer_detects_unbounded_loops) {
    auto r = compat::analyzeCompatibility("while true do end\nwhile true do task.wait() end\nwhile true do if x then break end end\nrepeat local a = 1 until false");
    int loops = 0;
    for (const auto& f : r.findings)
        if (f.kind == FindingKind::RuntimeAssumption && f.symbol.find("while") != std::string::npos) ++loops;
    CHECK_EQ(loops, 1);
    bool repeatFlagged = std::any_of(r.findings.begin(), r.findings.end(), [](const auto& f) { return f.symbol.find("repeat") != std::string::npos; });
    CHECK(repeatFlagged);
}

TEST_CASE(analyzer_reports_syntax_errors) {
    auto r = compat::analyzeCompatibility("local x = = 1");
    CHECK(!r.syntaxOk);
    CHECK(!r.syntax.empty());
    for (const auto& v : r.verdicts) CHECK(v.verdict == Availability::Unavailable);
    CHECK(!r.toDiagnostics().empty());
    auto j = r.toJson();
    CHECK_EQ(j.getString("format"), std::string("nexus-compatibility-v1"));
    CHECK(r.toText().find("Syntax errors") != std::string::npos);
}

TEST_CASE(highlighter_classifies_tokens) {
    std::string src = "-- comment\nlocal s = \"str\" .. `x{1}` .. 42\nprint(game, getgenv, math.floor(1.5), true)\nfunction foo() end\n";
    auto toks = editor::highlight(src);
    CHECK_EQ(kindAt(src, toks, "-- comment"), std::string("comment"));
    CHECK_EQ(kindAt(src, toks, "local"), std::string("keyword"));
    CHECK_EQ(kindAt(src, toks, "\"str\""), std::string("string"));
    CHECK_EQ(kindAt(src, toks, "42"), std::string("number"));
    CHECK_EQ(kindAt(src, toks, "print"), std::string("builtin"));
    CHECK_EQ(kindAt(src, toks, "game"), std::string("roblox"));
    CHECK_EQ(kindAt(src, toks, "getgenv"), std::string("executor"));
    CHECK_EQ(kindAt(src, toks, "floor"), std::string("function"));
    CHECK_EQ(kindAt(src, toks, "true"), std::string("constant"));
    CHECK_EQ(kindAt(src, toks, "foo"), std::string("function"));
    // Tokens are sorted and within bounds.
    for (size_t i = 1; i < toks.size(); ++i) CHECK(toks[i].offset >= toks[i - 1].offset);
    for (const auto& t : toks) CHECK(t.offset + t.length <= src.size());
}

TEST_CASE(highlighter_handles_broken_and_unicode_input) {
    std::string src = "local s = \"unterminated\nprint('é😀')\n--[[ unterminated block";
    auto toks = editor::highlight(src);
    CHECK(!toks.empty());
    for (const auto& t : toks) CHECK(t.offset + t.length <= src.size());
}

TEST_CASE(formatter_indents_blocks) {
    std::string src = "local function f(x)\nif x then\nprint(x)\nelseif x == 2 then\nprint(2)\nelse\nfor i=1,3 do\nprint(i)\nend\nend\nend\n";
    auto r = editor::format(src, {2, false, true, true});
    REQUIRE(r.ok);
    CHECK_EQ(r.text, std::string("local function f(x)\n  if x then\n    print(x)\n  elseif x == 2 then\n    print(2)\n  else\n    for i=1,3 do\n      print(i)\n    end\n  end\nend\n"));
}

TEST_CASE(formatter_handles_closures_tables_and_strings) {
    std::string src = "task.spawn(function()\nlocal t = {\na = 1,\n}\nlocal s = [[\n   keep   \n  this]]\nend)   \n";
    auto r = editor::format(src);
    REQUIRE(r.ok);
    CHECK_EQ(r.text, std::string("task.spawn(function()\n    local t = {\n        a = 1,\n    }\n    local s = [[\n   keep   \n  this]]\nend)\n"));
}

TEST_CASE(formatter_is_idempotent_and_preserves_bytecode) {
    std::string src = "local x=1\n  while x<10 do\nx=x+1\n      if x%2==0 then print(x) end\n end\nreturn x";
    auto once = editor::format(src);
    REQUIRE(once.ok);
    auto twice = editor::format(once.text);
    REQUIRE(twice.ok);
    CHECK_EQ(once.text, twice.text);
    CHECK(!twice.changed);
    auto a = compileSource(src);
    auto b = compileSource(once.text);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(a.bytecode == b.bytecode); // whitespace-only change, same line numbers
}

TEST_CASE(formatter_rejects_syntax_errors_and_keeps_crlf) {
    CHECK(!editor::format("if x then").ok);
    auto r = editor::format("if x then\r\nprint(1)\r\nend\r\n");
    REQUIRE(r.ok);
    CHECK_EQ(r.text, std::string("if x then\r\n    print(1)\r\nend\r\n"));
}

TEST_CASE(completion_suggests_members_services_and_keywords) {
    std::string src = "local x = math.fl";
    auto c = editor::complete(src, src.size());
    CHECK_EQ(c.prefix, std::string("fl"));
    REQUIRE(!c.items.empty());
    CHECK_EQ(c.items.front().label, std::string("floor"));
    std::string svc = "local p = game:GetService(\"Pla";
    auto s = editor::complete(svc, svc.size());
    REQUIRE(!s.items.empty());
    CHECK_EQ(s.items.front().label, std::string("Players"));
    std::string kw = "loc";
    auto k = editor::complete(kw, kw.size());
    REQUIRE(!k.items.empty());
    CHECK_EQ(k.items.front().label, std::string("local"));
    std::string t = "task.";
    auto tk = editor::complete(t, t.size());
    CHECK_EQ(tk.items.size(), size_t(5));
}

TEST_CASE(bracket_matching_ignores_strings) {
    std::string src = "print((\"(\" .. {1}[1]))";
    size_t open = src.find('(');
    int64_t close = editor::matchingBracket(src, open);
    CHECK_EQ(close, static_cast<int64_t>(src.size() - 1));
    CHECK_EQ(editor::matchingBracket(src, src.size()), static_cast<int64_t>(open)); // cursor just after ')'
    CHECK_EQ(editor::matchingBracket(src, 2), int64_t(-1));
}
