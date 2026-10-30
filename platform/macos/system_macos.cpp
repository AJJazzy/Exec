// macOS implementations using public system APIs only:
//   libproc / sysctl        read-only process enumeration and Rosetta detection
//   Security.framework      static code-signature validation of files on disk
//   Seatbelt (sandbox_init) self-sandboxing of the nexus-runner helper
//   Launch Services         locating installed applications by bundle identifier
//   CoreFoundation          property-list decoding
#include "nexus/platform/system.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <Security/Security.h>
#include <libproc.h>
#include <sandbox.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <fstream>
#include <iterator>

#ifndef P_TRANSLATED
#define P_TRANSLATED 0x00020000
#endif

namespace nexus::platform {

namespace {

// Owns a CoreFoundation reference.
template <class T>
class CFRef {
public:
    CFRef() = default;
    explicit CFRef(T ref) : ref_(ref) {}
    ~CFRef() {
        if (ref_) CFRelease(ref_);
    }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    T get() const { return ref_; }
    T* out() { return &ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    T ref_ = nullptr;
};

std::string toStdString(CFStringRef s) {
    if (!s) return {};
    CFIndex length = CFStringGetLength(s);
    CFIndex max = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

CFRef<CFStringRef> makeCFString(const std::string& s) {
    return CFRef<CFStringRef>(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()),
                                                      static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false));
}

std::string securityError(OSStatus status) {
    CFRef<CFStringRef> message(SecCopyErrorMessageString(status, nullptr));
    std::string text = message ? toStdString(message.get()) : std::string("unknown error");
    return text + " (OSStatus " + std::to_string(status) + ")";
}

std::string sysctlString(const char* name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::string out(size, '\0');
    if (sysctlbyname(name, out.data(), &size, nullptr, 0) != 0) return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

} // namespace

std::vector<ProcessEntry> listProcesses() {
    std::vector<ProcessEntry> out;
    int count = proc_listallpids(nullptr, 0);
    if (count <= 0) return out;
    std::vector<pid_t> pids(static_cast<size_t>(count) + 64);
    count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < count && i < static_cast<int>(pids.size()); ++i) {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (pids[i] > 0 && proc_pidpath(pids[i], path, sizeof(path)) > 0) out.push_back({pids[i], path});
    }
    return out;
}

std::optional<bool> isProcessTranslated(pid_t pid) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return std::nullopt;
    return (info.kp_proc.p_flag & P_TRANSLATED) != 0;
}

SignatureCheck verifyCodeSignature(const std::string& path, const std::string& requiredTeamId) {
    SignatureCheck result;
    result.supported = true;

    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        result.error = "file not found: " + path;
        return result;
    }
    CFRef<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                                static_cast<CFIndex>(path.size()), S_ISDIR(st.st_mode)));
    CFRef<SecStaticCodeRef> code;
    OSStatus status = SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, code.out());
    if (status != errSecSuccess) {
        result.error = securityError(status);
        return result;
    }

    CFRef<CFDictionaryRef> info;
    if (SecCodeCopySigningInformation(code.get(), kSecCSSigningInformation, info.out()) == errSecSuccess && info) {
        auto ident = static_cast<CFStringRef>(CFDictionaryGetValue(info.get(), kSecCodeInfoIdentifier));
        auto team = static_cast<CFStringRef>(CFDictionaryGetValue(info.get(), kSecCodeInfoTeamIdentifier));
        auto flags = static_cast<CFNumberRef>(CFDictionaryGetValue(info.get(), kSecCodeInfoFlags));
        result.signedCode = ident != nullptr;
        result.identifier = toStdString(ident);
        result.teamIdentifier = toStdString(team);
        if (flags) {
            int64_t value = 0;
            CFNumberGetValue(flags, kCFNumberSInt64Type, &value);
            result.flags = static_cast<uint32_t>(value);
        }
    }

    CFRef<SecRequirementRef> requirement;
    if (!requiredTeamId.empty()) {
        std::string text = "anchor apple generic and certificate leaf[subject.OU] = \"" + requiredTeamId + "\"";
        CFRef<CFStringRef> reqText = makeCFString(text);
        status = SecRequirementCreateWithString(reqText.get(), kSecCSDefaultFlags, requirement.out());
        if (status != errSecSuccess) {
            result.error = "invalid requirement: " + securityError(status);
            return result;
        }
    }

    // Resources are not re-hashed so that checking a large bundle stays fast; the executable code
    // and the signature itself are fully validated.
    status = SecStaticCodeCheckValidity(code.get(), kSecCSCheckAllArchitectures | kSecCSDoNotValidateResources, requirement.get());
    result.valid = status == errSecSuccess;
    if (!result.valid) result.error = securityError(status);
    return result;
}

SandboxStatus applyComputeOnlySandbox() {
    SandboxStatus s;
    s.mechanism = "macOS Seatbelt sandbox_init(kSBXProfilePureComputation)";
    char* error = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // The named profiles are deprecated in the SDK headers but remain the only public, documented
    // way for a plain executable to sandbox itself after start-up.
    int rc = sandbox_init(kSBXProfilePureComputation, SANDBOX_NAMED, &error);
    if (rc == 0) {
        s.applied = true;
        s.detail = "no file system writes, no network, no new processes";
    } else {
        s.detail = error ? error : "sandbox_init failed";
        if (error) sandbox_free_error(error);
    }
#pragma clang diagnostic pop
    return s;
}

std::vector<std::string> applicationPathsForBundleIdentifier(const std::string& bundleId) {
    std::vector<std::string> out;
    CFRef<CFStringRef> id = makeCFString(bundleId);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFRef<CFArrayRef> urls(LSCopyApplicationURLsForBundleIdentifier(id.get(), nullptr));
#pragma clang diagnostic pop
    if (!urls) return out;
    for (CFIndex i = 0; i < CFArrayGetCount(urls.get()); ++i) {
        auto url = static_cast<CFURLRef>(CFArrayGetValueAtIndex(urls.get(), i));
        char buf[PATH_MAX];
        if (url && CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(buf), sizeof(buf))) out.emplace_back(buf);
    }
    return out;
}

std::optional<std::string> propertyListAsXml(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CFRef<CFDataRef> data(CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.data()), static_cast<CFIndex>(bytes.size())));
    CFRef<CFPropertyListRef> plist(CFPropertyListCreateWithData(nullptr, data.get(), kCFPropertyListImmutable, nullptr, nullptr));
    if (!plist) return std::nullopt;
    CFRef<CFDataRef> xml(CFPropertyListCreateData(nullptr, plist.get(), kCFPropertyListXMLFormat_v1_0, 0, nullptr));
    if (!xml) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(xml.get())), static_cast<size_t>(CFDataGetLength(xml.get())));
}

std::string operatingSystemDescription() {
    std::string version = sysctlString("kern.osproductversion");
    std::string arch = machineArchitecture();
    int translated = 0;
    size_t size = sizeof(translated);
    if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 && translated == 1) arch += ", under Rosetta 2";
    return "macOS " + (version.empty() ? std::string("unknown") : version) + " (" + arch + ")";
}

} // namespace nexus::platform
