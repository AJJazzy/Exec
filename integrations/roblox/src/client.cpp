#include "nexus/roblox/client.hpp"

#include "nexus/common/strings.hpp"
#include "nexus/roblox/plist.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace nexus::roblox {

const char* toString(Product p) { return p == Product::Player ? "player" : "studio"; }

const ProductDescriptor& descriptor(Product p) {
    static const ProductDescriptor player{Product::Player, "Roblox Player", "com.roblox.RobloxPlayer", "RobloxPlayer", {"Roblox.app", "RobloxPlayer.app"}};
    static const ProductDescriptor studio{Product::Studio, "Roblox Studio", "com.Roblox.RobloxStudio", "RobloxStudio", {"RobloxStudio.app"}};
    return p == Product::Player ? player : studio;
}

json::Value InstalledClient::toJson() const {
    json::Array n;
    for (const auto& x : notes) n.push_back(x);
    json::Object sig{
        {"supported", signature.supported}, {"signed", signature.signedCode}, {"valid", signature.valid},
        {"identifier", signature.identifier}, {"teamIdentifier", signature.teamIdentifier}, {"flags", signature.flags},
        {"error", signature.error},
    };
    json::Object o{
        {"product", toString(product)},
        {"bundlePath", bundlePath},
        {"executablePath", executablePath},
        {"bundleId", bundleId},
        {"version", version},
        {"build", build},
        {"minimumSystemVersion", minimumSystemVersion},
        {"signatureCheck", std::move(sig)},
        {"mcpServerPath", mcpServerPath},
        {"binaryError", binaryError},
        {"notes", std::move(n)},
    };
    if (binary) {
        o["binary"] = binary->toJson();
        if (!binary->slices.empty()) o["securityPosture"] = evaluateSecurityPosture(binary->slices.front().signature).toJson();
    }
    return o;
}

json::Value RunningClient::toJson() const {
    json::Object o{{"pid", static_cast<int64_t>(pid)}, {"product", toString(product)}, {"executablePath", executablePath}, {"bundlePath", bundlePath}};
    if (translated) o["translated"] = *translated;
    return o;
}

std::vector<std::string> SystemClientEnvironment::candidateBundlePaths(const ProductDescriptor& product) {
    std::vector<std::string> paths = platform::applicationPathsForBundleIdentifier(product.bundleId);
    std::vector<std::string> dirs = {"/Applications", platform::homeDirectory() + "/Applications"};
    if (const char* extra = std::getenv("NEXUS_ROBLOX_SEARCH_PATHS"); extra && *extra) {
        for (auto& d : str::split(extra, ':'))
            if (!d.empty()) dirs.insert(dirs.begin(), d);
    }
    for (const auto& dir : dirs)
        for (const char* name : product.bundleNames) paths.push_back(dir + "/" + name);
    return paths;
}

std::vector<platform::ProcessEntry> SystemClientEnvironment::processes() { return platform::listProcesses(); }
std::optional<bool> SystemClientEnvironment::isTranslated(pid_t pid) { return platform::isProcessTranslated(pid); }
platform::SignatureCheck SystemClientEnvironment::verifySignature(const std::string& path, const std::string& teamId) {
    return platform::verifyCodeSignature(path, teamId);
}

Result<InstalledClient> inspectBundle(const std::string& bundlePath, Product product, ClientEnvironment& env, bool verifySignature) {
    const ProductDescriptor& d = descriptor(product);
    fs::path bundle(bundlePath);
    std::error_code ec;
    if (!fs::is_directory(bundle, ec)) return Error("roblox.not_found", bundlePath + " is not a directory");
    auto plist = readPlistFile(bundle / "Contents" / "Info.plist");
    if (!plist.ok()) return Error("roblox.plist", "cannot read Info.plist: " + plist.error().message);
    const json::Value& info = plist.value();

    InstalledClient c;
    c.product = product;
    c.bundlePath = fs::weakly_canonical(bundle, ec).string();
    c.bundleId = info.getString("CFBundleIdentifier");
    if (str::toLower(c.bundleId) != str::toLower(d.bundleId))
        return Error("roblox.bundle_id", bundlePath + " has bundle identifier '" + c.bundleId + "', expected " + d.bundleId);
    c.version = info.getString("CFBundleShortVersionString");
    c.build = info.getString("CFBundleVersion");
    c.minimumSystemVersion = info.getString("LSMinimumSystemVersion");
    std::string exe = info.getString("CFBundleExecutable", d.executableName);
    if (exe.find('/') != std::string::npos) exe = d.executableName;
    c.executablePath = (bundle / "Contents" / "MacOS" / exe).string();

    auto bin = inspectMachO(c.executablePath);
    if (bin.ok()) c.binary = bin.value();
    else c.binaryError = bin.error().message;

    if (product == Product::Studio) {
        fs::path mcp = bundle / "Contents" / "MacOS" / "StudioMCP";
        if (fs::exists(mcp, ec)) c.mcpServerPath = mcp.string();
        else c.notes.push_back("StudioMCP was not found in this Studio installation (it ships with recent Studio versions).");
    }
    if (verifySignature) c.signature = env.verifySignature(c.bundlePath, kRobloxTeamId);
    if (c.version.empty()) c.notes.push_back("Info.plist does not declare CFBundleShortVersionString.");
    return c;
}

std::optional<InstalledClient> findInstalled(Product product, ClientEnvironment& env, std::vector<std::string>* notes, bool verifySignature) {
    std::set<std::string> seen;
    for (const auto& path : env.candidateBundlePaths(descriptor(product))) {
        std::error_code ec;
        if (!fs::exists(path, ec)) continue;
        std::string canonical = fs::weakly_canonical(path, ec).string();
        if (!seen.insert(canonical).second) continue;
        auto client = inspectBundle(canonical, product, env, verifySignature);
        if (client.ok()) return client.value();
        if (notes) notes->push_back(client.error().message);
    }
    return std::nullopt;
}

std::vector<RunningClient> findRunning(Product product, ClientEnvironment& env) {
    const ProductDescriptor& d = descriptor(product);
    std::string suffix = std::string("/Contents/MacOS/") + d.executableName;
    std::vector<RunningClient> out;
    for (const auto& p : env.processes()) {
        if (!str::endsWith(p.executablePath, suffix)) continue;
        RunningClient r;
        r.pid = p.pid;
        r.product = product;
        r.executablePath = p.executablePath;
        r.bundlePath = p.executablePath.substr(0, p.executablePath.size() - suffix.size());
        r.translated = env.isTranslated(p.pid);
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const RunningClient& a, const RunningClient& b) { return a.pid < b.pid; });
    return out;
}

int compareVersions(const std::string& a, const std::string& b) {
    auto pa = str::split(a, '.');
    auto pb = str::split(b, '.');
    size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned long long x = i < pa.size() ? std::strtoull(pa[i].c_str(), nullptr, 10) : 0;
        unsigned long long y = i < pb.size() ? std::strtoull(pb[i].c_str(), nullptr, 10) : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

} // namespace nexus::roblox
