#pragma once

#include "Target/Target.h"

#include <cstdint>

namespace Rux::System {
/// Whether CPUID leaf 1 permits executing XGETBV. Kept separate so detection never probes disabled state.
[[nodiscard]] constexpr bool CanReadXcr0(const std::uint32_t leaf1Ecx) noexcept {
    constexpr auto required = (1U << 26) | (1U << 27); // XSAVE and OSXSAVE
    return (leaf1Ecx & required) == required;
}

/// Decode supported x86 features from captured hardware/OS state. Missing leaves must not supply feature bits.
/// AVX512 denotes Foundation only; it does not imply BW, VL, DQ, or another independently enumerated extension.
[[nodiscard]] constexpr Target::CpuFeatures
DecodeX86CpuFeatures(const std::uint32_t maximumLeaf, const std::uint32_t leaf1Ecx, const std::uint32_t leaf1Edx,
                     const std::uint32_t leaf7Ebx, const std::uint64_t xcr0) noexcept {
    using namespace Target;
    CpuFeatures features = CpuFeature::None;
    if (maximumLeaf < 1) {
        return features;
    }
    if (leaf1Edx & (1U << 26))
        features |= CpuFeature::SSE2;
    if (leaf1Ecx & (1U << 0))
        features |= CpuFeature::SSE3;
    if (leaf1Ecx & (1U << 9))
        features |= CpuFeature::SSSE3;
    if (leaf1Ecx & (1U << 19))
        features |= CpuFeature::SSE41;
    if (leaf1Ecx & (1U << 20))
        features |= CpuFeature::SSE42;
    const bool avx = (leaf1Ecx & (1U << 28)) != 0 && CanReadXcr0(leaf1Ecx) && (xcr0 & 0x6U) == 0x6U;
    if (avx) {
        features |= CpuFeature::AVX;
        if (maximumLeaf >= 7) {
            if (leaf7Ebx & (1U << 5))
                features |= CpuFeature::AVX2;
            if ((leaf7Ebx & (1U << 16)) != 0 && (xcr0 & 0xE6U) == 0xE6U)
                features |= CpuFeature::AVX512;
        }
    }
    return features;
}
} // namespace Rux::System
