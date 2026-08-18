#pragma once

#include "Numeric/FloatFormat.h"
#include "Numeric/WideInteger.h"

#include <cstdint>
#include <vector>

namespace Rux {
enum class FloatClass : std::uint8_t {
    Zero,
    Subnormal,
    Normal,
    Infinity,
    QuietNaN,
    SignalingNaN,
    Invalid,
};

/// A raw value in one language float format. This owns the exact bits, including a negative zero and a NaN payload;
/// arithmetic is deliberately layered on top in the following software-float tasks.
class FloatEncoding {
public:
    [[nodiscard]] static FloatEncoding FromBits(const FloatFormat &format, const WideInteger &bits) noexcept;
    [[nodiscard]] static FloatEncoding Zero(const FloatFormat &format, bool negative = false) noexcept;
    [[nodiscard]] static FloatEncoding Infinity(const FloatFormat &format, bool negative = false) noexcept;
    [[nodiscard]] static FloatEncoding QuietNaN(const FloatFormat &format, bool negative = false) noexcept;
    [[nodiscard]] static FloatEncoding SignalingNaN(const FloatFormat &format, bool negative = false) noexcept;
    [[nodiscard]] static FloatEncoding MaxFinite(const FloatFormat &format, bool negative = false) noexcept;
    [[nodiscard]] static FloatEncoding MinPositiveNormal(const FloatFormat &format) noexcept;
    [[nodiscard]] static FloatEncoding MinPositiveSubnormal(const FloatFormat &format) noexcept;

    [[nodiscard]] const FloatFormat &Format() const noexcept {
        return *format;
    }

    [[nodiscard]] const WideInteger &Bits() const noexcept {
        return bits;
    }

    [[nodiscard]] bool IsNegative() const noexcept;
    [[nodiscard]] std::uint32_t ExponentField() const noexcept;
    [[nodiscard]] WideInteger SignificandField() const noexcept;
    [[nodiscard]] WideInteger Fraction() const noexcept;
    [[nodiscard]] FloatClass Classify() const noexcept;
    [[nodiscard]] bool IsCanonical() const noexcept;

    /// Little-endian target-independent storage bytes. The six padding bytes of `float80` are always zero.
    [[nodiscard]] std::vector<std::uint8_t> ToLittleEndianBytes() const;

private:
    FloatEncoding(const FloatFormat &inputFormat, WideInteger inputBits) noexcept;

    [[nodiscard]] static FloatEncoding Compose(const FloatFormat &format, bool negative, std::uint32_t exponent,
                                               const WideInteger &significand) noexcept;

    const FloatFormat *format;
    WideInteger bits;
};
} // namespace Rux
