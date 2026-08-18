#include "Numeric/SoftwareFloat.h"

#include <cassert>
#include <compare>
#include <cstdint>

namespace Rux {
namespace {
[[nodiscard]] WideInteger OneAt(const std::uint32_t bit) noexcept {
    return WideInteger::FromUnsigned(1, WideInteger::MaxBits).ShiftedLeft(bit);
}

[[nodiscard]] bool IsNaN(const FloatClass classification) noexcept {
    return classification == FloatClass::QuietNaN || classification == FloatClass::SignalingNaN;
}

[[nodiscard]] FloatEncoding Quieted(const FloatEncoding &encoding) noexcept {
    UnpackedFloat value = UnpackFloat(encoding);
    value.classification = FloatClass::QuietNaN;
    return PackFloat(value);
}

[[nodiscard]] FloatEncoding FirstNaN(const FloatEncoding &left, const FloatEncoding &right) noexcept {
    return IsNaN(left.Classify()) ? Quieted(left) : Quieted(right);
}

[[nodiscard]] WideInteger IntegerSignificand(const UnpackedFloat &value) noexcept {
    return value.significand.ShiftedRight(FloatExtraPrecisionBits, false);
}

[[nodiscard]] WideInteger AddModulo(const WideInteger &left, const WideInteger &right,
                                    const WideInteger &modulus) noexcept {
    WideInteger sum = left.Added(right);
    if (sum.Compare(modulus, false) != std::strong_ordering::less) {
        sum = sum.Subtracted(modulus);
    }
    return sum;
}

[[nodiscard]] WideInteger MultiplyModulo(WideInteger left, const WideInteger &right,
                                         const WideInteger &modulus) noexcept {
    WideInteger result = WideInteger::Zero(WideInteger::MaxBits);
    for (std::uint32_t bit = 0; bit < right.Width(); ++bit) {
        if (right.BitSet(bit)) {
            result = AddModulo(result, left, modulus);
        }
        left = AddModulo(left, left, modulus);
    }
    return result;
}

[[nodiscard]] WideInteger ShiftModulo(const WideInteger &value, std::uint64_t amount,
                                      const WideInteger &modulus) noexcept {
    WideInteger result = value.Divided(modulus, false).remainder;
    WideInteger factor = WideInteger::FromUnsigned(2, WideInteger::MaxBits);
    while (amount != 0) {
        if ((amount & 1) != 0) {
            result = MultiplyModulo(result, factor, modulus);
        }
        amount >>= 1;
        if (amount != 0) {
            factor = MultiplyModulo(factor, factor, modulus);
        }
    }
    return result;
}

[[nodiscard]] WideIntegerProduct ShiftToDoubleWidth(const WideInteger &value, const std::uint32_t amount) noexcept {
    assert(amount < WideInteger::MaxBits);
    return WideIntegerProduct{
        .low = value.ShiftedLeft(amount),
        .high = value.ShiftedRight(WideInteger::MaxBits - amount, false),
    };
}

[[nodiscard]] std::strong_ordering CompareDoubleWidth(const WideIntegerProduct &left,
                                                      const WideIntegerProduct &right) noexcept {
    const std::strong_ordering high = left.high.Compare(right.high, false);
    return high == std::strong_ordering::equal ? left.low.Compare(right.low, false) : high;
}

[[nodiscard]] bool EqualDoubleWidth(const WideIntegerProduct &left, const WideIntegerProduct &right) noexcept {
    return left.low == right.low && left.high == right.high;
}
} // namespace

FloatEncoding DivideFloat(const FloatEncoding &leftEncoding, const FloatEncoding &rightEncoding) noexcept {
    assert(&leftEncoding.Format() == &rightEncoding.Format());
    const FloatFormat &format = leftEncoding.Format();
    const FloatClass leftClass = leftEncoding.Classify();
    const FloatClass rightClass = rightEncoding.Classify();
    if (IsNaN(leftClass) || IsNaN(rightClass)) {
        return FirstNaN(leftEncoding, rightEncoding);
    }
    if (leftClass == FloatClass::Invalid || rightClass == FloatClass::Invalid) {
        return FloatEncoding::QuietNaN(format);
    }

    const bool negative = leftEncoding.IsNegative() != rightEncoding.IsNegative();
    const bool leftZero = leftClass == FloatClass::Zero;
    const bool rightZero = rightClass == FloatClass::Zero;
    const bool leftInfinity = leftClass == FloatClass::Infinity;
    const bool rightInfinity = rightClass == FloatClass::Infinity;
    if ((leftZero && rightZero) || (leftInfinity && rightInfinity)) {
        return FloatEncoding::QuietNaN(format);
    }
    if (leftInfinity || rightZero) {
        return FloatEncoding::Infinity(format, negative);
    }
    if (leftZero || rightInfinity) {
        return FloatEncoding::Zero(format, negative);
    }

    const UnpackedFloat left = UnpackFloat(leftEncoding);
    const UnpackedFloat right = UnpackFloat(rightEncoding);
    WideInteger remainder = IntegerSignificand(left);
    const WideInteger divisor = IntegerSignificand(right);
    std::int64_t exponent = left.exponent - right.exponent;
    if (remainder.Compare(divisor, false) == std::strong_ordering::less) {
        remainder = remainder.ShiftedLeft(1);
        --exponent;
    }

    const std::uint32_t leading = format.precisionBits + FloatExtraPrecisionBits - 1;
    WideInteger quotient = WideInteger::Zero(WideInteger::MaxBits);
    for (std::uint32_t position = leading + 1; position > 0; --position) {
        const std::uint32_t bit = position - 1;
        if (remainder.Compare(divisor, false) != std::strong_ordering::less) {
            quotient = quotient.BitwiseOr(OneAt(bit));
            remainder = remainder.Subtracted(divisor);
        }
        if (bit != 0) {
            remainder = remainder.ShiftedLeft(1);
        }
    }
    if (!remainder.IsZero()) {
        quotient = quotient.BitwiseOr(WideInteger::FromUnsigned(1, WideInteger::MaxBits));
    }
    return PackFloat(UnpackedFloat{
        .format = &format,
        .classification = FloatClass::Normal,
        .negative = negative,
        .exponent = exponent,
        .significand = quotient,
    });
}

FloatEncoding RemainderFloat(const FloatEncoding &leftEncoding, const FloatEncoding &rightEncoding) noexcept {
    assert(&leftEncoding.Format() == &rightEncoding.Format());
    const FloatFormat &format = leftEncoding.Format();
    const FloatClass leftClass = leftEncoding.Classify();
    const FloatClass rightClass = rightEncoding.Classify();
    if (IsNaN(leftClass) || IsNaN(rightClass)) {
        return FirstNaN(leftEncoding, rightEncoding);
    }
    if (leftClass == FloatClass::Invalid || rightClass == FloatClass::Invalid || leftClass == FloatClass::Infinity ||
        rightClass == FloatClass::Zero) {
        return FloatEncoding::QuietNaN(format);
    }
    if (leftClass == FloatClass::Zero || rightClass == FloatClass::Infinity) {
        return leftEncoding;
    }

    const UnpackedFloat left = UnpackFloat(leftEncoding);
    const UnpackedFloat right = UnpackFloat(rightEncoding);
    if (left.exponent < right.exponent) {
        return leftEncoding;
    }
    WideInteger dividend = IntegerSignificand(left);
    const WideInteger divisor = IntegerSignificand(right);
    if (left.exponent == right.exponent && dividend.Compare(divisor, false) == std::strong_ordering::less) {
        return leftEncoding;
    }

    dividend = ShiftModulo(dividend, static_cast<std::uint64_t>(left.exponent - right.exponent), divisor);
    if (dividend.IsZero()) {
        return FloatEncoding::Zero(format, left.negative);
    }
    UnpackedFloat result{
        .format = &format,
        .classification = FloatClass::Normal,
        .negative = left.negative,
        .exponent = right.exponent,
        .significand = dividend.ShiftedLeft(FloatExtraPrecisionBits),
    };
    NormalizeFloat(result);
    return PackFloat(result);
}

FloatEncoding SquareRootFloat(const FloatEncoding &encoding) noexcept {
    const FloatFormat &format = encoding.Format();
    const FloatClass classification = encoding.Classify();
    if (IsNaN(classification)) {
        return Quieted(encoding);
    }
    if (classification == FloatClass::Invalid || (encoding.IsNegative() && classification != FloatClass::Zero)) {
        return FloatEncoding::QuietNaN(format);
    }
    if (classification == FloatClass::Infinity || classification == FloatClass::Zero) {
        return encoding;
    }

    const UnpackedFloat value = UnpackFloat(encoding);
    std::int64_t exponent = value.exponent / 2;
    if (value.exponent < 0 && value.exponent % 2 != 0) {
        --exponent;
    }
    const bool oddExponent = value.exponent - exponent * 2 != 0;
    const std::uint32_t scale = format.precisionBits + 5 + (oddExponent ? 1U : 0U);
    const WideIntegerProduct radicand = ShiftToDoubleWidth(IntegerSignificand(value), scale);

    WideInteger root = WideInteger::Zero(WideInteger::MaxBits);
    const std::uint32_t leading = format.precisionBits + FloatExtraPrecisionBits - 1;
    WideIntegerProduct square{WideInteger::Zero(WideInteger::MaxBits), WideInteger::Zero(WideInteger::MaxBits)};
    for (std::uint32_t position = leading + 1; position > 0; --position) {
        const WideInteger candidate = root.BitwiseOr(OneAt(position - 1));
        const WideIntegerProduct candidateSquare = candidate.MultipliedFull(candidate);
        if (CompareDoubleWidth(candidateSquare, radicand) != std::strong_ordering::greater) {
            root = candidate;
            square = candidateSquare;
        }
    }
    if (!EqualDoubleWidth(square, radicand)) {
        root = root.BitwiseOr(WideInteger::FromUnsigned(1, WideInteger::MaxBits));
    }
    return PackFloat(UnpackedFloat{
        .format = &format,
        .classification = FloatClass::Normal,
        .exponent = exponent,
        .significand = root,
    });
}
} // namespace Rux
