#pragma once

#include <cstdint>

namespace Rux {
/// A position in UTF-8 source text. Line and column are one-based for source diagnostics; the zero values are reserved
/// for diagnostics without a source.
struct SourceLocation {
    std::uint32_t line = 1;
    std::uint32_t column = 1; ///< UTF-8 byte offset within the line
    std::uint32_t offset = 0; ///< byte offset from the start of the file

    bool operator==(const SourceLocation &) const = default;
};

/// A half-open byte range in one source file. `start` names the first byte and `end` names the first byte after it,
/// which lets adjacent ranges meet without overlapping and makes the byte length `end.offset - start.offset`.
struct SourceRange {
    SourceLocation start;
    SourceLocation end;

    bool operator==(const SourceRange &) const = default;

    [[nodiscard]] bool Empty() const noexcept {
        return start.offset == end.offset;
    }

    [[nodiscard]] std::uint32_t Length() const noexcept {
        return end.offset - start.offset;
    }
};
} // namespace Rux
