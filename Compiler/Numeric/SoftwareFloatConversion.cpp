#include "Numeric/SoftwareFloat.h"

#include <compare>
#include <cstdint>

namespace Rux {
namespace {
[[nodiscard]] bool IsNaNOrInvalid(const FloatClass classification) noexcept {
    return classification == FloatClass::QuietNaN || classification == FloatClass::SignalingNaN ||
           classification == FloatClass::Invalid;
}

[[nodiscard]] std::strong_ordering CompareMagnitude(const UnpackedFloat &left, const UnpackedFloat &right) noexcept {
    if (left.exponent != right.exponent) {
        return left.exponent < right.exponent ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    const std::uint32_t commonPrecision = left.format->precisionBits > right.format->precisionBits
                                            ? left.format->precisionBits
                                            : right.format->precisionBits;
    const WideInteger leftSignificand = left.significand.ShiftedLeft(commonPrecision - left.format->precisionBits);
    const WideInteger rightSignificand = right.significand.ShiftedLeft(commonPrecision - right.format->precisionBits);
    return leftSignificand.Compare(rightSignificand, false);
}

[[nodiscard]] WideInteger NormalizedIntegerSignificand(const WideInteger &magnitude,
                                                       const FloatFormat &format) noexcept {
    const std::uint32_t actual = magnitude.Width() - magnitude.CountLeadingZeros() - 1;
    const std::uint32_t expected = format.precisionBits - 1;
    if (actual < expected) {
        return magnitude.Extended(WideInteger::MaxBits, false).ShiftedLeft(expected - actual + FloatExtraPrecisionBits);
    }
    if (actual > expected) {
        const std::uint32_t difference = actual - expected;
        return difference < FloatExtraPrecisionBits
                 ? magnitude.Extended(WideInteger::MaxBits, false).ShiftedLeft(FloatExtraPrecisionBits - difference)
                 : ShiftRightSticky(magnitude.Extended(WideInteger::MaxBits, false),
                                    difference - FloatExtraPrecisionBits);
    }
    return magnitude.Extended(WideInteger::MaxBits, false).ShiftedLeft(FloatExtraPrecisionBits);
}
} // namespace

FloatComparison CompareFloat(const FloatEncoding &leftEncoding, const FloatEncoding &rightEncoding) noexcept {
    const FloatClass leftClass = leftEncoding.Classify();
    const FloatClass rightClass = rightEncoding.Classify();
    if (IsNaNOrInvalid(leftClass) || IsNaNOrInvalid(rightClass)) {
        return FloatComparison::Unordered;
    }
    if (leftClass == FloatClass::Zero && rightClass == FloatClass::Zero) {
        return FloatComparison::Equal;
    }
    if (leftClass == FloatClass::Zero) {
        return rightEncoding.IsNegative() ? FloatComparison::Greater : FloatComparison::Less;
    }
    if (rightClass == FloatClass::Zero) {
        return leftEncoding.IsNegative() ? FloatComparison::Less : FloatComparison::Greater;
    }
    if (leftEncoding.IsNegative() != rightEncoding.IsNegative()) {
        return leftEncoding.IsNegative() ? FloatComparison::Less : FloatComparison::Greater;
    }
    if (leftClass == FloatClass::Infinity || rightClass == FloatClass::Infinity) {
        if (leftClass == rightClass) {
            return FloatComparison::Equal;
        }
        const bool leftGreater = leftClass == FloatClass::Infinity;
        if (leftEncoding.IsNegative()) {
            return leftGreater ? FloatComparison::Less : FloatComparison::Greater;
        }
        return leftGreater ? FloatComparison::Greater : FloatComparison::Less;
    }

    const std::strong_ordering magnitude = CompareMagnitude(UnpackFloat(leftEncoding), UnpackFloat(rightEncoding));
    if (magnitude == std::strong_ordering::equal) {
        return FloatComparison::Equal;
    }
    const bool less = magnitude == std::strong_ordering::less;
    if (leftEncoding.IsNegative()) {
        return less ? FloatComparison::Greater : FloatComparison::Less;
    }
    return less ? FloatComparison::Less : FloatComparison::Greater;
}

FloatEncoding IntegerToFloat(const WideInteger &value, const bool sourceSigned, const FloatFormat &format) noexcept {
    const bool negative = sourceSigned && value.IsNegative();
    const WideInteger magnitude = value.Magnitude(sourceSigned).Extended(WideInteger::MaxBits, false);
    if (magnitude.IsZero()) {
        return FloatEncoding::Zero(format);
    }
    const std::int64_t exponent = magnitude.Width() - magnitude.CountLeadingZeros() - 1;
    return PackFloat(UnpackedFloat{
        .format = &format,
        .classification = FloatClass::Normal,
        .negative = negative,
        .exponent = exponent,
        .significand = NormalizedIntegerSignificand(magnitude, format),
    });
}

FloatToIntegerResult FloatToInteger(const FloatEncoding &encoding, const std::uint32_t targetWidth,
                                    const bool targetSigned) noexcept {
    const std::uint32_t width = targetWidth == 0                   ? 1
                              : targetWidth > WideInteger::MaxBits ? WideInteger::MaxBits
                                                                   : targetWidth;
    const FloatClass classification = encoding.Classify();
    if (classification == FloatClass::Infinity || IsNaNOrInvalid(classification)) {
        return {WideInteger::Zero(width), FloatConversionError::NonFinite};
    }
    if (classification == FloatClass::Zero) {
        return {WideInteger::Zero(width), FloatConversionError::None};
    }

    const UnpackedFloat value = UnpackFloat(encoding);
    if (value.exponent < 0) {
        return {WideInteger::Zero(width), FloatConversionError::None};
    }
    if (value.exponent >= WideInteger::MaxBits || (!targetSigned && value.negative)) {
        return {WideInteger::Zero(width), FloatConversionError::OutOfRange};
    }

    const WideInteger significand = value.significand.ShiftedRight(FloatExtraPrecisionBits, false);
    WideInteger magnitude =
        value.exponent >= static_cast<std::int64_t>(value.format->precisionBits - 1)
            ? significand.ShiftedLeft(static_cast<std::uint32_t>(value.exponent - (value.format->precisionBits - 1)))
            : significand.ShiftedRight(static_cast<std::uint32_t>(value.format->precisionBits - 1 - value.exponent),
                                       false);

    const WideInteger limit =
        value.negative ? WideInteger::MinMagnitude(width, targetSigned) : WideInteger::MaxValue(width, targetSigned);
    if (magnitude.Compare(limit.Extended(WideInteger::MaxBits, false), false) == std::strong_ordering::greater) {
        return {WideInteger::Zero(width), FloatConversionError::OutOfRange};
    }
    magnitude = magnitude.Truncated(width);
    return {value.negative ? magnitude.Negated() : magnitude, FloatConversionError::None};
}

FloatEncoding ConvertFloat(const FloatEncoding &encoding, const FloatFormat &targetFormat) noexcept {
    UnpackedFloat value = UnpackFloat(encoding);
    value.format = &targetFormat;
    if (!value.IsFinite() || value.classification == FloatClass::Zero) {
        if (value.classification == FloatClass::Invalid) {
            value.classification = FloatClass::QuietNaN;
        }
        return PackFloat(value);
    }

    const std::uint32_t sourceLeading = encoding.Format().precisionBits + FloatExtraPrecisionBits - 1;
    const std::uint32_t targetLeading = targetFormat.precisionBits + FloatExtraPrecisionBits - 1;
    value.significand = sourceLeading < targetLeading
                          ? value.significand.ShiftedLeft(targetLeading - sourceLeading)
                          : ShiftRightSticky(value.significand, sourceLeading - targetLeading);
    return PackFloat(value);
}
} // namespace Rux
