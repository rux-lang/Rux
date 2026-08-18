#include "Numeric/SoftwareFloat.h"

#include <cassert>
#include <compare>

namespace Rux {
namespace {
[[nodiscard]] bool IsNaN(const FloatClass classification) noexcept {
    return classification == FloatClass::QuietNaN || classification == FloatClass::SignalingNaN;
}

[[nodiscard]] FloatEncoding Quieted(const FloatEncoding &encoding) noexcept {
    UnpackedFloat value = UnpackFloat(encoding);
    value.classification = FloatClass::QuietNaN;
    return PackFloat(value);
}

[[nodiscard]] FloatEncoding InvalidResult(const FloatEncoding &encoding) noexcept {
    return FloatEncoding::QuietNaN(encoding.Format(), encoding.IsNegative());
}

[[nodiscard]] FloatEncoding AddOrSubtract(const FloatEncoding &leftEncoding, const FloatEncoding &rightEncoding,
                                          const bool subtract) noexcept {
    assert(&leftEncoding.Format() == &rightEncoding.Format());
    const FloatFormat &format = leftEncoding.Format();
    const FloatClass leftClass = leftEncoding.Classify();
    const FloatClass rightClass = rightEncoding.Classify();

    if (IsNaN(leftClass)) {
        return Quieted(leftEncoding);
    }
    if (IsNaN(rightClass)) {
        return Quieted(rightEncoding);
    }
    if (leftClass == FloatClass::Invalid) {
        return InvalidResult(leftEncoding);
    }
    if (rightClass == FloatClass::Invalid) {
        return InvalidResult(rightEncoding);
    }

    const bool rightNegative = rightEncoding.IsNegative() != subtract;
    if (leftClass == FloatClass::Infinity && rightClass == FloatClass::Infinity) {
        if (leftEncoding.IsNegative() != rightNegative) {
            return FloatEncoding::QuietNaN(format);
        }
        return FloatEncoding::Infinity(format, leftEncoding.IsNegative());
    }
    if (leftClass == FloatClass::Infinity) {
        return FloatEncoding::Infinity(format, leftEncoding.IsNegative());
    }
    if (rightClass == FloatClass::Infinity) {
        return FloatEncoding::Infinity(format, rightNegative);
    }

    if (leftClass == FloatClass::Zero && rightClass == FloatClass::Zero) {
        return FloatEncoding::Zero(format, leftEncoding.IsNegative() && rightNegative);
    }
    if (leftClass == FloatClass::Zero) {
        UnpackedFloat right = UnpackFloat(rightEncoding);
        right.negative = rightNegative;
        return PackFloat(right);
    }
    if (rightClass == FloatClass::Zero) {
        return leftEncoding;
    }

    UnpackedFloat left = UnpackFloat(leftEncoding);
    UnpackedFloat right = UnpackFloat(rightEncoding);
    right.negative = rightNegative;
    if (left.exponent < right.exponent) {
        left.significand = ShiftRightSticky(
            left.significand, static_cast<std::uint64_t>(right.exponent - left.exponent) >= WideInteger::MaxBits
                                  ? WideInteger::MaxBits
                                  : static_cast<std::uint32_t>(right.exponent - left.exponent));
        left.exponent = right.exponent;
    }
    else if (right.exponent < left.exponent) {
        right.significand = ShiftRightSticky(
            right.significand, static_cast<std::uint64_t>(left.exponent - right.exponent) >= WideInteger::MaxBits
                                   ? WideInteger::MaxBits
                                   : static_cast<std::uint32_t>(left.exponent - right.exponent));
        right.exponent = left.exponent;
    }

    UnpackedFloat result = left;
    if (left.negative == right.negative) {
        result.significand = left.significand.Added(right.significand);
    }
    else {
        const std::strong_ordering order = left.significand.Compare(right.significand, false);
        if (order == std::strong_ordering::equal) {
            return FloatEncoding::Zero(format);
        }
        if (order == std::strong_ordering::greater) {
            result.significand = left.significand.Subtracted(right.significand);
        }
        else {
            result.significand = right.significand.Subtracted(left.significand);
            result.negative = right.negative;
        }
    }
    result.classification = FloatClass::Normal;
    NormalizeFloat(result);
    return PackFloat(result);
}
} // namespace

FloatEncoding AddFloat(const FloatEncoding &left, const FloatEncoding &right) noexcept {
    return AddOrSubtract(left, right, false);
}

FloatEncoding SubtractFloat(const FloatEncoding &left, const FloatEncoding &right) noexcept {
    return AddOrSubtract(left, right, true);
}
} // namespace Rux
