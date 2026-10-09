#pragma once

// The record-layout rule shared by sema, HIR lowering, and the backend: every field is placed at the next multiple of
// its own alignment, and the record is padded to its largest field alignment. A field's alignment is the one its
// type states -- a primitive's comes from the primitive catalog, which gives the 128-bit and wider primitives 16 --
// rather than one derived here from its size, so a `{ uint8, int128 }` puts the `int128` at 16 everywhere it is
// laid out.

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

/// The alignment a pointer-like value of `size` bytes takes: its own size, which is a power of two for every such
/// value. Only scalars that are not primitives answer from here -- pointers, references and function addresses.
[[nodiscard]] constexpr std::uint64_t WordAlign(const std::uint64_t size) noexcept {
    return size > 0 ? std::min<std::uint64_t>(size, 8) : 1;
}

/// Accumulate `fields` into a record, calling `layoutOf(field)` (returning
/// std::optional<std::pair<std::uint64_t, std::uint64_t>> of {size, alignment}) for each.
/// Returns {total size, alignment}, or nullopt as soon as any field layout is unknown.
template <typename Range, typename LayoutFn>
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> FieldsSizeAndAlign(const Range &fields,
                                                                                        LayoutFn &&layoutOf) {
    std::uint64_t offset = 0;
    std::uint64_t maxAlign = 1;
    for (const auto &field : fields) {
        const std::optional<std::pair<std::uint64_t, std::uint64_t>> fieldLayout = layoutOf(field);
        if (!fieldLayout) {
            return std::nullopt;
        }
        const auto [fieldSize, align] = *fieldLayout;
        if (align > 1) {
            offset = AlignUp(offset, align);
        }
        offset += fieldSize;
        maxAlign = std::max(maxAlign, align);
    }
    return std::pair{AlignUp(offset, maxAlign), maxAlign};
}
} // namespace Rux::Layout
