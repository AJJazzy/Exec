#pragma once
// Front end over the official Luau parser and compiler.

#include "nexus/diagnostics/diagnostic.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace nexus {

struct CompileOptions {
    int optimizationLevel = 1; // 0..2, see Luau::CompileOptions
    int debugLevel = 1;        // 1 = line info + function names (enough for tracebacks)
};

struct CompileOutput {
    bool ok = false;
    std::string bytecode; // valid Luau bytecode when ok
    std::vector<Diagnostic> diagnostics;
    double parseMilliseconds = 0;
    double compileMilliseconds = 0;
};

// Parses and compiles. Every parse error is reported (the Luau parser recovers after errors), so
// the editor can underline all of them at once.
CompileOutput compileSource(std::string_view source, const CompileOptions& options = {});

// Parse only; cheaper than compileSource and used for as-you-type diagnostics.
std::vector<Diagnostic> checkSyntax(std::string_view source);

// Version string of the linked Luau implementation.
const char* luauVersion();

} // namespace nexus
