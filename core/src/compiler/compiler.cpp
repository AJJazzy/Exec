#include "nexus/compiler/compiler.hpp"

#include "Luau/BytecodeBuilder.h"
#include "Luau/Compiler.h"
#include "Luau/Parser.h"

#include <chrono>

namespace nexus {

namespace {

SourceRange toRange(const Luau::Location& loc) {
    SourceRange r;
    r.line = loc.begin.line + 1;
    r.column = loc.begin.column + 1;
    r.endLine = loc.end.line + 1;
    r.endColumn = loc.end.column + 1;
    return r;
}

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void appendParseErrors(const Luau::ParseResult& result, std::vector<Diagnostic>& out) {
    for (const Luau::ParseError& e : result.errors) {
        Diagnostic d;
        d.severity = Severity::Error;
        d.code = "syntax";
        d.message = e.getMessage();
        d.range = toRange(e.getLocation());
        d.origin = "parser";
        out.push_back(std::move(d));
    }
}

} // namespace

CompileOutput compileSource(std::string_view source, const CompileOptions& options) {
    CompileOutput out;
    auto start = std::chrono::steady_clock::now();

    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator, Luau::ParseOptions());
    out.parseMilliseconds = msSince(start);

    if (!parsed.errors.empty()) {
        appendParseErrors(parsed, out.diagnostics);
        return out;
    }

    auto compileStart = std::chrono::steady_clock::now();
    Luau::CompileOptions copts;
    copts.optimizationLevel = options.optimizationLevel;
    copts.debugLevel = options.debugLevel;
    try {
        Luau::BytecodeBuilder bytecode;
        Luau::compileOrThrow(bytecode, parsed, names, copts);
        out.bytecode = bytecode.getBytecode();
        out.ok = true;
    } catch (const Luau::CompileError& e) {
        Diagnostic d;
        d.severity = Severity::Error;
        d.code = "compile";
        d.message = e.what();
        d.range = toRange(e.getLocation());
        d.origin = "compiler";
        out.diagnostics.push_back(std::move(d));
    }
    out.compileMilliseconds = msSince(compileStart);
    return out;
}

std::vector<Diagnostic> checkSyntax(std::string_view source) {
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator, Luau::ParseOptions());
    std::vector<Diagnostic> out;
    appendParseErrors(parsed, out);
    return out;
}

const char* luauVersion() {
#ifdef NEXUS_LUAU_TAG
    return NEXUS_LUAU_TAG;
#else
    return "unknown";
#endif
}

} // namespace nexus
