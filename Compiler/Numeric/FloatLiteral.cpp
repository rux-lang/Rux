#include "Numeric/FloatLiteral.h"

#include "Numeric/IntegerLiteral.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace Rux {
namespace {
[[nodiscard]] std::int64_t SaturatingAdd(const std::int64_t left, const std::int64_t right) noexcept {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

[[nodiscard]] bool AppendDigits(std::string_view text, std::string &output) {
    bool previousDigit = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '_') {
            if (!previousDigit || index + 1 == text.size() ||
                !std::isdigit(static_cast<unsigned char>(text[index + 1]))) {
                return false;
            }
            previousDigit = false;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(character))) {
            return false;
        }
        output.push_back(character);
        previousDigit = true;
    }
    return !text.empty() && previousDigit;
}

[[nodiscard]] std::optional<std::int64_t> ParseExponent(std::string_view text) {
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    std::string digits;
    if (!AppendDigits(text, digits)) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char digit : digits) {
        const int number = digit - '0';
        if (value > (std::numeric_limits<std::int64_t>::max() - number) / 10) {
            return negative ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
        }
        value = value * 10 + number;
    }
    return negative ? -value : value;
}
} // namespace

std::optional<FloatLiteralParts> SplitFloatLiteral(std::string_view text) {
    FloatLiteralParts parts;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        parts.negative = text.front() == '-';
        text.remove_prefix(1);
    }

    parts.suffix = NumericLiteralSuffixOf(text);
    if (!parts.suffix.empty()) {
        const NumericLiteralSuffixInfo *suffix = FindNumericLiteralSuffix(parts.suffix);
        if (!suffix || !suffix->isFloat) {
            return std::nullopt;
        }
        text.remove_suffix(parts.suffix.size());
    }

    if (text == "inf" || text == "infinity") {
        parts.kind = FloatLiteralKind::Infinity;
        return parts;
    }
    if (text == "nan" || text == "snan") {
        parts.kind = text == "nan" ? FloatLiteralKind::QuietNaN : FloatLiteralKind::SignalingNaN;
        return parts;
    }

    const std::size_t exponentMarker = text.find_first_of("eE");
    if (exponentMarker != std::string_view::npos &&
        text.find_first_of("eE", exponentMarker + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view mantissa = text.substr(0, exponentMarker);
    const std::string_view exponentText =
        exponentMarker == std::string_view::npos ? std::string_view{} : text.substr(exponentMarker + 1);
    const std::size_t point = mantissa.find('.');
    if (point != std::string_view::npos && mantissa.find('.', point + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view whole = mantissa.substr(0, point);
    const std::string_view fraction = point == std::string_view::npos ? std::string_view{} : mantissa.substr(point + 1);
    std::string wholeDigits;
    std::string fractionDigits;
    if ((!whole.empty() && !AppendDigits(whole, wholeDigits)) ||
        (!fraction.empty() && !AppendDigits(fraction, fractionDigits)) ||
        (wholeDigits.empty() && fractionDigits.empty())) {
        return std::nullopt;
    }
    if (point != std::string_view::npos && fraction.empty() && wholeDigits.empty()) {
        return std::nullopt;
    }

    std::int64_t exponent = 0;
    if (exponentMarker != std::string_view::npos) {
        const auto parsed = ParseExponent(exponentText);
        if (!parsed) {
            return std::nullopt;
        }
        exponent = *parsed;
    }
    exponent = SaturatingAdd(exponent, -static_cast<std::int64_t>(fractionDigits.size()));
    parts.digits = wholeDigits + fractionDigits;

    const std::size_t firstNonZero = parts.digits.find_first_not_of('0');
    if (firstNonZero == std::string::npos) {
        parts.digits = "0";
        parts.decimalExponent = 0;
        return parts;
    }
    parts.digits.erase(0, firstNonZero);
    while (parts.digits.size() > 1 && parts.digits.back() == '0') {
        parts.digits.pop_back();
        exponent = SaturatingAdd(exponent, 1);
    }
    parts.decimalExponent = exponent;
    return parts;
}
} // namespace Rux
