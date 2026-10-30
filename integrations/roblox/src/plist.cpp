#include "nexus/roblox/plist.hpp"

#include "nexus/library/script_library.hpp"
#include "nexus/platform/system.hpp"

#include <cctype>
#include <cstdlib>

namespace nexus::roblox {

namespace {

class XmlPlistParser {
public:
    explicit XmlPlistParser(std::string_view text) : s_(text) {}

    Result<json::Value> run() {
        skipProlog();
        std::string tag;
        bool selfClosing = false;
        if (!readOpenTag(tag, selfClosing)) return fail("expected <plist>");
        if (tag != "plist") return fail("root element is not <plist>");
        json::Value value;
        if (!parseValue(value, 0)) return fail(error_.empty() ? "invalid plist value" : error_);
        skipMisc();
        if (!expectClose("plist")) return fail("expected </plist>");
        return value;
    }

private:
    Error fail(const std::string& what) const { return Error("plist.parse", what + " at offset " + std::to_string(pos_)); }

    void skipWs() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool startsWith(std::string_view p) const { return s_.substr(pos_, p.size()) == p; }

    // Skips whitespace, comments and processing instructions.
    void skipMisc() {
        while (true) {
            skipWs();
            if (startsWith("<!--")) {
                size_t e = s_.find("-->", pos_);
                pos_ = e == std::string_view::npos ? s_.size() : e + 3;
            } else if (startsWith("<?")) {
                size_t e = s_.find("?>", pos_);
                pos_ = e == std::string_view::npos ? s_.size() : e + 2;
            } else {
                return;
            }
        }
    }

    void skipProlog() {
        while (true) {
            skipMisc();
            if (startsWith("<!DOCTYPE")) {
                size_t e = s_.find('>', pos_);
                pos_ = e == std::string_view::npos ? s_.size() : e + 1;
            } else {
                return;
            }
        }
    }

    bool readOpenTag(std::string& name, bool& selfClosing) {
        skipMisc();
        if (pos_ >= s_.size() || s_[pos_] != '<' || startsWith("</")) return false;
        size_t end = s_.find('>', pos_);
        if (end == std::string_view::npos) return false;
        std::string_view inner = s_.substr(pos_ + 1, end - pos_ - 1);
        selfClosing = !inner.empty() && inner.back() == '/';
        if (selfClosing) inner.remove_suffix(1);
        size_t sp = 0;
        while (sp < inner.size() && !std::isspace(static_cast<unsigned char>(inner[sp]))) ++sp;
        name = std::string(inner.substr(0, sp));
        pos_ = end + 1;
        return !name.empty();
    }

    bool expectClose(std::string_view name) {
        skipMisc();
        std::string close = "</" + std::string(name);
        if (!startsWith(close)) return false;
        size_t end = s_.find('>', pos_);
        if (end == std::string_view::npos) return false;
        pos_ = end + 1;
        return true;
    }

    static void appendUtf8(std::string& out, unsigned long cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }

    // Reads character data up to the closing tag `name`, decoding entities and CDATA.
    bool readText(std::string_view name, std::string& out) {
        std::string close = "</" + std::string(name);
        while (pos_ < s_.size()) {
            if (startsWith(close)) return expectClose(name);
            if (startsWith("<![CDATA[")) {
                size_t e = s_.find("]]>", pos_);
                if (e == std::string_view::npos) return false;
                out.append(s_.substr(pos_ + 9, e - pos_ - 9));
                pos_ = e + 3;
                continue;
            }
            char c = s_[pos_];
            if (c == '<') return false;
            if (c == '&') {
                size_t semi = s_.find(';', pos_);
                if (semi == std::string_view::npos || semi - pos_ > 10) return false;
                std::string_view ent = s_.substr(pos_ + 1, semi - pos_ - 1);
                if (ent == "amp") out += '&';
                else if (ent == "lt") out += '<';
                else if (ent == "gt") out += '>';
                else if (ent == "quot") out += '"';
                else if (ent == "apos") out += '\'';
                else if (!ent.empty() && ent[0] == '#') {
                    std::string num(ent.substr(1));
                    unsigned long cp = (!num.empty() && (num[0] == 'x' || num[0] == 'X')) ? std::strtoul(num.c_str() + 1, nullptr, 16)
                                                                                         : std::strtoul(num.c_str(), nullptr, 10);
                    if (cp == 0 || cp > 0x10FFFF) return false;
                    appendUtf8(out, cp);
                } else {
                    return false;
                }
                pos_ = semi + 1;
                continue;
            }
            out += c;
            ++pos_;
        }
        return false;
    }

    bool parseValue(json::Value& out, int depth) {
        if (depth > 64) {
            error_ = "plist nesting too deep";
            return false;
        }
        std::string tag;
        bool selfClosing = false;
        if (!readOpenTag(tag, selfClosing)) return false;
        if (tag == "true" || tag == "false") {
            if (!selfClosing && !expectClose(tag)) return false;
            out = json::Value(tag == "true");
            return true;
        }
        if (tag == "dict") {
            json::Object obj;
            if (selfClosing) {
                out = std::move(obj);
                return true;
            }
            while (true) {
                skipMisc();
                if (startsWith("</dict")) break;
                std::string ktag;
                bool kself = false;
                if (!readOpenTag(ktag, kself) || ktag != "key") {
                    error_ = "expected <key> in <dict>";
                    return false;
                }
                std::string key;
                if (!kself && !readText("key", key)) return false;
                json::Value v;
                if (!parseValue(v, depth + 1)) return false;
                obj[key] = std::move(v);
            }
            if (!expectClose("dict")) return false;
            out = std::move(obj);
            return true;
        }
        if (tag == "array") {
            json::Array arr;
            if (selfClosing) {
                out = std::move(arr);
                return true;
            }
            while (true) {
                skipMisc();
                if (startsWith("</array")) break;
                json::Value v;
                if (!parseValue(v, depth + 1)) return false;
                arr.push_back(std::move(v));
            }
            if (!expectClose("array")) return false;
            out = std::move(arr);
            return true;
        }
        if (tag == "string" || tag == "date" || tag == "data" || tag == "integer" || tag == "real") {
            std::string text;
            if (!selfClosing && !readText(tag, text)) return false;
            if (tag == "integer" || tag == "real") {
                char* end = nullptr;
                double d = std::strtod(text.c_str(), &end);
                if (end == text.c_str()) {
                    error_ = "invalid number";
                    return false;
                }
                out = json::Value(d);
            } else if (tag == "data") {
                std::string compact;
                for (char c : text)
                    if (!std::isspace(static_cast<unsigned char>(c))) compact += c;
                out = json::Value(compact);
            } else {
                out = json::Value(text);
            }
            return true;
        }
        error_ = "unsupported element <" + tag + ">";
        return false;
    }

    std::string_view s_;
    size_t pos_ = 0;
    std::string error_;
};

} // namespace

Result<json::Value> parseXmlPlist(std::string_view xml) { return XmlPlistParser(xml).run(); }

Result<json::Value> readPlistFile(const std::filesystem::path& path) {
    auto data = readFile(path, 16 << 20);
    if (!data.ok()) return data.error();
    if (data.value().rfind("bplist", 0) == 0) {
        auto xml = platform::propertyListAsXml(path.string());
        if (!xml) return Error("plist.binary", "binary property lists can only be decoded on macOS: " + path.string());
        return parseXmlPlist(*xml);
    }
    return parseXmlPlist(data.value());
}

} // namespace nexus::roblox
