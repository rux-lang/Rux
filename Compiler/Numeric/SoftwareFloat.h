#pragma once

#include "Numeric/FloatEncoding.h"

#include <cstdint>

namespace Rux {
/// Three low working bits retain the information needed for round-to-nearest, ties-to-even after alignment and
/// normalization. The normalized leading bit is therefore at `precisionBits + 2`.
constexpr std::uint32_t FloatExtraPrecisionBits = 3;

struct UnpackedFloat {
    const FloatFormat *format = nullptr;
    FloatClass classification = FloatClass::Zero;
    bool negative = false;
    /// The unbiased exponent of the normalized leading bit.
    std::int64_t exponent = 0;
    /// A 512-bit working significand with guard, round, and sticky bits in positions 2, 1, and 0.
    WideInteger significand = WideInteger::Zero(WideInteger::MaxBits);
    /// A NaN's fraction field, kept separately so unpacking and packing preserve its payload exactly.
    WideInteger nanPayload = WideInteger::Zero(WideInteger::MaxBits);

    [[nodiscard]] bool IsFinite() const noexcept {
        return classification == FloatClass::Zero || classification == FloatClass::Subnormal ||
               classification == FloatClass::Normal;
    }
};

/// Logical right shift that ORs every discarded one bit into bit zero.
[[nodiscard]] WideInteger ShiftRightSticky(const WideInteger &value, std::uint32_t amount) noexcept;

/// Decode a raw value into the common normalized representation. Subnormals and float80 pseudo-denormals become
/// ordinary normalized finite values; NaN sign and payload remain intact.
[[nodiscard]] UnpackedFloat UnpackFloat(const FloatEncoding &encoding) noexcept;

/// Move a finite nonzero significand's leading bit to the format's working position, adjusting its exponent and
/// retaining all discarded information in the sticky bit.
void NormalizeFloat(UnpackedFloat &value) noexcept;

/// Round to nearest with ties to even, encode overflow as infinity and gradual underflow as a subnormal or signed
/// zero, and canonicalize float80's noncanonical raw forms.
[[nodiscard]] FloatEncoding PackFloat(UnpackedFloat value) noexcept;

/// Add two values of the same format with round-to-nearest, ties-to-even semantics. NaNs are quieted while retaining
/// the first NaN operand's sign and payload.
[[nodiscard]] FloatEncoding AddFloat(const FloatEncoding &left, const FloatEncoding &right) noexcept;

/// Subtract two values of the same format with the same special-value and rounding rules as `AddFloat`.
[[nodiscard]] FloatEncoding SubtractFloat(const FloatEncoding &left, const FloatEncoding &right) noexcept;

/// Multiply two values of the same format with one final rounding step.
[[nodiscard]] FloatEncoding MultiplyFloat(const FloatEncoding &left, const FloatEncoding &right) noexcept;

/// Compute `left * right + addend` with a single final rounding step.
[[nodiscard]] FloatEncoding FusedMultiplyAddFloat(const FloatEncoding &left, const FloatEncoding &right,
                                                  const FloatEncoding &addend) noexcept;

/// Divide two values of the same format with one final ties-to-even rounding step.
[[nodiscard]] FloatEncoding DivideFloat(const FloatEncoding &left, const FloatEncoding &right) noexcept;

/// Return `left - trunc(left / right) * right`, with the dividend's sign on every nonzero result and signed zero.
[[nodiscard]] FloatEncoding RemainderFloat(const FloatEncoding &left, const FloatEncoding &right) noexcept;

/// Compute the correctly rounded square root of one value.
[[nodiscard]] FloatEncoding SquareRootFloat(const FloatEncoding &value) noexcept;
} // namespace Rux
