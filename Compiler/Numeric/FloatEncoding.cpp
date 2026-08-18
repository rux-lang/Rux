#include "Numeric/FloatEncoding.h"

#include <algorithm>

namespace Rux {
namespace {
[[nodiscard]] WideInteger OneAt(const std::uint32_t bit, const std::uint32_t width) noexcept {
    return WideInteger::FromUnsigned(1, width).ShiftedLeft(bit);
}

[[nodiscard]] WideInteger IntegerBit(const FloatFormat &format) noexcept {
    return format.explicitIntegerBit ? OneAt(format.FractionBits(), format.valueBits)
                                     : WideInteger::Zero(format.valueBits);
}

[[nodiscard]] WideInteger WithSign(WideInteger value, const FloatFormat &format, const bool negative) noexcept {
    return negative ? value.BitwiseOr(OneAt(format.valueBits - 1, format.valueBits)) : value;
}
} // namespace

FloatEncoding::FloatEncoding(const FloatFormat &inputFormat, WideInteger inputBits) noexcept
    : format(&inputFormat)
    , bits(inputBits.Truncated(inputFormat.valueBits)) {
}

FloatEncoding FloatEncoding::FromBits(const FloatFormat &format, const WideInteger &bits) noexcept {
    return FloatEncoding(format, bits);
}

FloatEncoding FloatEncoding::Compose(const FloatFormat &format, const bool negative, const std::uint32_t exponent,
                                     const WideInteger &significand) noexcept {
    WideInteger raw =
        WideInteger::FromUnsigned(exponent, format.valueBits)
            .ShiftedLeft(format.SignificandFieldBits())
            .BitwiseOr(significand.Truncated(format.SignificandFieldBits()).Extended(format.valueBits, false));
    return FloatEncoding(format, WithSign(raw, format, negative));
}

FloatEncoding FloatEncoding::Zero(const FloatFormat &format, const bool negative) noexcept {
    return Compose(format, negative, 0, WideInteger::Zero(format.valueBits));
}

FloatEncoding FloatEncoding::Infinity(const FloatFormat &format, const bool negative) noexcept {
    return Compose(format, negative, format.MaxExponentField(), IntegerBit(format));
}

FloatEncoding FloatEncoding::QuietNaN(const FloatFormat &format, const bool negative) noexcept {
    const WideInteger payload = IntegerBit(format).BitwiseOr(OneAt(format.FractionBits() - 1, format.valueBits));
    return Compose(format, negative, format.MaxExponentField(), payload);
}

FloatEncoding FloatEncoding::SignalingNaN(const FloatFormat &format, const bool negative) noexcept {
    const WideInteger payload = IntegerBit(format).BitwiseOr(WideInteger::FromUnsigned(1, format.valueBits));
    return Compose(format, negative, format.MaxExponentField(), payload);
}

FloatEncoding FloatEncoding::MaxFinite(const FloatFormat &format, const bool negative) noexcept {
    const WideInteger significand =
        WideInteger::AllOnes(format.SignificandFieldBits()).Extended(format.valueBits, false);
    return Compose(format, negative, format.MaxExponentField() - 1, significand);
}

FloatEncoding FloatEncoding::MinPositiveNormal(const FloatFormat &format) noexcept {
    return Compose(format, false, 1, IntegerBit(format));
}

FloatEncoding FloatEncoding::MinPositiveSubnormal(const FloatFormat &format) noexcept {
    return Compose(format, false, 0, WideInteger::FromUnsigned(1, format.valueBits));
}

bool FloatEncoding::IsNegative() const noexcept {
    return bits.BitSet(format->valueBits - 1);
}

std::uint32_t FloatEncoding::ExponentField() const noexcept {
    return static_cast<std::uint32_t>(bits.ShiftedRight(format->SignificandFieldBits(), false)
                                          .Truncated(format->exponentBits)
                                          .ToUnsigned()
                                          .value_or(0));
}

WideInteger FloatEncoding::SignificandField() const noexcept {
    return bits.Truncated(format->SignificandFieldBits());
}

WideInteger FloatEncoding::Fraction() const noexcept {
    return bits.Truncated(format->FractionBits());
}

FloatClass FloatEncoding::Classify() const noexcept {
    const std::uint32_t exponent = ExponentField();
    const WideInteger significand = SignificandField();
    const WideInteger fraction = Fraction();
    const bool integerBit = !format->explicitIntegerBit || significand.BitSet(format->FractionBits());

    if (exponent == 0) {
        if (significand.IsZero()) {
            return FloatClass::Zero;
        }
        return !format->explicitIntegerBit || !integerBit ? FloatClass::Subnormal : FloatClass::Normal;
    }
    if (exponent != format->MaxExponentField()) {
        return integerBit ? FloatClass::Normal : FloatClass::Invalid;
    }
    if (!integerBit) {
        return FloatClass::Invalid;
    }
    if (fraction.IsZero()) {
        return FloatClass::Infinity;
    }
    return fraction.BitSet(format->FractionBits() - 1) ? FloatClass::QuietNaN : FloatClass::SignalingNaN;
}

bool FloatEncoding::IsCanonical() const noexcept {
    if (!format->explicitIntegerBit) {
        return true;
    }
    const std::uint32_t exponent = ExponentField();
    const bool integerBit = SignificandField().BitSet(format->FractionBits());
    return exponent == 0 ? !integerBit : integerBit;
}

std::vector<std::uint8_t> FloatEncoding::ToLittleEndianBytes() const {
    std::vector<std::uint8_t> result(format->storageBytes, 0);
    const std::uint32_t valueBytes = (format->valueBits + 7) / 8;
    for (std::uint32_t index = 0; index < valueBytes; ++index) {
        result[index] = static_cast<std::uint8_t>((bits.Word64(index / 8) >> ((index % 8) * 8)) & 0xFF);
    }
    return result;
}
} // namespace Rux
