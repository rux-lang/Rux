#pragma once

// Target-selected AAPCS64 fixed-argument policy. This is separate from the
// emitter so the Apple deviations can be checked without exposing the
// emitter's private LIR classification machinery.

#include "Target/Target.h"

#include <algorithm>

namespace Rux {
struct AArch64CallLayoutPolicy {
    bool compactStackArguments = false;
    bool alignWideGeneralArgumentsToEvenRegister = true;
    bool callerExtendsNarrowIntegers = false;

    [[nodiscard]] constexpr int StackAlignment(const int naturalAlignment) const noexcept {
        return compactStackArguments ? std::clamp(naturalAlignment, 1, 16) : std::clamp(naturalAlignment, 8, 16);
    }

    [[nodiscard]] constexpr int StackBytes(const int size) const noexcept {
        const int bytes = std::max(size, 1);
        return compactStackArguments ? bytes : (bytes + 7) / 8 * 8;
    }

    [[nodiscard]] constexpr unsigned FirstGeneralRegister(const unsigned next, const int alignment) const noexcept {
        if (alignment >= 16 && alignWideGeneralArgumentsToEvenRegister) {
            return next + next % 2;
        }
        return next;
    }
};

[[nodiscard]] constexpr AArch64CallLayoutPolicy AArch64CallPolicyFor(const Target::OS os) noexcept {
    if (os == Target::OS::MacOS || os == Target::OS::iOS) {
        return AArch64CallLayoutPolicy{
            .compactStackArguments = true,
            .alignWideGeneralArgumentsToEvenRegister = false,
            .callerExtendsNarrowIntegers = true,
        };
    }
    return {};
}
} // namespace Rux
