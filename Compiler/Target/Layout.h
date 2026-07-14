#pragma once

// The record-layout rule shared by sema, HIR lowering, and the backend:
// every field is aligned to its natural alignment capped at 8 bytes, fields
// of unknown or zero size occupy 8 bytes, and the aggregate is padded to the
// largest field alignment.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace Rux::Layout {
[[nodiscard]] constexpr std::uint64_t AlignUp(const std::uint64_t value, const std::uint64_t align) noexcept {
    return (value + align - 1) & ~(align - 1);
}

[[nodiscard]] constexpr int AlignUp(const int value, const int align) noexcept {
    return (value + align - 1) & ~(align - 1);
}

// Natural alignment of a field of `size` bytes, capped at 8.
[[nodiscard]] constexpr std::uint64_t FieldAlign(const std::uint64_t size) noexcept {
    return size > 0 ? std::min<std::uint64_t>(size, 8) : 1;
}

// Accumulate `fields` into a record, calling `sizeOf(field)` (returning
// std::optional<std::uint64_t>) for each. Returns {total size, alignment},
// or nullopt as soon as any field size is unknown.
template <typename Range, typename SizeFn>
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> FieldsSizeAndAlign(const Range &fields,
                                                                                        SizeFn &&sizeOf) {
    std::uint64_t offset = 0;
    std::uint64_t maxAlign = 1;
    for (const auto &field : fields) {
        const std::optional<std::uint64_t> fieldSize = sizeOf(field);
        if (!fieldSize) {
            return std::nullopt;
        }
        const std::uint64_t align = FieldAlign(*fieldSize);
        if (align > 1) {
            offset = AlignUp(offset, align);
        }
        offset += *fieldSize > 0 ? *fieldSize : 8;
        maxAlign = std::max(maxAlign, align);
    }
    return std::pair{AlignUp(offset, maxAlign), maxAlign};
}
} // namespace Rux::Layout
