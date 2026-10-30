#pragma once
// Converts between Luau source positions (0-based line, 0-based byte column), byte offsets and
// UTF-16 offsets (used by NSString/NSTextView in the macOS editor).

#include <cstdint>
#include <string_view>
#include <vector>

namespace nexus {

class SourceMap {
public:
    explicit SourceMap(std::string_view source);

    size_t lineCount() const { return lineStarts_.size(); }
    // Byte offset of a 0-based (line, column); clamped to the source size.
    size_t offsetOf(uint32_t line, uint32_t column) const;
    // 0-based line containing the byte offset.
    uint32_t lineOf(size_t offset) const;
    size_t lineStart(uint32_t line) const;

    // UTF-16 code-unit offset corresponding to a byte offset (computed lazily).
    size_t utf16Offset(size_t byteOffset) const;

private:
    std::string_view source_;
    std::vector<size_t> lineStarts_;
    mutable std::vector<uint32_t> utf16_; // utf16_[byte] for every byte offset (lazily built)
};

} // namespace nexus
