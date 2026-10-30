#include "nexus/editor/editor_services.hpp"

#include "nexus/compatibility/catalog.hpp"
#include "nexus/compiler/source_map.hpp"
#include "nexus/runtime/luau_runtime.hpp"

#include "Luau/Lexer.h"

namespace nexus::editor {

const char* toString(TokenKind k) {
    switch (k) {
    case TokenKind::Plain: return "plain";
    case TokenKind::Keyword: return "keyword";
    case TokenKind::Constant: return "constant";
    case TokenKind::Number: return "number";
    case TokenKind::String: return "string";
    case TokenKind::Comment: return "comment";
    case TokenKind::Operator: return "operator";
    case TokenKind::Builtin: return "builtin";
    case TokenKind::RobloxGlobal: return "roblox";
    case TokenKind::ExecutorGlobal: return "executor";
    case TokenKind::Function: return "function";
    case TokenKind::Property: return "property";
    case TokenKind::Attribute: return "attribute";
    case TokenKind::Error: return "error";
    }
    return "plain";
}

namespace {

struct Lex {
    Luau::Lexeme::Type type;
    uint32_t begin;
    uint32_t end;
    std::string name; // for Name lexemes
};

TokenKind classifyGlobal(const std::string& name) {
    if (isStandaloneGlobal(name)) return TokenKind::Builtin;
    if (const compat::SymbolInfo* s = compat::lookupGlobal(name)) {
        switch (s->kind) {
        case compat::SymbolKind::LuauBuiltin:
        case compat::SymbolKind::NexusExtension: return TokenKind::Builtin;
        case compat::SymbolKind::RobloxGlobal:
        case compat::SymbolKind::RobloxDatatype:
        case compat::SymbolKind::RobloxDeprecated: return TokenKind::RobloxGlobal;
        case compat::SymbolKind::ExecutorApi: return TokenKind::ExecutorGlobal;
        }
    }
    return TokenKind::Plain;
}

} // namespace

std::vector<HighlightToken> highlight(std::string_view source) {
    using T = Luau::Lexeme::Type;
    SourceMap map(source);
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::Lexer lexer(source.data(), source.size(), names);
    lexer.setSkipComments(false);

    std::vector<Lex> lexes;
    while (true) {
        const Luau::Lexeme& lx = lexer.next();
        if (lx.type == T::Eof) break;
        Lex l;
        l.type = lx.type;
        l.begin = static_cast<uint32_t>(map.offsetOf(lx.location.begin.line, lx.location.begin.column));
        l.end = static_cast<uint32_t>(map.offsetOf(lx.location.end.line, lx.location.end.column));
        if (lx.type == T::Name && lx.name) l.name = lx.name;
        if (l.end > l.begin) lexes.push_back(std::move(l));
        if (lexes.size() > 2'000'000) break; // defensive bound for pathological input
    }

    std::vector<HighlightToken> out;
    out.reserve(lexes.size());
    for (size_t i = 0; i < lexes.size(); ++i) {
        const Lex& l = lexes[i];
        TokenKind kind = TokenKind::Plain;
        switch (l.type) {
        case T::Number: kind = TokenKind::Number; break;
        case T::RawString:
        case T::QuotedString:
        case T::InterpStringBegin:
        case T::InterpStringMid:
        case T::InterpStringEnd:
        case T::InterpStringSimple: kind = TokenKind::String; break;
        case T::Comment:
        case T::BlockComment: kind = TokenKind::Comment; break;
        case T::Attribute:
        case T::AttributeOpen: kind = TokenKind::Attribute; break;
        case T::BrokenString:
        case T::BrokenComment:
        case T::BrokenUnicode:
        case T::BrokenInterpDoubleBrace:
        case T::Error: kind = TokenKind::Error; break;
        case T::ReservedTrue:
        case T::ReservedFalse:
        case T::ReservedNil: kind = TokenKind::Constant; break;
        case T::Name: {
            const Lex* prev = i > 0 ? &lexes[i - 1] : nullptr;
            // Skip comments when looking at neighbours.
            size_t j = i + 1;
            while (j < lexes.size() && (lexes[j].type == T::Comment || lexes[j].type == T::BlockComment)) ++j;
            const Lex* next = j < lexes.size() ? &lexes[j] : nullptr;
            bool calledNext = next && (next->type == T('(') || next->type == T::QuotedString || next->type == T('{'));
            if (prev && (prev->type == T('.') || prev->type == T(':'))) {
                kind = calledNext ? TokenKind::Function : TokenKind::Property;
            } else if (prev && prev->type == T::ReservedFunction) {
                kind = TokenKind::Function;
            } else {
                kind = classifyGlobal(l.name);
                if (kind == TokenKind::Plain && calledNext) kind = TokenKind::Function;
            }
            break;
        }
        default:
            if (l.type >= T::Reserved_BEGIN && l.type < T::Reserved_END) kind = TokenKind::Keyword;
            else kind = TokenKind::Operator;
            break;
        }
        if (kind == TokenKind::Plain) continue;
        out.push_back({l.begin, l.end - l.begin, kind});
    }
    return out;
}

} // namespace nexus::editor
