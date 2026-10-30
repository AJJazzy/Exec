#include "nexus/editor/editor_services.hpp"

#include "nexus/compiler/compiler.hpp"
#include "nexus/compiler/source_map.hpp"

#include "Luau/Lexer.h"

#include <vector>

namespace nexus::editor {

namespace {

using T = Luau::Lexeme::Type;

struct Tok {
    T type;
    uint32_t beginLine, endLine;
    size_t beginOffset;
};

std::vector<Tok> lexAll(std::string_view source, const SourceMap& map) {
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::Lexer lexer(source.data(), source.size(), names);
    lexer.setSkipComments(false);
    std::vector<Tok> toks;
    while (true) {
        const Luau::Lexeme& lx = lexer.next();
        if (lx.type == T::Eof) break;
        toks.push_back({lx.type, lx.location.begin.line, lx.location.end.line, map.offsetOf(lx.location.begin.line, lx.location.begin.column)});
    }
    return toks;
}

bool isOpener(T t) {
    return t == T::ReservedFunction || t == T::ReservedThen || t == T::ReservedDo || t == T::ReservedRepeat || t == T('{') ||
           t == T('(') || t == T('[');
}

bool isCloser(T t) { return t == T::ReservedEnd || t == T::ReservedUntil || t == T('}') || t == T(')') || t == T(']'); }

bool isComment(T t) { return t == T::Comment || t == T::BlockComment; }

} // namespace

FormatResult format(std::string_view source, const FormatOptions& options) {
    FormatResult result;
    if (!checkSyntax(source).empty()) {
        result.error = "The script has syntax errors; fix them before formatting.";
        return result;
    }

    SourceMap map(source);
    std::vector<Tok> toks = lexAll(source, map);

    // Split into lines, remembering CRLF usage.
    std::vector<std::string_view> lines;
    bool crlf = false;
    {
        size_t start = 0;
        for (size_t i = 0; i <= source.size(); ++i) {
            if (i == source.size() || source[i] == '\n') {
                std::string_view line = source.substr(start, i - start);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                    crlf = true;
                }
                lines.push_back(line);
                start = i + 1;
            }
        }
    }
    const size_t lineCount = lines.size();

    // Lines that begin inside a multi-line token (long string / block comment) are kept verbatim;
    // lines whose end lies inside one keep their trailing whitespace (it is part of the literal).
    std::vector<bool> verbatim(lineCount, false), keepTrailing(lineCount, false);
    for (const Tok& t : toks) {
        for (uint32_t l = t.beginLine + 1; l <= t.endLine && l < lineCount; ++l) verbatim[l] = true;
        for (uint32_t l = t.beginLine; l < t.endLine && l < lineCount; ++l) keepTrailing[l] = true;
    }

    // Compute each line's indent level with a stack of open blocks. Openers that share a line share
    // a base level, so `foo(function()` indents its body by one level, not two.
    std::vector<int> indent(lineCount, 0);
    std::vector<int> stack; // base indent of each open block
    size_t ti = 0;
    for (size_t line = 0; line < lineCount; ++line) {
        // First non-comment token starting on this line.
        size_t first = ti;
        while (first < toks.size() && toks[first].beginLine == line && isComment(toks[first].type)) ++first;
        bool hasCode = first < toks.size() && toks[first].beginLine == line;
        int lineIndent = stack.empty() ? 0 : stack.back() + 1;
        if (hasCode) {
            T ft = toks[first].type;
            if (isCloser(ft) || ft == T::ReservedElse || ft == T::ReservedElseif) lineIndent = stack.empty() ? 0 : stack.back();
        }
        indent[line] = lineIndent;

        for (; ti < toks.size() && toks[ti].beginLine == line; ++ti) {
            T t = toks[ti].type;
            if (t == T::ReservedElse || t == T::ReservedElseif) {
                // Closes the previous branch; `else` opens the next one, `elseif` does so at `then`.
                if (!stack.empty()) stack.pop_back();
                if (t == T::ReservedElse) stack.push_back(lineIndent);
            } else if (isCloser(t)) {
                if (!stack.empty()) stack.pop_back();
            } else if (isOpener(t)) {
                stack.push_back(lineIndent);
            }
        }
    }

    std::string unit = options.useTabs ? std::string("\t") : std::string(static_cast<size_t>(options.indentWidth), ' ');
    std::string out;
    out.reserve(source.size() + source.size() / 8);
    const char* eol = crlf ? "\r\n" : "\n";
    for (size_t line = 0; line < lineCount; ++line) {
        std::string_view text = lines[line];
        bool lastLine = line + 1 == lineCount;
        if (lastLine && text.empty()) break; // source ended with a newline
        if (verbatim[line]) {
            out.append(text);
        } else {
            size_t s = 0;
            while (s < text.size() && (text[s] == ' ' || text[s] == '\t')) ++s;
            std::string_view body = text.substr(s);
            if (options.trimTrailingWhitespace && !keepTrailing[line]) {
                while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.remove_suffix(1);
            }
            if (!body.empty()) {
                for (int k = 0; k < indent[line]; ++k) out += unit;
            }
            out.append(body);
        }
        if (!lastLine) out += eol;
        else if (options.ensureFinalNewline && !out.empty()) out += eol;
    }

    result.ok = true;
    result.changed = out != source;
    result.text = std::move(out);
    return result;
}

int64_t matchingBracket(std::string_view source, size_t offset) {
    SourceMap map(source);
    std::vector<Tok> toks = lexAll(source, map);
    auto isOpen = [](T t) { return t == T('(') || t == T('[') || t == T('{'); };
    auto isClose = [](T t) { return t == T(')') || t == T(']') || t == T('}'); };
    // Find the bracket token at offset, or just before it.
    long index = -1;
    for (size_t i = 0; i < toks.size(); ++i) {
        if ((isOpen(toks[i].type) || isClose(toks[i].type)) && (toks[i].beginOffset == offset || toks[i].beginOffset + 1 == offset)) {
            index = static_cast<long>(i);
            if (toks[i].beginOffset == offset) break;
        }
    }
    if (index < 0) return -1;
    T t = toks[static_cast<size_t>(index)].type;
    int depth = 0;
    if (isOpen(t)) {
        for (size_t i = static_cast<size_t>(index); i < toks.size(); ++i) {
            if (isOpen(toks[i].type)) ++depth;
            else if (isClose(toks[i].type) && --depth == 0) return static_cast<int64_t>(toks[i].beginOffset);
        }
    } else {
        for (long i = index; i >= 0; --i) {
            if (isClose(toks[static_cast<size_t>(i)].type)) ++depth;
            else if (isOpen(toks[static_cast<size_t>(i)].type) && --depth == 0) return static_cast<int64_t>(toks[static_cast<size_t>(i)].beginOffset);
        }
    }
    return -1;
}

} // namespace nexus::editor
