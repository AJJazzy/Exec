#include "nexus/compiler/source_map.hpp"

#include <algorithm>

namespace nexus {

SourceMap::SourceMap(std::string_view source) : source_(source) {
    lineStarts_.push_back(0);
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\n') lineStarts_.push_back(i + 1);
    }
}

size_t SourceMap::lineStart(uint32_t line) const {
    if (line >= lineStarts_.size()) return source_.size();
    return lineStarts_[line];
}

size_t SourceMap::offsetOf(uint32_t line, uint32_t column) const {
    return std::min(lineStart(line) + column, source_.size());
}

uint32_t SourceMap::lineOf(size_t offset) const {
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    return static_cast<uint32_t>(std::distance(lineStarts_.begin(), it) - 1);
}

size_t SourceMap::utf16Offset(size_t byteOffset) const {
    if (utf16_.empty()) {
        utf16_.resize(source_.size() + 1);
        uint32_t units = 0;
        size_t i = 0;
        while (i < source_.size()) {
            unsigned char c = static_cast<unsigned char>(source_[i]);
            size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            if (i + len > source_.size()) len = source_.size() - i;
            // Bytes inside a multi-byte sequence map to the start of that character.
            for (size_t k = 0; k < len; ++k) utf16_[i + k] = units;
            units += len == 4 ? 2 : 1; // supplementary planes use a surrogate pair
            i += len;
        }
        utf16_[source_.size()] = units;
    }
    return utf16_[std::min(byteOffset, source_.size())];
}

} // namespace nexus
