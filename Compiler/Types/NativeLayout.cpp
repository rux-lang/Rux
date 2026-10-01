#include "Types/NativeLayout.h"

#include <algorithm>
#include <limits>

namespace Rux {
namespace {
std::optional<std::uint64_t> CheckedAlignUp(const std::uint64_t value, const std::uint64_t alignment) {
    if (alignment == 0 || value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
        return std::nullopt;
    }
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

std::optional<NativeLayout> ComputeNativeLayout(const TypeRef &type, const NativeMemberLayout &memberLayout) {
    NativeLayout layout;
    const auto addCase = [&](const std::uint64_t tag, std::optional<TypeRef> payload) {
        NativeLayout::Case nativeCase;
        nativeCase.tag = tag;
        if (payload) {
            const std::optional<SizeAndAlignment> payloadLayout = memberLayout(*payload);
            if (!payloadLayout) {
                return false;
            }
            nativeCase.layout = *payloadLayout;
        }
        nativeCase.payload = std::move(payload);
        layout.cases.push_back(std::move(nativeCase));
        return true;
    };

    if (type.IsSum()) {
        for (std::size_t index = 0; index < type.inner.size(); ++index) {
            if (!addCase(index, type.inner[index])) {
                return std::nullopt;
            }
        }
    }
    else if (type.IsOptional() && !type.IsNoneValue()) {
        if (!addCase(NativeAbsentTag, std::nullopt) || !addCase(NativePresentTag, type.inner[0])) {
            return std::nullopt;
        }
    }
    else if (type.IsFallible() && !type.IsIncompleteNative()) {
        if (!addCase(NativeSuccessTag, type.inner[0]) || !addCase(NativeFailureTag, type.inner[1])) {
            return std::nullopt;
        }
    }
    else {
        return std::nullopt;
    }

    // The tag is the default variant tag, a pointer-sized integer, so a native level and a variant share one shape.
    layout.tagOffset = 0;
    layout.tagSize = 8;
    layout.alignment = 8;
    std::uint64_t end = layout.tagSize;
    for (NativeLayout::Case &nativeCase : layout.cases) {
        const auto offset = CheckedAlignUp(layout.tagSize, nativeCase.layout.alignment);
        if (!offset || nativeCase.layout.size > std::numeric_limits<std::uint64_t>::max() - *offset) {
            return std::nullopt;
        }
        nativeCase.offset = *offset;
        end = std::max(end, *offset + nativeCase.layout.size);
        layout.alignment = std::max(layout.alignment, nativeCase.layout.alignment);
    }
    const auto size = CheckedAlignUp(end, layout.alignment);
    if (!size) {
        return std::nullopt;
    }
    layout.size = *size;
    return layout;
}
} // namespace Rux
