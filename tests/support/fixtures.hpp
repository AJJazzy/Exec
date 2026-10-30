#pragma once
// Test fixtures: synthetic Mach-O images and fake application bundles.
// These are constructed byte-by-byte by the tests; they are NOT Roblox binaries.

#include <cstdint>
#include <string>
#include <vector>

namespace nexus_test {

struct SyntheticSlice {
    uint32_t cpuType = 0x0100000c; // arm64
    uint32_t cpuSubtype = 0;
    bool signature = true;
    uint32_t csFlags = 0x10000; // hardened runtime
    std::string identifier = "com.example.synthetic";
    std::string teamId = "TEAMID1234";
    std::string entitlementsXml; // empty = no entitlements blob
};

std::string buildMachO(const SyntheticSlice& slice);
std::string buildUniversal(const std::vector<SyntheticSlice>& slices);

std::string infoPlist(const std::string& bundleId, const std::string& version, const std::string& executable,
                      const std::string& minimumSystem = "10.13");

// Creates <dir>/<bundleName>/Contents/{Info.plist,MacOS/<executable>} and returns the bundle path.
std::string makeBundle(const std::string& dir, const std::string& bundleName, const std::string& bundleId, const std::string& version,
                       const std::string& executable, const std::string& binary);

} // namespace nexus_test
