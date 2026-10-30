#include "nexus/editor/editor_services.hpp"

#include "nexus/common/strings.hpp"
#include "nexus/compatibility/catalog.hpp"
#include "nexus/runtime/luau_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace nexus::editor {

const char* toString(CompletionKind k) {
    switch (k) {
    case CompletionKind::Keyword: return "keyword";
    case CompletionKind::Global: return "global";
    case CompletionKind::Library: return "library";
    case CompletionKind::Member: return "member";
    case CompletionKind::Service: return "service";
    case CompletionKind::Identifier: return "identifier";
    case CompletionKind::Snippet: return "snippet";
    }
    return "identifier";
}

namespace {

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

const char* kKeywords[] = {"and", "break", "continue", "do", "else", "elseif", "end", "export", "false", "for", "function", "if",
                           "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "type", "until", "while"};

struct Snippet {
    const char* label;
    const char* insert;
    const char* detail;
};
const Snippet kSnippets[] = {
    {"function", "function name()\n\t\nend", "function declaration"},
    {"local function", "local function name()\n\t\nend", "local function"},
    {"for i", "for i = 1, 10 do\n\t\nend", "numeric for loop"},
    {"for pairs", "for key, value in pairs(t) do\n\t\nend", "generic for loop"},
    {"if", "if condition then\n\t\nend", "if statement"},
    {"while", "while condition do\n\t\nend", "while loop"},
    {"task.spawn", "task.spawn(function()\n\t\nend)", "spawn a task"},
};

// Methods commonly called on Roblox instances (offered after ':' when nothing better is known).
const char* kInstanceMethods[] = {"GetService", "FindFirstChild", "FindFirstChildOfClass", "FindFirstChildWhichIsA", "WaitForChild",
                                  "GetChildren", "GetDescendants", "IsA", "Destroy", "Clone", "GetAttribute", "SetAttribute",
                                  "Connect", "Once", "Wait", "Disconnect", "GetPropertyChangedSignal"};

int rank(CompletionKind k) {
    switch (k) {
    case CompletionKind::Member: return 0;
    case CompletionKind::Service: return 0;
    case CompletionKind::Identifier: return 1;
    case CompletionKind::Keyword: return 2;
    case CompletionKind::Library: return 2;
    case CompletionKind::Global: return 3;
    case CompletionKind::Snippet: return 4;
    }
    return 5;
}

} // namespace

CompletionResult complete(std::string_view source, size_t cursorOffset, size_t limit) {
    CompletionResult result;
    cursorOffset = std::min(cursorOffset, source.size());
    size_t start = cursorOffset;
    while (start > 0 && isIdent(source[start - 1])) --start;
    result.replaceOffset = static_cast<uint32_t>(start);
    result.prefix = std::string(source.substr(start, cursorOffset - start));
    std::string lowerPrefix = str::toLower(result.prefix);

    std::vector<CompletionItem> items;
    std::set<std::string> seen;
    auto add = [&](std::string label, CompletionKind kind, std::string detail, std::string insert = {}) {
        if (!str::startsWith(str::toLower(label), lowerPrefix)) return;
        if (label == result.prefix && kind != CompletionKind::Snippet) return;
        if (!seen.insert(label + "\x1f" + toString(kind)).second) return;
        items.push_back({std::move(label), kind, std::move(detail), insert.empty() ? std::string() : std::move(insert)});
    };

    // Inside GetService("...") → service names.
    {
        size_t q = start;
        if (q > 0 && (source[q - 1] == '"' || source[q - 1] == '\'')) {
            std::string_view before = source.substr(0, q - 1);
            while (!before.empty() && std::isspace(static_cast<unsigned char>(before.back()))) before.remove_suffix(1);
            if (str::endsWith(before, "GetService(") || str::endsWith(before, "FindService(")) {
                for (const auto& svc : compat::allServices()) add(std::string(svc.name), CompletionKind::Service, std::string(svc.description));
                goto finish;
            }
        }
    }

    if (start > 0 && (source[start - 1] == '.' || source[start - 1] == ':')) {
        char op = source[start - 1];
        size_t objEnd = start - 1;
        size_t objStart = objEnd;
        while (objStart > 0 && isIdent(source[objStart - 1])) --objStart;
        std::string object(source.substr(objStart, objEnd - objStart));
        const auto& libs = standaloneLibraryMembers();
        if (auto it = libs.find(object); it != libs.end() && op == '.') {
            for (const auto& m : it->second) add(m, CompletionKind::Member, object + "." + m);
        } else if (object == "game" || op == ':') {
            for (const char* m : kInstanceMethods) add(m, CompletionKind::Member, "Roblox Instance method");
            if (object == "game" && op == '.') {
                for (const auto& svc : compat::allServices()) add(std::string(svc.name), CompletionKind::Service, std::string(svc.description));
            }
        }
        // Words previously used after '.' or ':' in this file.
        for (size_t i = 1; i < source.size(); ++i) {
            if ((source[i - 1] == '.' || source[i - 1] == ':') && isIdent(source[i]) && i != start) {
                size_t e = i;
                while (e < source.size() && isIdent(source[e])) ++e;
                if (i != start) add(std::string(source.substr(i, e - i)), CompletionKind::Identifier, "used in this script");
                i = e;
            }
        }
        goto finish;
    }

    for (const char* kw : kKeywords) add(kw, CompletionKind::Keyword, "keyword");
    for (const auto& g : standaloneGlobals()) {
        bool isLib = standaloneLibraryMembers().count(g) > 0;
        const compat::SymbolInfo* info = compat::lookupGlobal(g);
        add(g, isLib ? CompletionKind::Library : CompletionKind::Global, info ? std::string(info->description) : "Luau global");
    }
    for (const auto& sym : compat::allGlobals()) {
        if (sym.kind == compat::SymbolKind::RobloxGlobal || sym.kind == compat::SymbolKind::RobloxDatatype)
            add(std::string(sym.name), CompletionKind::Global, "Roblox: " + std::string(sym.description));
    }
    // Identifiers already present in the file (locals, functions, fields).
    for (size_t i = 0; i < source.size();) {
        if (std::isalpha(static_cast<unsigned char>(source[i])) || source[i] == '_') {
            size_t e = i;
            while (e < source.size() && isIdent(source[e])) ++e;
            if (i != start && e - i >= 2) add(std::string(source.substr(i, e - i)), CompletionKind::Identifier, "used in this script");
            i = e;
        } else {
            ++i;
        }
    }
    if (!result.prefix.empty()) {
        for (const auto& s : kSnippets) add(s.label, CompletionKind::Snippet, s.detail, s.insert);
    }

finish:
    std::stable_sort(items.begin(), items.end(), [&](const CompletionItem& a, const CompletionItem& b) {
        bool ap = str::startsWith(a.label, result.prefix), bp = str::startsWith(b.label, result.prefix);
        if (ap != bp) return ap;
        if (rank(a.kind) != rank(b.kind)) return rank(a.kind) < rank(b.kind);
        return a.label < b.label;
    });
    if (items.size() > limit) items.resize(limit);
    result.items = std::move(items);
    return result;
}

} // namespace nexus::editor
