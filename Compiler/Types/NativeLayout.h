#pragma once

#include "Types/Type.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace Rux {
/// The size and alignment of one type, as a layout source reports it.
struct SizeAndAlignment {
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
};

/// The one layout contract for a native sum, optional, or fallible. Every native level is a tag followed by storage
/// for its widest case, laid out the way a variant with the default tag lays out a single-field case: the tag at
/// offset zero and each case's payload at the tag's end aligned for that payload. A level never rewrites a nested
/// native value's tag, so the active payload is always a complete value of its own type at its case's `offset`, which
/// borrowed access and pass-through rely on.
struct NativeLayout {
    /// One alternative: a sum member, the absent or present level of an optional, or a fallible channel. `payload` is
    /// the stored type, absent for an optional's absent case, and `tag` is the value the tag holds for it.
    struct Case {
        std::uint64_t tag = 0;
        std::optional<TypeRef> payload;
        SizeAndAlignment layout;
        std::uint64_t offset = 0;
    };

    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
    std::uint64_t tagOffset = 0;
    std::uint64_t tagSize = 8;
    std::vector<Case> cases;
};

/// The tag value of an optional's absent and present cases, and of a fallible's success and failure channels. A sum's
/// member takes its index in canonical member order.
inline constexpr std::uint64_t NativeAbsentTag = 0;
inline constexpr std::uint64_t NativePresentTag = 1;
inline constexpr std::uint64_t NativeSuccessTag = 0;
inline constexpr std::uint64_t NativeFailureTag = 1;

/// A source of member layouts. It answers nullopt for a member it cannot lay out, which makes the native layout
/// unavailable too.
using NativeMemberLayout = std::function<std::optional<SizeAndAlignment>(const TypeRef &)>;

/// The layout of the native type `type` given its members' layouts, or nullopt when `type` is not a native sum,
/// optional, or fallible, or a member cannot be laid out. A zero-sized member reserves no bytes.
[[nodiscard]] std::optional<NativeLayout> ComputeNativeLayout(const TypeRef &type,
                                                              const NativeMemberLayout &memberLayout);
} // namespace Rux
