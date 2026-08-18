#include "Numeric/SoftwareFloat.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace Rux {
namespace {
[[nodiscard]] WideInteger OneAt(const std::uint32_t bit) noexcept {
    return WideInteger::FromUnsigned(1, WideInteger::MaxBits).ShiftedLeft(bit);
}

[[nodiscard]] FloatEncoding Compose(const FloatFormat &format, const bool negative, const std::uint32_t exponent,
                                    const WideInteger &significand) noexcept {
    WideInteger raw =
        WideInteger::FromUnsigned(exponent, format.valueBits)
            .ShiftedLeft(format.SignificandFieldBits())
            .BitwiseOr(significand.Truncated(format.SignificandFieldBits()).Extended(format.valueBits, false));
    if (negative) {
        raw = raw.BitwiseOr(WideInteger::FromUnsigned(1, format.valueBits).ShiftedLeft(format.valueBits - 1));
    }
    return FloatEncoding::FromBits(format, raw);
}

[[nodiscard]] std::int64_t SaturatingAdd(const std::int64_t value, const std::int64_t amount) noexcept {
    if (amount > 0 && value > std::numeric_limits<std::int64_t>::max() - amount) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (amount < 0 && value < std::numeric_limits<std::int64_t>::min() - amount) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return value + amount;
}

[[nodiscard]] WideInteger RoundedSignificand(const WideInteger &working) noexcept {
    WideInteger rounded = working.ShiftedRight(FloatExtraPrecisionBits, false);
    const bool guard = working.BitSet(2);
    const bool round = working.BitSet(1);
    const bool sticky = working.BitSet(0);
    if (guard && (round || sticky || rounded.BitSet(0))) {
        rounded = rounded.Added(WideInteger::FromUnsigned(1, WideInteger::MaxBits));
    }
    return rounded;
}
} // namespace

WideInteger ShiftRightSticky(const WideInteger &value, const std::uint32_t amount) noexcept {
    if (amount == 0) {
        return value.Extended(WideInteger::MaxBits, false);
    }
    bool sticky = false;
    for (std::uint32_t bit = 0; bit < std::min(amount, value.Width()); ++bit) {
        sticky = sticky || value.BitSet(bit);
    }
    WideInteger shifted = value.Extended(WideInteger::MaxBits, false).ShiftedRight(amount, false);
    if (sticky) {
        shifted = shifted.BitwiseOr(WideInteger::FromUnsigned(1, WideInteger::MaxBits));
    }
    return shifted;
}

UnpackedFloat UnpackFloat(const FloatEncoding &encoding) noexcept {
    UnpackedFloat result;
    result.format = &encoding.Format();
    result.classification = encoding.Classify();
    result.negative = encoding.IsNegative();
    if (result.classification == FloatClass::QuietNaN || result.classification == FloatClass::SignalingNaN) {
        result.nanPayload = encoding.Fraction().Extended(WideInteger::MaxBits, false);
        return result;
    }
    if (result.classification == FloatClass::Infinity || result.classification == FloatClass::Zero ||
        result.classification == FloatClass::Invalid) {
        return result;
    }

    const FloatFormat &format = *result.format;
    WideInteger significand = encoding.SignificandField().Extended(WideInteger::MaxBits, false);
    if (!format.explicitIntegerBit && encoding.ExponentField() != 0) {
        significand = significand.BitwiseOr(OneAt(format.FractionBits()));
    }
    result.exponent = encoding.ExponentField() == 0
                        ? 1 - format.exponentBias
                        : static_cast<std::int64_t>(encoding.ExponentField()) - format.exponentBias;
    result.significand = significand.ShiftedLeft(FloatExtraPrecisionBits);
    NormalizeFloat(result);
    return result;
}

void NormalizeFloat(UnpackedFloat &value) noexcept {
    if (!value.format || value.significand.IsZero()) {
        value.classification = FloatClass::Zero;
        value.significand = WideInteger::Zero(WideInteger::MaxBits);
        return;
    }
    const std::uint32_t expected = value.format->precisionBits + FloatExtraPrecisionBits - 1;
    const std::uint32_t actual = value.significand.Width() - value.significand.CountLeadingZeros() - 1;
    if (actual < expected) {
        const std::uint32_t shift = expected - actual;
        value.significand = value.significand.ShiftedLeft(shift);
        value.exponent = SaturatingAdd(value.exponent, -static_cast<std::int64_t>(shift));
    }
    else if (actual > expected) {
        const std::uint32_t shift = actual - expected;
        value.significand = ShiftRightSticky(value.significand, shift);
        value.exponent = SaturatingAdd(value.exponent, shift);
    }
    value.classification = FloatClass::Normal;
}

FloatEncoding PackFloat(UnpackedFloat value) noexcept {
    assert(value.format != nullptr);
    const FloatFormat &format = *value.format;
    if (value.classification == FloatClass::Infinity) {
        return FloatEncoding::Infinity(format, value.negative);
    }
    if (value.classification == FloatClass::Invalid) {
        return FloatEncoding::QuietNaN(format, value.negative);
    }
    if (value.classification == FloatClass::QuietNaN || value.classification == FloatClass::SignalingNaN) {
        WideInteger payload = value.nanPayload.Truncated(format.FractionBits()).Extended(format.valueBits, false);
        if (value.classification == FloatClass::QuietNaN) {
            payload = payload.BitwiseOr(
                WideInteger::FromUnsigned(1, format.valueBits).ShiftedLeft(format.FractionBits() - 1));
        }
        else {
            payload = payload.BitwiseAnd(
                WideInteger::FromUnsigned(1, format.valueBits).ShiftedLeft(format.FractionBits() - 1).BitwiseNot());
            if (payload.IsZero()) {
                payload = WideInteger::FromUnsigned(1, format.valueBits);
            }
        }
        if (format.explicitIntegerBit) {
            payload =
                payload.BitwiseOr(WideInteger::FromUnsigned(1, format.valueBits).ShiftedLeft(format.FractionBits()));
        }
        return Compose(format, value.negative, format.MaxExponentField(), payload);
    }
    if (value.classification == FloatClass::Zero || value.significand.IsZero()) {
        return FloatEncoding::Zero(format, value.negative);
    }

    NormalizeFloat(value);
    const std::int64_t minimumNormal = 1 - static_cast<std::int64_t>(format.exponentBias);
    const std::int64_t maximumNormal = static_cast<std::int64_t>(format.MaxExponentField() - 1) - format.exponentBias;
    if (value.exponent > maximumNormal) {
        return FloatEncoding::Infinity(format, value.negative);
    }
    if (value.exponent < minimumNormal) {
        const std::uint64_t distance = value.exponent < minimumNormal - WideInteger::MaxBits
                                         ? WideInteger::MaxBits
                                         : static_cast<std::uint64_t>(minimumNormal - value.exponent);
        value.significand = ShiftRightSticky(value.significand, distance >= WideInteger::MaxBits
                                                                    ? WideInteger::MaxBits
                                                                    : static_cast<std::uint32_t>(distance));
        value.exponent = minimumNormal;
    }

    WideInteger rounded = RoundedSignificand(value.significand);
    if (rounded.BitSet(format.precisionBits)) {
        rounded = rounded.ShiftedRight(1, false);
        value.exponent = SaturatingAdd(value.exponent, 1);
    }
    if (value.exponent > maximumNormal) {
        return FloatEncoding::Infinity(format, value.negative);
    }
    if (rounded.IsZero()) {
        return FloatEncoding::Zero(format, value.negative);
    }

    const bool normal = value.exponent > minimumNormal || rounded.BitSet(format.precisionBits - 1);
    const std::uint32_t exponentField = normal ? static_cast<std::uint32_t>(value.exponent + format.exponentBias) : 0;
    WideInteger field = rounded;
    if (!format.explicitIntegerBit && normal) {
        field = field.BitwiseAnd(OneAt(format.precisionBits - 1).BitwiseNot());
    }
    return Compose(format, value.negative, exponentField, field);
}
} // namespace Rux
