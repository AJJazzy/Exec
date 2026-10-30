#include "nexus/roblox/macho.hpp"

#include "nexus/roblox/plist.hpp"

#include <fstream>

namespace nexus::roblox {

namespace {

constexpr uint32_t FAT_MAGIC = 0xcafebabe;
constexpr uint32_t FAT_MAGIC_64 = 0xcafebabf;
constexpr uint32_t MH_MAGIC = 0xfeedface;
constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;

constexpr uint32_t LC_CODE_SIGNATURE = 0x1d;
constexpr uint32_t LC_VERSION_MIN_MACOSX = 0x24;
constexpr uint32_t LC_BUILD_VERSION = 0x32;
constexpr uint32_t LC_LOAD_DYLIB = 0xc;
constexpr uint32_t LC_LOAD_WEAK_DYLIB = 0x80000018;
constexpr uint32_t LC_REEXPORT_DYLIB = 0x8000001f;

constexpr uint32_t CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0;
constexpr uint32_t CSMAGIC_CODEDIRECTORY = 0xfade0c02;
constexpr uint32_t CSMAGIC_EMBEDDED_ENTITLEMENTS = 0xfade7171;
constexpr uint32_t CSMAGIC_BLOBWRAPPER = 0xfade0b01;
constexpr uint32_t CSSLOT_CODEDIRECTORY = 0;

constexpr uint32_t CS_ADHOC = 0x2;
constexpr uint32_t CS_REQUIRE_LV = 0x2000;
constexpr uint32_t CS_RUNTIME = 0x10000;
constexpr uint32_t CS_LINKER_SIGNED = 0x20000;

constexpr uint64_t kMaxLoadCommandBytes = 16ull << 20;
constexpr uint64_t kMaxSignatureBytes = 64ull << 20;

// Random-access reader over a file or an in-memory buffer.
class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : file_(path, std::ios::binary) {
        if (file_) {
            file_.seekg(0, std::ios::end);
            size_ = static_cast<uint64_t>(file_.tellg());
        }
    }
    explicit Reader(const std::string& bytes) : bytes_(&bytes), size_(bytes.size()) {}

    bool ok() const { return bytes_ || static_cast<bool>(file_); }
    uint64_t size() const { return size_; }

    bool read(uint64_t offset, uint64_t length, std::string& out) {
        if (offset > size_ || length > size_ - offset) return false;
        if (bytes_) {
            out.assign(*bytes_, static_cast<size_t>(offset), static_cast<size_t>(length));
            return true;
        }
        out.resize(static_cast<size_t>(length));
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));
        file_.read(out.data(), static_cast<std::streamsize>(length));
        return static_cast<uint64_t>(file_.gcount()) == length;
    }

private:
    std::ifstream file_;
    const std::string* bytes_ = nullptr;
    uint64_t size_ = 0;
};

uint32_t le32(const std::string& b, size_t o) {
    return static_cast<uint32_t>(static_cast<unsigned char>(b[o])) | static_cast<uint32_t>(static_cast<unsigned char>(b[o + 1])) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(b[o + 2])) << 16 | static_cast<uint32_t>(static_cast<unsigned char>(b[o + 3])) << 24;
}

uint32_t be32(const std::string& b, size_t o) {
    return static_cast<uint32_t>(static_cast<unsigned char>(b[o])) << 24 | static_cast<uint32_t>(static_cast<unsigned char>(b[o + 1])) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(b[o + 2])) << 8 | static_cast<uint32_t>(static_cast<unsigned char>(b[o + 3]));
}

uint64_t be64(const std::string& b, size_t o) { return static_cast<uint64_t>(be32(b, o)) << 32 | be32(b, o + 4); }

std::string versionString(uint32_t v) {
    std::string s = std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 0xff);
    if (v & 0xff) s += "." + std::to_string(v & 0xff);
    return s;
}

std::string cString(const std::string& b, size_t offset) {
    if (offset >= b.size()) return {};
    size_t end = b.find('\0', offset);
    return b.substr(offset, (end == std::string::npos ? b.size() : end) - offset);
}

Result<CodeSignatureInfo> parseSignature(const std::string& blob) {
    CodeSignatureInfo info;
    if (blob.size() < 12 || be32(blob, 0) != CSMAGIC_EMBEDDED_SIGNATURE) return Error("macho.signature", "invalid signature superblob");
    info.present = true;
    uint32_t count = be32(blob, 8);
    if (count > 64 || 12 + static_cast<uint64_t>(count) * 8 > blob.size()) return Error("macho.signature", "invalid blob index");
    bool haveDirectory = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t type = be32(blob, 12 + i * 8);
        uint32_t offset = be32(blob, 16 + i * 8);
        if (offset + 8ull > blob.size()) continue;
        uint32_t magic = be32(blob, offset);
        uint32_t length = be32(blob, offset + 4);
        if (length < 8 || offset + static_cast<uint64_t>(length) > blob.size()) continue;
        if (magic == CSMAGIC_CODEDIRECTORY && type == CSSLOT_CODEDIRECTORY && !haveDirectory && length >= 44) {
            haveDirectory = true;
            info.codeDirectoryVersion = be32(blob, offset + 8);
            info.flags = be32(blob, offset + 12);
            uint32_t identOffset = be32(blob, offset + 20);
            if (identOffset < length) info.identifier = cString(blob.substr(offset, length), identOffset);
            if (info.codeDirectoryVersion >= 0x20200 && length >= 52) {
                uint32_t teamOffset = be32(blob, offset + 48);
                if (teamOffset && teamOffset < length) info.teamId = cString(blob.substr(offset, length), teamOffset);
            }
        } else if (magic == CSMAGIC_EMBEDDED_ENTITLEMENTS) {
            auto plist = parseXmlPlist(std::string_view(blob).substr(offset + 8, length - 8));
            if (plist.ok()) {
                for (const auto& [k, v] : plist.value().asObject()) info.entitlements[k] = v;
            }
        } else if (magic == CSMAGIC_BLOBWRAPPER) {
            info.hasCmsSignature = length > 8;
        }
    }
    info.adhoc = (info.flags & CS_ADHOC) != 0;
    info.linkerSigned = (info.flags & CS_LINKER_SIGNED) != 0;
    info.hardenedRuntime = (info.flags & CS_RUNTIME) != 0;
    info.libraryValidation = (info.flags & CS_REQUIRE_LV) != 0;
    return info;
}

Result<MachOSlice> parseSlice(Reader& reader, uint64_t base, uint64_t sliceSize) {
    MachOSlice slice;
    slice.offset = base;
    slice.size = sliceSize;
    std::string header;
    if (!reader.read(base, 32, header)) return Error("macho.truncated", "truncated Mach-O header");
    uint32_t magic = le32(header, 0);
    bool is64 = magic == MH_MAGIC_64;
    if (!is64 && magic != MH_MAGIC) return Error("macho.magic", "not a (little-endian) Mach-O image");
    slice.cpuType = le32(header, 4);
    slice.cpuSubtype = le32(header, 8) & 0x00ffffff;
    slice.fileType = le32(header, 12);
    slice.architecture = cpuTypeName(slice.cpuType, slice.cpuSubtype);
    uint32_t ncmds = le32(header, 16);
    uint32_t sizeofcmds = le32(header, 20);
    uint64_t headerSize = is64 ? 32 : 28;
    if (sizeofcmds > kMaxLoadCommandBytes || ncmds > 65536) return Error("macho.commands", "unreasonable load command table");
    std::string cmds;
    if (!reader.read(base + headerSize, sizeofcmds, cmds)) return Error("macho.truncated", "truncated load commands");

    uint64_t sigOffset = 0, sigSize = 0;
    size_t p = 0;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (p + 8 > cmds.size()) return Error("macho.commands", "load command overruns table");
        uint32_t cmd = le32(cmds, p);
        uint32_t size = le32(cmds, p + 4);
        if (size < 8 || p + size > cmds.size()) return Error("macho.commands", "invalid load command size");
        switch (cmd) {
        case LC_CODE_SIGNATURE:
            if (size >= 16) {
                sigOffset = le32(cmds, p + 8);
                sigSize = le32(cmds, p + 12);
            }
            break;
        case LC_BUILD_VERSION:
            if (size >= 24) {
                slice.minimumOS = versionString(le32(cmds, p + 12));
                slice.sdk = versionString(le32(cmds, p + 16));
            }
            break;
        case LC_VERSION_MIN_MACOSX:
            if (size >= 16) {
                slice.minimumOS = versionString(le32(cmds, p + 8));
                slice.sdk = versionString(le32(cmds, p + 12));
            }
            break;
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB: ++slice.linkedLibraryCount; break;
        default: break;
        }
        p += size;
    }

    if (sigSize > 0) {
        if (sigSize > kMaxSignatureBytes || sigOffset > sliceSize || sigSize > sliceSize - sigOffset)
            return Error("macho.signature", "code signature outside the image");
        std::string blob;
        if (!reader.read(base + sigOffset, sigSize, blob)) return Error("macho.truncated", "truncated code signature");
        auto sig = parseSignature(blob);
        if (!sig.ok()) return sig.error();
        slice.signature = sig.value();
    }
    return slice;
}

Result<MachOInfo> inspect(Reader& reader) {
    if (!reader.ok()) return Error("io.read", "cannot open file");
    MachOInfo info;
    std::string head;
    if (!reader.read(0, 8, head)) return Error("macho.truncated", "file too small to be a Mach-O image");
    uint32_t bigMagic = be32(head, 0);
    if (bigMagic == FAT_MAGIC || bigMagic == FAT_MAGIC_64) {
        bool fat64 = bigMagic == FAT_MAGIC_64;
        uint32_t n = be32(head, 4);
        if (n == 0 || n > 32) return Error("macho.fat", "implausible universal binary header"); // also rejects Java class files
        uint64_t entrySize = fat64 ? 32 : 20;
        std::string table;
        if (!reader.read(8, n * entrySize, table)) return Error("macho.truncated", "truncated universal header");
        info.universal = true;
        for (uint32_t i = 0; i < n; ++i) {
            size_t o = static_cast<size_t>(i * entrySize);
            uint64_t offset = fat64 ? be64(table, o + 8) : be32(table, o + 8);
            uint64_t size = fat64 ? be64(table, o + 16) : be32(table, o + 12);
            if (offset > reader.size() || size > reader.size() - offset) return Error("macho.fat", "slice outside the file");
            auto slice = parseSlice(reader, offset, size);
            if (!slice.ok()) return slice.error();
            info.slices.push_back(std::move(slice.value()));
        }
        return info;
    }
    auto slice = parseSlice(reader, 0, reader.size());
    if (!slice.ok()) return slice.error();
    info.slices.push_back(std::move(slice.value()));
    return info;
}

} // namespace

std::string cpuTypeName(uint32_t cpuType, uint32_t cpuSubtype) {
    switch (cpuType) {
    case 0x01000007: return "x86_64";
    case 0x0100000c: return cpuSubtype == 2 ? "arm64e" : "arm64";
    case 0x0200000c: return "arm64_32";
    case 7: return "i386";
    case 12: return "arm";
    default: return "cpu-" + std::to_string(cpuType);
    }
}

Result<MachOInfo> inspectMachO(const std::filesystem::path& path) {
    Reader reader(path);
    if (!reader.ok()) return Error("io.read", "cannot open " + path.string());
    return inspect(reader);
}

Result<MachOInfo> inspectMachOBytes(const std::string& bytes) {
    Reader reader(bytes);
    return inspect(reader);
}

std::vector<std::string> MachOInfo::architectures() const {
    std::vector<std::string> out;
    for (const auto& s : slices) out.push_back(s.architecture);
    return out;
}

json::Value CodeSignatureInfo::toJson() const {
    json::Object ent;
    for (const auto& [k, v] : entitlements) ent[k] = v;
    return json::Object{
        {"present", present}, {"identifier", identifier}, {"teamId", teamId}, {"flags", flags},
        {"codeDirectoryVersion", codeDirectoryVersion}, {"hasCmsSignature", hasCmsSignature}, {"adhoc", adhoc},
        {"linkerSigned", linkerSigned}, {"hardenedRuntime", hardenedRuntime}, {"libraryValidation", libraryValidation},
        {"entitlements", std::move(ent)},
    };
}

json::Value MachOSlice::toJson() const {
    return json::Object{
        {"architecture", architecture}, {"cpuType", cpuType}, {"cpuSubtype", cpuSubtype}, {"fileType", fileType},
        {"minimumOS", minimumOS}, {"sdk", sdk}, {"offset", offset}, {"size", size},
        {"linkedLibraryCount", linkedLibraryCount}, {"signature", signature.toJson()},
    };
}

json::Value MachOInfo::toJson() const {
    json::Array s;
    for (const auto& slice : slices) s.push_back(slice.toJson());
    return json::Object{{"universal", universal}, {"slices", std::move(s)}};
}

json::Value SecurityPosture::toJson() const {
    json::Array e;
    for (const auto& x : explanations) e.push_back(x);
    return json::Object{
        {"hardenedRuntime", hardenedRuntime}, {"getTaskAllow", getTaskAllow}, {"allowDyldEnvironment", allowDyldEnvironment},
        {"disableLibraryValidation", disableLibraryValidation}, {"allowUnsignedExecutableMemory", allowUnsignedExecutableMemory},
        {"disableExecutablePageProtection", disableExecutablePageProtection}, {"explanations", std::move(e)},
    };
}

SecurityPosture evaluateSecurityPosture(const CodeSignatureInfo& sig) {
    SecurityPosture p;
    auto ent = [&](const char* key) {
        auto it = sig.entitlements.find(key);
        return it != sig.entitlements.end() && it->second.asBool();
    };
    p.hardenedRuntime = sig.hardenedRuntime;
    p.getTaskAllow = ent("com.apple.security.get-task-allow");
    p.allowDyldEnvironment = ent("com.apple.security.cs.allow-dyld-environment-variables");
    p.disableLibraryValidation = ent("com.apple.security.cs.disable-library-validation");
    p.allowUnsignedExecutableMemory = ent("com.apple.security.cs.allow-unsigned-executable-memory");
    p.disableExecutablePageProtection = ent("com.apple.security.cs.disable-executable-page-protection");

    if (!sig.present) {
        p.explanations.push_back("The binary carries no code signature.");
        return p;
    }
    if (p.hardenedRuntime) {
        p.explanations.push_back("Hardened Runtime is enabled (CodeDirectory flag 0x10000).");
        p.explanations.push_back(p.getTaskAllow ? "get-task-allow is present: debuggers may attach."
                                                : "No get-task-allow entitlement: macOS refuses debugger/task-port attachment from other processes.");
        p.explanations.push_back(p.allowDyldEnvironment ? "DYLD_* environment variables are honoured."
                                                        : "No allow-dyld-environment-variables entitlement: macOS ignores DYLD_INSERT_LIBRARIES for this program.");
        p.explanations.push_back(p.disableLibraryValidation ? "Library validation is disabled: libraries signed by other teams may load."
                                                            : "Library validation is in force: only Apple-signed libraries or libraries signed by the same team can load.");
    } else {
        p.explanations.push_back("Hardened Runtime is not enabled.");
    }
    if (p.disableExecutablePageProtection)
        p.explanations.push_back("disable-executable-page-protection is present (typically required by JIT or code-patching engines inside the app itself).");
    if (!sig.teamId.empty()) p.explanations.push_back("Signed by Apple Developer Team ID " + sig.teamId + ".");
    return p;
}

} // namespace nexus::roblox
