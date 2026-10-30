#pragma once
// Read-only Mach-O inspector: architectures, deployment targets and the embedded code signature
// (CodeDirectory flags, Team ID, entitlements). Parses files on disk only; it never touches a
// running process. Every offset is bounds-checked, so malformed input yields an error, not a crash.

#include "nexus/common/json.hpp"
#include "nexus/common/result.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nexus::roblox {

struct CodeSignatureInfo {
    bool present = false;
    std::string identifier;
    std::string teamId;
    uint32_t flags = 0;
    uint32_t codeDirectoryVersion = 0;
    bool hasCmsSignature = false;     // signed with a certificate (not ad-hoc)
    bool adhoc = false;               // CS_ADHOC
    bool linkerSigned = false;        // CS_LINKER_SIGNED
    bool hardenedRuntime = false;     // CS_RUNTIME
    bool libraryValidation = false;   // CS_REQUIRE_LV
    std::map<std::string, json::Value> entitlements;

    json::Value toJson() const;
};

struct MachOSlice {
    std::string architecture; // "arm64", "arm64e", "x86_64", ...
    uint32_t cpuType = 0;
    uint32_t cpuSubtype = 0;
    uint32_t fileType = 0;    // MH_EXECUTE = 2, MH_DYLIB = 6, ...
    std::string minimumOS;    // e.g. "11.0"
    std::string sdk;          // e.g. "26.2"
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t linkedLibraryCount = 0;
    CodeSignatureInfo signature;

    json::Value toJson() const;
};

struct MachOInfo {
    bool universal = false;
    std::vector<MachOSlice> slices;

    std::vector<std::string> architectures() const;
    json::Value toJson() const;
};

Result<MachOInfo> inspectMachO(const std::filesystem::path& path);
Result<MachOInfo> inspectMachOBytes(const std::string& bytes); // for tests and in-memory input

// Security-relevant reading of a signature, with plain-language explanations of what macOS
// enforces for the inspected binary. Purely informational.
struct SecurityPosture {
    bool hardenedRuntime = false;
    bool getTaskAllow = false;                // debuggers may attach
    bool allowDyldEnvironment = false;        // DYLD_* variables honoured
    bool disableLibraryValidation = false;    // foreign dylibs may load
    bool allowUnsignedExecutableMemory = false;
    bool disableExecutablePageProtection = false;
    std::vector<std::string> explanations;

    json::Value toJson() const;
};

SecurityPosture evaluateSecurityPosture(const CodeSignatureInfo& signature);

std::string cpuTypeName(uint32_t cpuType, uint32_t cpuSubtype);

} // namespace nexus::roblox
