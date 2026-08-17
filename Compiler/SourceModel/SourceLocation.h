#pragma once

#include <cstdint>

namespace Rux {
/// A position in UTF-8 source text. Line and column are one-based for source diagnostics; the zero values are reserved
/// for diagnostics without a source.
struct SourceLocation {
    std::uint32_t line = 1;
    std::uint32_t column = 1; ///< UTF-8 byte offset within the line
    std::uint32_t offset = 0; ///< byte offset from the start of the file
};
} // namespace Rux
