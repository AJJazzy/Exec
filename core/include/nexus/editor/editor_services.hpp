#pragma once
// Language services for the code editor, built on the official Luau lexer so highlighting and
// formatting agree exactly with how Luau itself tokenises the source.

#include "nexus/common/json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::editor {

// ---- Syntax highlighting --------------------------------------------------------------------

enum class TokenKind : uint8_t {
    Plain,
    Keyword,
    Constant,       // true, false, nil
    Number,
    String,
    Comment,
    Operator,
    Builtin,        // Luau/Nexus standalone global (print, math, task, ...)
    RobloxGlobal,   // Roblox engine global or datatype (game, Instance, Vector3, ...)
    ExecutorGlobal, // third-party executor API (getgenv, ...)
    Function,       // called or declared function name
    Property,       // name after '.' or ':'
    Attribute,      // @native etc.
    Error,          // malformed token
};
const char* toString(TokenKind k);

struct HighlightToken {
    uint32_t offset = 0; // byte offset in the UTF-8 source
    uint32_t length = 0; // byte length
    TokenKind kind = TokenKind::Plain;
};

std::vector<HighlightToken> highlight(std::string_view source);

// ---- Formatting -------------------------------------------------------------------------------

struct FormatOptions {
    int indentWidth = 4;
    bool useTabs = false;
    bool trimTrailingWhitespace = true;
    bool ensureFinalNewline = true;
};

struct FormatResult {
    bool ok = false;
    std::string text;
    std::string error;
    bool changed = false;
};

// Re-indents code by block structure and trims trailing whitespace. Only whitespace outside
// string literals and comments changes, and the line count is preserved, so the compiled bytecode
// (which carries line numbers) is identical before and after formatting. Sources with syntax
// errors are rejected rather than risk damaging them.
FormatResult format(std::string_view source, const FormatOptions& options = {});

// ---- Completion ------------------------------------------------------------------------------

enum class CompletionKind : uint8_t { Keyword, Global, Library, Member, Service, Identifier, Snippet };
const char* toString(CompletionKind k);

struct CompletionItem {
    std::string label;
    CompletionKind kind = CompletionKind::Identifier;
    std::string detail;
    std::string insertText;
};

struct CompletionResult {
    uint32_t replaceOffset = 0; // byte offset where the completed word starts
    std::string prefix;
    std::vector<CompletionItem> items;
};

CompletionResult complete(std::string_view source, size_t cursorOffset, size_t limit = 100);

// Bracket matching: byte offset of the bracket matching the one at `offset` (which may also be the
// position just after a bracket), or -1. Brackets inside strings/comments are ignored.
int64_t matchingBracket(std::string_view source, size_t offset);

} // namespace nexus::editor
