#include "Numeric/IntegerLiteral.h"

#include <algorithm>
#include <array>

namespace Rux {
namespace {
/// The suffixes, narrowest first within each signedness so the diagnostic reads in a sensible order. A pointer-sized
/// suffix carries a width of zero, which the target fills in.
constexpr std::array<NumericLiteralSuffixInfo, 18> Suffixes{{
    {"i", 0, true, false},
    {"i8", 8, true, false},
    {"i16", 16, true, false},
    {"i32", 32, true, false},
    {"i64", 64, true, false},
    {"i128", 128, true, false},
    {"i256", 256, true, false},
    {"i512", 512, true, false},
    {"u", 0, false, false},
    {"u8", 8, false, false},
    {"u16", 16, false, false},
    {"u32", 32, false, false},
    {"u64", 64, false, false},
    {"u128", 128, false, false},
    {"u256", 256, false, false},
    {"u512", 512, false, false},
    {"f32", 32, true, true},
    {"f64", 64, true, true},
}};
} // namespace

std::span<const NumericLiteralSuffixInfo> NumericLiteralSuffixes() noexcept {
    return Suffixes;
}

const NumericLiteralSuffixInfo *FindNumericLiteralSuffix(const std::string_view suffix) noexcept {
    if (suffix.empty()) {
        return nullptr;
    }
    const auto found = std::ranges::find(Suffixes, suffix, &NumericLiteralSuffixInfo::text);
    return found == Suffixes.end() ? nullptr : &*found;
}

std::string_view NumericLiteralSuffixOf(const std::string_view text) noexcept {
    // Longest match wins, so `1u128` is not read as `1u1` followed by `28`.
    std::string_view best;
    for (const NumericLiteralSuffixInfo &suffix : Suffixes) {
        if (text.size() > suffix.text.size() && text.ends_with(suffix.text) && suffix.text.size() > best.size()) {
            best = suffix.text;
        }
    }
    return best;
}

std::string NumericLiteralSuffixList() {
    std::string list;
    for (std::size_t index = 0; index < Suffixes.size(); ++index) {
        if (index != 0) {
            list += index + 1 == Suffixes.size() ? ", or " : ", ";
        }
        list += '\'';
        list += Suffixes[index].text;
        list += '\'';
    }
    return list;
}

std::optional<IntegerLiteralParts> SplitIntegerLiteral(std::string_view text) noexcept {
    IntegerLiteralParts parts;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        parts.negative = text.front() == '-';
        text.remove_prefix(1);
    }

    parts.suffix = NumericLiteralSuffixOf(text);
    text.remove_suffix(parts.suffix.size());

    if (text.size() > 2 && text[0] == '0') {
        switch (text[1]) {
        case 'x':
        case 'X':
            parts.base = 16;
            text.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            parts.base = 2;
            text.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            parts.base = 8;
            text.remove_prefix(2);
            break;
        default:
            break;
        }
    }

    parts.digits = text;
    if (parts.digits.empty()) {
        return std::nullopt;
    }
    return parts;
}

std::optional<WideInteger> DecodeIntegerLiteral(const std::string_view text, const std::uint32_t width) {
    const auto parts = SplitIntegerLiteral(text);
    if (!parts) {
        return std::nullopt;
    }
    return WideInteger::Parse(parts->digits, parts->base, width);
}

bool IntegerLiteralFits(const WideInteger &magnitude, const bool negative, const std::uint32_t width,
                        const bool isSigned) noexcept {
    if (!isSigned) {
        // Zero is the one magnitude a negative literal may have: `-0` is zero, and nothing else fits.
        if (negative && !magnitude.IsZero()) {
            return false;
        }
        return magnitude <= WideInteger::MaxValue(width, false);
    }
    const WideInteger limit = negative ? WideInteger::MinMagnitude(width, true) : WideInteger::MaxValue(width, true);
    return magnitude <= limit;
}
} // namespace Rux
