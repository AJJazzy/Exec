#include "fixtures.hpp"

#include "nexus/library/script_library.hpp"

#include <filesystem>

namespace nexus_test {

namespace {
void le32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b += static_cast<char>((v >> (8 * i)) & 0xff);
}
void be32(std::string& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b += static_cast<char>((v >> (8 * i)) & 0xff);
}
void putBe32(std::string& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<char>((v >> (8 * (3 - i))) & 0xff);
}
void pad8(std::string& b) {
    while (b.size() % 8) b += '\0';
}
} // namespace

std::string buildMachO(const SyntheticSlice& s) {
    // Load commands.
    std::string cmds;
    // LC_BUILD_VERSION: platform macOS, minos 11.0, sdk 26.2
    le32(cmds, 0x32); le32(cmds, 24); le32(cmds, 1); le32(cmds, 0x000B0000); le32(cmds, 0x001A0200); le32(cmds, 0);
    // LC_LOAD_DYLIB /usr/lib/libSystem.B.dylib
    std::string dylib;
    le32(dylib, 0xc); le32(dylib, 0); le32(dylib, 24); le32(dylib, 2); le32(dylib, 0x10000); le32(dylib, 0x10000);
    dylib += "/usr/lib/libSystem.B.dylib";
    dylib += '\0';
    pad8(dylib);
    dylib[4] = static_cast<char>(dylib.size()); // cmdsize (fits in one byte here)
    cmds += dylib;

    std::string signature;
    if (s.signature) {
        // CodeDirectory (version 0x20400 with team id).
        std::string cd;
        be32(cd, 0xfade0c02); be32(cd, 0); be32(cd, 0x20400); be32(cd, s.csFlags);
        be32(cd, 0);  // hashOffset
        be32(cd, 0);  // identOffset (patched)
        be32(cd, 0);  // nSpecialSlots
        be32(cd, 0);  // nCodeSlots
        be32(cd, 0);  // codeLimit
        cd += '\x20'; cd += '\x02'; cd += '\0'; cd += '\x0c'; // hashSize, hashType, platform, pageSize
        be32(cd, 0);  // spare2
        be32(cd, 0);  // scatterOffset
        be32(cd, 0);  // teamOffset (patched)
        while (cd.size() < 88) cd += '\0';
        putBe32(cd, 20, static_cast<uint32_t>(cd.size()));
        cd += s.identifier + '\0';
        if (!s.teamId.empty()) {
            putBe32(cd, 48, static_cast<uint32_t>(cd.size()));
            cd += s.teamId + '\0';
        }
        putBe32(cd, 4, static_cast<uint32_t>(cd.size()));

        std::string ent;
        if (!s.entitlementsXml.empty()) {
            be32(ent, 0xfade7171); be32(ent, static_cast<uint32_t>(8 + s.entitlementsXml.size()));
            ent += s.entitlementsXml;
        }
        uint32_t count = ent.empty() ? 1 : 2;
        uint32_t headerSize = 12 + count * 8;
        be32(signature, 0xfade0cc0); be32(signature, 0); be32(signature, count);
        be32(signature, 0); be32(signature, headerSize);
        if (!ent.empty()) { be32(signature, 5); be32(signature, headerSize + static_cast<uint32_t>(cd.size())); }
        signature += cd + ent;
        putBe32(signature, 4, static_cast<uint32_t>(signature.size()));
    }

    uint32_t ncmds = 2 + (s.signature ? 1 : 0);
    uint32_t sizeofcmds = static_cast<uint32_t>(cmds.size()) + (s.signature ? 16 : 0);
    std::string out;
    le32(out, 0xfeedfacf); le32(out, s.cpuType); le32(out, s.cpuSubtype); le32(out, 2);
    le32(out, ncmds); le32(out, sizeofcmds); le32(out, 0x00200085); le32(out, 0);
    out += cmds;
    if (s.signature) {
        uint32_t dataoff = static_cast<uint32_t>(out.size() + 16 + 64); // some "code" bytes before the signature
        le32(out, 0x1d); le32(out, 16); le32(out, dataoff); le32(out, static_cast<uint32_t>(signature.size()));
        out.append(64, '\xcc');
        out += signature;
    } else {
        out.append(64, '\xcc');
    }
    return out;
}

std::string buildUniversal(const std::vector<SyntheticSlice>& slices) {
    std::vector<std::string> images;
    for (const auto& s : slices) images.push_back(buildMachO(s));
    std::string out;
    be32(out, 0xcafebabe); be32(out, static_cast<uint32_t>(images.size()));
    uint32_t offset = 4096;
    std::vector<uint32_t> offsets;
    for (size_t i = 0; i < images.size(); ++i) {
        be32(out, slices[i].cpuType); be32(out, slices[i].cpuSubtype); be32(out, offset);
        be32(out, static_cast<uint32_t>(images[i].size())); be32(out, 12);
        offsets.push_back(offset);
        offset += ((static_cast<uint32_t>(images[i].size()) + 4095) / 4096) * 4096;
    }
    for (size_t i = 0; i < images.size(); ++i) {
        out.resize(offsets[i], '\0');
        out += images[i];
    }
    return out;
}

std::string infoPlist(const std::string& bundleId, const std::string& version, const std::string& executable, const std::string& minimumSystem) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n"
           "\t<key>CFBundleExecutable</key>\n\t<string>" + executable + "</string>\n"
           "\t<key>CFBundleIdentifier</key>\n\t<string>" + bundleId + "</string>\n"
           "\t<key>CFBundleShortVersionString</key>\n\t<string>" + version + "</string>\n"
           "\t<key>CFBundleVersion</key>\n\t<string>" + version.substr(version.rfind('.') + 1) + "</string>\n"
           "\t<key>LSMinimumSystemVersion</key>\n\t<string>" + minimumSystem + "</string>\n"
           "\t<key>LSUIElement</key>\n\t<true/>\n"
           "</dict>\n</plist>\n";
}

std::string makeBundle(const std::string& dir, const std::string& bundleName, const std::string& bundleId, const std::string& version,
                       const std::string& executable, const std::string& binary) {
    namespace fs = std::filesystem;
    fs::path bundle = fs::path(dir) / bundleName;
    fs::create_directories(bundle / "Contents" / "MacOS");
    (void)nexus::writeFileAtomic(bundle / "Contents" / "Info.plist", infoPlist(bundleId, version, executable));
    (void)nexus::writeFileAtomic(bundle / "Contents" / "MacOS" / executable, binary);
    fs::permissions(bundle / "Contents" / "MacOS" / executable, fs::perms::owner_all, fs::perm_options::add);
    return bundle.string();
}

} // namespace nexus_test
