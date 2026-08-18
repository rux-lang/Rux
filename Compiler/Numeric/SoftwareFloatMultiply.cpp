#include "Numeric/SoftwareFloat.h"

#include <array>
#include <bit>
#include <cassert>
#include <compare>
#include <cstdint>

namespace Rux {
namespace {
constexpr std::uint32_t ExtendedBits = WideInteger::MaxBits * 2;
constexpr std::uint32_t ExtendedLimbBits = 32;
constexpr std::uint32_t ExtendedLimbs = ExtendedBits / ExtendedLimbBits;
constexpr std::uint32_t ExtendedLeadingBit = ExtendedBits - 2;

class ExtendedInteger {
public:
    [[nodiscard]] static ExtendedInteger FromWide(const WideInteger &value) noexcept {
        ExtendedInteger result;
        result.SetWide(value, 0);
        return result;
    }

    [[nodiscard]] static ExtendedInteger FromProduct(const WideIntegerProduct &value) noexcept {
        ExtendedInteger result;
        result.SetWide(value.low, 0);
        result.SetWide(value.high, WideInteger::MaxBits / ExtendedLimbBits);
        return result;
    }

    [[nodiscard]] bool IsZero() const noexcept {
        for (const std::uint32_t limb : limbs) {
            if (limb != 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool BitSet(const std::uint32_t bit) const noexcept {
        return bit < ExtendedBits && (limbs[bit / ExtendedLimbBits] & (std::uint32_t{1} << (bit % 32))) != 0;
    }

    [[nodiscard]] std::uint32_t LeadingBit() const noexcept {
        for (std::uint32_t index = ExtendedLimbs; index > 0; --index) {
            if (limbs[index - 1] != 0) {
                return (index - 1) * ExtendedLimbBits + (ExtendedLimbBits - 1 - std::countl_zero(limbs[index - 1]));
            }
        }
        return 0;
    }

    [[nodiscard]] ExtendedInteger ShiftedLeft(const std::uint32_t amount) const noexcept {
        ExtendedInteger result;
        if (amount >= ExtendedBits) {
            return result;
        }
        const std::uint32_t limbShift = amount / ExtendedLimbBits;
        const std::uint32_t bitShift = amount % ExtendedLimbBits;
        for (std::uint32_t destination = ExtendedLimbs; destination > limbShift; --destination) {
            const std::uint32_t source = destination - 1 - limbShift;
            result.limbs[destination - 1] = limbs[source] << bitShift;
            if (bitShift != 0 && source > 0) {
                result.limbs[destination - 1] |= limbs[source - 1] >> (ExtendedLimbBits - bitShift);
            }
        }
        return result;
    }

    [[nodiscard]] ExtendedInteger ShiftedRightSticky(const std::uint32_t amount) const noexcept {
        if (amount == 0) {
            return *this;
        }
        ExtendedInteger result;
        bool sticky = false;
        for (std::uint32_t bit = 0; bit < amount && bit < ExtendedBits; ++bit) {
            sticky = sticky || BitSet(bit);
        }
        if (amount < ExtendedBits) {
            const std::uint32_t limbShift = amount / ExtendedLimbBits;
            const std::uint32_t bitShift = amount % ExtendedLimbBits;
            for (std::uint32_t destination = 0; destination + limbShift < ExtendedLimbs; ++destination) {
                const std::uint32_t source = destination + limbShift;
                result.limbs[destination] = limbs[source] >> bitShift;
                if (bitShift != 0 && source + 1 < ExtendedLimbs) {
                    result.limbs[destination] |= limbs[source + 1] << (ExtendedLimbBits - bitShift);
                }
            }
        }
        if (sticky) {
            result.limbs[0] |= 1;
        }
        return result;
    }

    [[nodiscard]] ExtendedInteger Added(const ExtendedInteger &other) const noexcept {
        ExtendedInteger result;
        std::uint64_t carry = 0;
        for (std::uint32_t index = 0; index < ExtendedLimbs; ++index) {
            const std::uint64_t sum = static_cast<std::uint64_t>(limbs[index]) + other.limbs[index] + carry;
            result.limbs[index] = static_cast<std::uint32_t>(sum);
            carry = sum >> ExtendedLimbBits;
        }
        return result;
    }

    [[nodiscard]] ExtendedInteger Subtracted(const ExtendedInteger &other) const noexcept {
        ExtendedInteger result;
        std::uint64_t borrow = 0;
        for (std::uint32_t index = 0; index < ExtendedLimbs; ++index) {
            const std::uint64_t subtrahend = static_cast<std::uint64_t>(other.limbs[index]) + borrow;
            result.limbs[index] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(limbs[index]) - subtrahend);
            borrow = static_cast<std::uint64_t>(limbs[index]) < subtrahend;
        }
        return result;
    }

    [[nodiscard]] std::strong_ordering Compare(const ExtendedInteger &other) const noexcept {
        for (std::uint32_t index = ExtendedLimbs; index > 0; --index) {
            if (limbs[index - 1] != other.limbs[index - 1]) {
                return limbs[index - 1] < other.limbs[index - 1] ? std::strong_ordering::less
                                                                 : std::strong_ordering::greater;
            }
        }
        return std::strong_ordering::equal;
    }

    [[nodiscard]] WideInteger ToWide() const noexcept {
        std::array<std::uint64_t, WideInteger::MaxBits / 64> words{};
        for (std::size_t index = 0; index < words.size(); ++index) {
            words[index] =
                static_cast<std::uint64_t>(limbs[index * 2]) | (static_cast<std::uint64_t>(limbs[index * 2 + 1]) << 32);
        }
        return WideInteger::FromWords(words, WideInteger::MaxBits);
    }

private:
    void SetWide(const WideInteger &value, const std::uint32_t limbOffset) noexcept {
        for (std::uint32_t word = 0; word < WideInteger::MaxBits / 64; ++word) {
            const std::uint64_t bits = value.Word64(word);
            limbs[limbOffset + word * 2] = static_cast<std::uint32_t>(bits);
            limbs[limbOffset + word * 2 + 1] = static_cast<std::uint32_t>(bits >> 32);
        }
    }

    std::array<std::uint32_t, ExtendedLimbs> limbs{};
};

struct ExtendedFloat {
    ExtendedInteger significand;
    std::int64_t exponent = 0;
    bool negative = false;
};

[[nodiscard]] bool IsNaN(const FloatClass classification) noexcept {
    return classification == FloatClass::QuietNaN || classification == FloatClass::SignalingNaN;
}

[[nodiscard]] FloatEncoding Quieted(const FloatEncoding &encoding) noexcept {
    UnpackedFloat value = UnpackFloat(encoding);
    value.classification = FloatClass::QuietNaN;
    return PackFloat(value);
}

[[nodiscard]] FloatEncoding FirstNaN(const FloatEncoding &left, const FloatEncoding &right,
                                     const FloatEncoding *addend = nullptr) noexcept {
    if (IsNaN(left.Classify())) {
        return Quieted(left);
    }
    if (IsNaN(right.Classify())) {
        return Quieted(right);
    }
    if (addend && IsNaN(addend->Classify())) {
        return Quieted(*addend);
    }
    return FloatEncoding::QuietNaN(left.Format());
}

[[nodiscard]] ExtendedFloat NormalizeExtended(ExtendedInteger significand, const std::int64_t exponent,
                                              const bool negative) noexcept {
    const std::uint32_t leading = significand.LeadingBit();
    return ExtendedFloat{
        .significand = significand.ShiftedLeft(ExtendedLeadingBit - leading),
        .exponent = exponent,
        .negative = negative,
    };
}

[[nodiscard]] ExtendedFloat FiniteProduct(const UnpackedFloat &left, const UnpackedFloat &right) noexcept {
    const std::uint32_t precision = left.format->precisionBits;
    const WideInteger leftInteger = left.significand.ShiftedRight(FloatExtraPrecisionBits, false);
    const WideInteger rightInteger = right.significand.ShiftedRight(FloatExtraPrecisionBits, false);
    ExtendedInteger product = ExtendedInteger::FromProduct(leftInteger.MultipliedFull(rightInteger));
    const std::uint32_t productLeading = product.LeadingBit();
    const std::int64_t exponent = left.exponent + right.exponent + productLeading - 2 * (precision - 1);
    return NormalizeExtended(product, exponent, left.negative != right.negative);
}

[[nodiscard]] ExtendedFloat FiniteValue(const UnpackedFloat &value) noexcept {
    const WideInteger integer = value.significand.ShiftedRight(FloatExtraPrecisionBits, false);
    return NormalizeExtended(ExtendedInteger::FromWide(integer), value.exponent, value.negative);
}

[[nodiscard]] UnpackedFloat ToUnpacked(const ExtendedFloat &value, const FloatFormat &format) noexcept {
    const std::uint32_t actual = value.significand.LeadingBit();
    const std::uint32_t expected = format.precisionBits + FloatExtraPrecisionBits - 1;
    ExtendedInteger significand = value.significand;
    std::int64_t exponent = value.exponent + static_cast<std::int64_t>(actual) - ExtendedLeadingBit;
    if (actual > expected) {
        significand = significand.ShiftedRightSticky(actual - expected);
    }
    else if (actual < expected) {
        significand = significand.ShiftedLeft(expected - actual);
    }
    return UnpackedFloat{
        .format = &format,
        .classification = FloatClass::Normal,
        .negative = value.negative,
        .exponent = exponent,
        .significand = significand.ToWide(),
    };
}

[[nodiscard]] ExtendedFloat AddExtended(ExtendedFloat left, ExtendedFloat right) noexcept {
    if (left.exponent < right.exponent) {
        const std::uint64_t distance = static_cast<std::uint64_t>(right.exponent - left.exponent);
        left.significand = left.significand.ShiftedRightSticky(
            distance >= ExtendedBits ? ExtendedBits : static_cast<std::uint32_t>(distance));
        left.exponent = right.exponent;
    }
    else if (right.exponent < left.exponent) {
        const std::uint64_t distance = static_cast<std::uint64_t>(left.exponent - right.exponent);
        right.significand = right.significand.ShiftedRightSticky(
            distance >= ExtendedBits ? ExtendedBits : static_cast<std::uint32_t>(distance));
        right.exponent = left.exponent;
    }

    ExtendedFloat result = left;
    if (left.negative == right.negative) {
        result.significand = left.significand.Added(right.significand);
        return result;
    }
    const std::strong_ordering order = left.significand.Compare(right.significand);
    if (order == std::strong_ordering::equal) {
        result.significand = ExtendedInteger{};
        result.negative = false;
    }
    else if (order == std::strong_ordering::greater) {
        result.significand = left.significand.Subtracted(right.significand);
    }
    else {
        result.significand = right.significand.Subtracted(left.significand);
        result.negative = right.negative;
    }
    return result;
}
} // namespace

FloatEncoding MultiplyFloat(const FloatEncoding &leftEncoding, const FloatEncoding &rightEncoding) noexcept {
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
    if ((leftZero && rightInfinity) || (leftInfinity && rightZero)) {
        return FloatEncoding::QuietNaN(format);
    }
    if (leftInfinity || rightInfinity) {
        return FloatEncoding::Infinity(format, negative);
    }
    if (leftZero || rightZero) {
        return FloatEncoding::Zero(format, negative);
    }

    const ExtendedFloat product = FiniteProduct(UnpackFloat(leftEncoding), UnpackFloat(rightEncoding));
    return PackFloat(ToUnpacked(product, format));
}

FloatEncoding FusedMultiplyAddFloat(const FloatEncoding &leftEncoding, const FloatEncoding &rightEncoding,
                                    const FloatEncoding &addendEncoding) noexcept {
    assert(&leftEncoding.Format() == &rightEncoding.Format());
    assert(&leftEncoding.Format() == &addendEncoding.Format());
    const FloatFormat &format = leftEncoding.Format();
    const FloatClass leftClass = leftEncoding.Classify();
    const FloatClass rightClass = rightEncoding.Classify();
    const FloatClass addendClass = addendEncoding.Classify();
    if (IsNaN(leftClass) || IsNaN(rightClass) || IsNaN(addendClass)) {
        return FirstNaN(leftEncoding, rightEncoding, &addendEncoding);
    }
    if (leftClass == FloatClass::Invalid || rightClass == FloatClass::Invalid || addendClass == FloatClass::Invalid) {
        return FloatEncoding::QuietNaN(format);
    }

    const bool productNegative = leftEncoding.IsNegative() != rightEncoding.IsNegative();
    const bool productZero = leftClass == FloatClass::Zero || rightClass == FloatClass::Zero;
    const bool productInfinity = leftClass == FloatClass::Infinity || rightClass == FloatClass::Infinity;
    if (productZero && productInfinity) {
        return FloatEncoding::QuietNaN(format);
    }
    if (productInfinity) {
        if (addendClass == FloatClass::Infinity && productNegative != addendEncoding.IsNegative()) {
            return FloatEncoding::QuietNaN(format);
        }
        return FloatEncoding::Infinity(format, productNegative);
    }
    if (addendClass == FloatClass::Infinity) {
        return addendEncoding;
    }
    if (productZero) {
        return AddFloat(FloatEncoding::Zero(format, productNegative), addendEncoding);
    }

    ExtendedFloat result = FiniteProduct(UnpackFloat(leftEncoding), UnpackFloat(rightEncoding));
    if (addendClass != FloatClass::Zero) {
        result = AddExtended(result, FiniteValue(UnpackFloat(addendEncoding)));
        if (result.significand.IsZero()) {
            return FloatEncoding::Zero(format);
        }
    }
    return PackFloat(ToUnpacked(result, format));
}
} // namespace Rux
