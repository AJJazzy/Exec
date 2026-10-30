#pragma once
// Portable reader for XML property lists (Info.plist, entitlements). Binary plists are converted to
// XML first through CoreFoundation on macOS (platform::propertyListAsXml).

#include "nexus/common/json.hpp"
#include "nexus/common/result.hpp"

#include <filesystem>
#include <string_view>

namespace nexus::roblox {

// dict -> object, array -> array, string/date/data -> string, integer/real -> number, true/false -> bool.
Result<json::Value> parseXmlPlist(std::string_view xml);

// Reads a plist file of any format (binary support requires macOS).
Result<json::Value> readPlistFile(const std::filesystem::path& path);

} // namespace nexus::roblox
