#pragma once

#include <cstdint>

namespace Rux {
struct SourceLocation {
    std::uint32_t line = 1;   // 1-based
    std::uint32_t column = 1; // 1-based (UTF-8 byte offset in line)
    std::uint32_t offset = 0; // byte offset from start of file
};
} // namespace Rux
