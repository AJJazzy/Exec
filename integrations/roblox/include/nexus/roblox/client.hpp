#pragma once
// Discovery of installed and running Roblox clients (Player and Studio) on macOS.
//
// Everything here is read-only: Info.plist and Mach-O headers are parsed from disk, the process
// list comes from libproc, and code signatures are validated with Security.framework. Nexus never
// opens a task port, attaches a debugger, injects code or sends signals to Roblox processes.

#include "nexus/common/json.hpp"
#include "nexus/platform/system.hpp"
#include "nexus/roblox/macho.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nexus::roblox {

// Apple Developer Team ID observed in the code signature of the Roblox Player for macOS
// (version 0.745.0.7451137, both arm64 and x86_64 builds). Used to verify Roblox binaries.
constexpr const char* kRobloxTeamId = "2CFABCH843";

enum class Product { Player, Studio };
const char* toString(Product p);

struct ProductDescriptor {
    Product product;
    const char* displayName;
    const char* bundleId;
    const char* executableName;
    std::vector<const char*> bundleNames; // conventional install names
};

const ProductDescriptor& descriptor(Product p);

struct InstalledClient {
    Product product = Product::Player;
    std::string bundlePath;
    std::string executablePath;
    std::string bundleId;
    std::string version;           // CFBundleShortVersionString, e.g. 0.745.0.7451137
    std::string build;             // CFBundleVersion
    std::string minimumSystemVersion;
    std::optional<MachOInfo> binary;
    std::string binaryError;
    platform::SignatureCheck signature; // Security.framework validation (macOS only)
    std::string mcpServerPath;     // Studio only: Contents/MacOS/StudioMCP when present
    std::vector<std::string> notes;

    json::Value toJson() const;
};

struct RunningClient {
    pid_t pid = 0;
    Product product = Product::Player;
    std::string executablePath;
    std::string bundlePath;
    std::optional<bool> translated; // running under Rosetta 2

    json::Value toJson() const;
};

// Abstraction over the operating system so discovery logic can be tested with fixtures.
class ClientEnvironment {
public:
    virtual ~ClientEnvironment() = default;
    virtual std::vector<std::string> candidateBundlePaths(const ProductDescriptor& product) = 0;
    virtual std::vector<platform::ProcessEntry> processes() = 0;
    virtual std::optional<bool> isTranslated(pid_t pid) = 0;
    virtual platform::SignatureCheck verifySignature(const std::string& path, const std::string& teamId) = 0;
};

// The real system: Launch Services + /Applications + ~/Applications, libproc, Security.framework.
// Additional search directories can be supplied with NEXUS_ROBLOX_SEARCH_PATHS (colon separated).
class SystemClientEnvironment final : public ClientEnvironment {
public:
    std::vector<std::string> candidateBundlePaths(const ProductDescriptor& product) override;
    std::vector<platform::ProcessEntry> processes() override;
    std::optional<bool> isTranslated(pid_t pid) override;
    platform::SignatureCheck verifySignature(const std::string& path, const std::string& teamId) override;
};

// Reads an application bundle; fails when it is not a bundle of the expected product.
Result<InstalledClient> inspectBundle(const std::string& bundlePath, Product product, ClientEnvironment& env, bool verifySignature = true);

std::optional<InstalledClient> findInstalled(Product product, ClientEnvironment& env, std::vector<std::string>* notes = nullptr,
                                             bool verifySignature = true);
std::vector<RunningClient> findRunning(Product product, ClientEnvironment& env);

// Numeric comparison of dotted versions ("0.745.0.7451137"); returns <0, 0, >0.
int compareVersions(const std::string& a, const std::string& b);

} // namespace nexus::roblox
