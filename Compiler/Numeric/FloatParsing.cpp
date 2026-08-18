#include "Numeric/FloatParsing.h"

#include "Numeric/FloatLiteral.h"
#include "Numeric/SoftwareFloat.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <vector>

namespace Rux {
namespace {
class BigUnsigned {
public:
    [[nodiscard]] static BigUnsigned Parse(const std::string_view digits, const unsigned base) {
        BigUnsigned result;
        for (const char character : digits) {
            const unsigned digit =
                std::isdigit(static_cast<unsigned char>(character))
                    ? static_cast<unsigned>(character - '0')
                    : static_cast<unsigned>(std::tolower(static_cast<unsigned char>(character)) - 'a' + 10);
            result.MultiplySmall(base);
            result.AddSmall(digit);
        }
        return result;
    }

    [[nodiscard]] bool IsZero() const noexcept {
        return limbs.empty();
    }

    [[nodiscard]] std::uint64_t BitLength() const noexcept {
        if (limbs.empty()) {
            return 0;
        }
        std::uint32_t top = limbs.back();
        std::uint32_t bits = 0;
        while (top != 0) {
            ++bits;
            top >>= 1;
        }
        return (limbs.size() - 1) * 32 + bits;
    }

    void MultiplySmall(const std::uint32_t factor) {
        std::uint64_t carry = 0;
        for (std::uint32_t &limb : limbs) {
            const std::uint64_t product = static_cast<std::uint64_t>(limb) * factor + carry;
            limb = static_cast<std::uint32_t>(product);
            carry = product >> 32;
        }
        if (carry != 0) {
            limbs.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void AddSmall(const std::uint32_t addend) {
        if (limbs.empty()) {
            if (addend != 0) {
                limbs.push_back(addend);
            }
            return;
        }
        std::uint64_t carry = addend;
        for (std::uint32_t &limb : limbs) {
            const std::uint64_t sum = static_cast<std::uint64_t>(limb) + carry;
            limb = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
            if (carry == 0) {
                return;
            }
        }
        limbs.push_back(static_cast<std::uint32_t>(carry));
    }

    [[nodiscard]] BigUnsigned ShiftedLeft(const std::uint64_t amount) const {
        if (IsZero()) {
            return {};
        }
        const std::size_t limbShift = static_cast<std::size_t>(amount / 32);
        const std::uint32_t bitShift = amount % 32;
        BigUnsigned result;
        result.limbs.assign(limbShift + limbs.size() + (bitShift == 0 ? 0 : 1), 0);
        for (std::size_t index = 0; index < limbs.size(); ++index) {
            result.limbs[index + limbShift] |= limbs[index] << bitShift;
            if (bitShift != 0) {
                result.limbs[index + limbShift + 1] |= limbs[index] >> (32 - bitShift);
            }
        }
        result.Normalize();
        return result;
    }

    [[nodiscard]] int Compare(const BigUnsigned &other) const noexcept {
        if (limbs.size() != other.limbs.size()) {
            return limbs.size() < other.limbs.size() ? -1 : 1;
        }
        for (std::size_t index = limbs.size(); index > 0; --index) {
            if (limbs[index - 1] != other.limbs[index - 1]) {
                return limbs[index - 1] < other.limbs[index - 1] ? -1 : 1;
            }
        }
        return 0;
    }

    void Subtract(const BigUnsigned &other) noexcept {
        std::uint64_t borrow = 0;
        for (std::size_t index = 0; index < limbs.size(); ++index) {
            const std::uint64_t subtrahend = (index < other.limbs.size() ? other.limbs[index] : 0) + borrow;
            const std::uint64_t current = limbs[index];
            limbs[index] = static_cast<std::uint32_t>(current - subtrahend);
            borrow = current < subtrahend;
        }
        Normalize();
    }

    [[nodiscard]] WideInteger ToWide() const noexcept {
        std::vector<std::uint64_t> words((std::min<std::size_t>(limbs.size(), 16) + 1) / 2, 0);
        for (std::size_t index = 0; index < limbs.size() && index < 16; ++index) {
            words[index / 2] |= static_cast<std::uint64_t>(limbs[index]) << ((index % 2) * 32);
        }
        return WideInteger::FromWords(words, WideInteger::MaxBits);
    }

private:
    void Normalize() noexcept {
        while (!limbs.empty() && limbs.back() == 0) {
            limbs.pop_back();
        }
    }

    std::vector<std::uint32_t> limbs;
};

[[nodiscard]] WideInteger OneAt(const std::uint32_t bit) noexcept {
    return WideInteger::FromUnsigned(1, WideInteger::MaxBits).ShiftedLeft(bit);
}

[[nodiscard]] FloatEncoding RoundRatio(BigUnsigned numerator, BigUnsigned denominator, const bool negative,
                                       const FloatFormat &format) {
    if (numerator.IsZero()) {
        return FloatEncoding::Zero(format, negative);
    }
    const std::int64_t bitDifference =
        static_cast<std::int64_t>(numerator.BitLength()) - static_cast<std::int64_t>(denominator.BitLength());
    std::int64_t exponent = bitDifference;
    if ((bitDifference >= 0 && numerator.Compare(denominator.ShiftedLeft(bitDifference)) < 0) ||
        (bitDifference < 0 && numerator.ShiftedLeft(-bitDifference).Compare(denominator) < 0)) {
        --exponent;
    }

    BigUnsigned remainder;
    BigUnsigned divisor;
    if (exponent >= 0) {
        remainder = std::move(numerator);
        divisor = denominator.ShiftedLeft(exponent);
    }
    else {
        remainder = numerator.ShiftedLeft(-exponent);
        divisor = std::move(denominator);
    }

    const std::uint32_t leading = format.precisionBits + FloatExtraPrecisionBits - 1;
    WideInteger quotient = WideInteger::Zero(WideInteger::MaxBits);
    for (std::uint32_t position = leading + 1; position > 0; --position) {
        if (remainder.Compare(divisor) >= 0) {
            quotient = quotient.BitwiseOr(OneAt(position - 1));
            remainder.Subtract(divisor);
        }
        if (position > 1) {
            remainder = remainder.ShiftedLeft(1);
        }
    }
    if (!remainder.IsZero()) {
        quotient = quotient.BitwiseOr(WideInteger::FromUnsigned(1, WideInteger::MaxBits));
    }
    return PackFloat(UnpackedFloat{.format = &format,
                                   .classification = FloatClass::Normal,
                                   .negative = negative,
                                   .exponent = exponent,
                                   .significand = quotient});
}

[[nodiscard]] std::optional<FloatEncoding> ParseHexFloat(std::string_view literal, const FloatFormat &format) {
    bool negative = false;
    if (!literal.empty() && (literal.front() == '+' || literal.front() == '-')) {
        negative = literal.front() == '-';
        literal.remove_prefix(1);
    }
    if (!literal.starts_with("0x") && !literal.starts_with("0X")) {
        return std::nullopt;
    }
    literal.remove_prefix(2);
    const std::size_t marker = literal.find_first_of("pP");
    if (marker == std::string_view::npos) {
        return std::nullopt;
    }
    std::string digits;
    std::size_t fractionalDigits = 0;
    bool afterPoint = false;
    for (const char character : literal.substr(0, marker)) {
        if (character == '.') {
            if (afterPoint) {
                return std::nullopt;
            }
            afterPoint = true;
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(character))) {
            return std::nullopt;
        }
        digits.push_back(character);
        fractionalDigits += afterPoint;
    }
    std::int64_t binaryExponent = 0;
    const std::string_view exponentText = literal.substr(marker + 1);
    const auto [end, error] =
        std::from_chars(exponentText.data(), exponentText.data() + exponentText.size(), binaryExponent);
    if (error != std::errc{} || end != exponentText.data() + exponentText.size()) {
        return std::nullopt;
    }
    BigUnsigned integer = BigUnsigned::Parse(digits, 16);
    BigUnsigned denominator = BigUnsigned::Parse("1", 10);
    const std::int64_t scale = binaryExponent - static_cast<std::int64_t>(fractionalDigits * 4);
    if (scale >= 0) {
        integer = integer.ShiftedLeft(scale);
    }
    else {
        denominator = denominator.ShiftedLeft(-scale);
    }
    return RoundRatio(std::move(integer), std::move(denominator), negative, format);
}
} // namespace

std::optional<FloatEncoding> ParseFloatEncoding(const std::string_view literal, const FloatFormat &format) {
    if (literal.find_first_of("pP") != std::string_view::npos) {
        return ParseHexFloat(literal, format);
    }
    const auto parts = SplitFloatLiteral(literal);
    if (!parts) {
        return std::nullopt;
    }
    if (parts->kind == FloatLiteralKind::Infinity) {
        return FloatEncoding::Infinity(format, parts->negative);
    }
    if (parts->kind == FloatLiteralKind::QuietNaN) {
        return FloatEncoding::QuietNaN(format, parts->negative);
    }
    if (parts->kind == FloatLiteralKind::SignalingNaN) {
        return FloatEncoding::SignalingNaN(format, parts->negative);
    }

    const long double approximateExponent =
        (static_cast<long double>(parts->digits.size()) + parts->decimalExponent) * 3.32192809488736234787L;
    const std::int64_t maximumExponent = static_cast<std::int64_t>(format.MaxExponentField() - 1) - format.exponentBias;
    const std::int64_t minimumExponent =
        1 - static_cast<std::int64_t>(format.exponentBias) - static_cast<std::int64_t>(format.precisionBits);
    if (approximateExponent > maximumExponent + 64) {
        return FloatEncoding::Infinity(format, parts->negative);
    }
    if (approximateExponent < minimumExponent - 64) {
        return FloatEncoding::Zero(format, parts->negative);
    }

    BigUnsigned numerator = BigUnsigned::Parse(parts->digits, 10);
    BigUnsigned denominator = BigUnsigned::Parse("1", 10);
    if (parts->decimalExponent >= 0) {
        for (std::int64_t index = 0; index < parts->decimalExponent; ++index) {
            numerator.MultiplySmall(10);
        }
    }
    else {
        for (std::int64_t index = 0; index > parts->decimalExponent; --index) {
            denominator.MultiplySmall(10);
        }
    }
    return RoundRatio(std::move(numerator), std::move(denominator), parts->negative, format);
}

std::string FormatFloatEncoding(const FloatEncoding &encoding) {
    const FloatClass classification = encoding.Classify();
    const std::string sign = encoding.IsNegative() ? "-" : "";
    if (classification == FloatClass::Zero) {
        return sign + "0x0p0";
    }
    if (classification == FloatClass::Infinity) {
        return sign + "infinity";
    }
    if (classification == FloatClass::QuietNaN || classification == FloatClass::Invalid) {
        return sign + "nan";
    }
    if (classification == FloatClass::SignalingNaN) {
        return sign + "snan";
    }

    const UnpackedFloat value = UnpackFloat(encoding);
    const WideInteger significand = value.significand.ShiftedRight(FloatExtraPrecisionBits, false);
    const std::uint32_t fractionBits = encoding.Format().precisionBits - 1;
    const std::uint32_t digits = (fractionBits + 3) / 4;
    std::string result = sign + "0x1.";
    constexpr char hexadecimal[] = "0123456789abcdef";
    for (std::uint32_t digit = 0; digit < digits; ++digit) {
        unsigned nibble = 0;
        for (std::uint32_t bit = 0; bit < 4; ++bit) {
            const std::int64_t source = static_cast<std::int64_t>(fractionBits) - 1 - digit * 4 - bit;
            if (source >= 0 && significand.BitSet(static_cast<std::uint32_t>(source))) {
                nibble |= 1U << (3 - bit);
            }
        }
        result.push_back(hexadecimal[nibble]);
    }
    while (result.back() == '0') {
        result.pop_back();
    }
    if (result.back() == '.') {
        result.push_back('0');
    }
    result += "p" + std::to_string(value.exponent);
    return result;
}
} // namespace Rux
