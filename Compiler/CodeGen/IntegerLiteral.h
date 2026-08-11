#pragma once

// Reading a Rux integer literal back out of the text LIR carries it in.
//
// A `const` instruction keeps the literal the source wrote rather than a
// decoded value, so every back end has to read it again before it can encode
// one. What has to be read is the language's rule and not the machine's — the
// type suffixes, the four bases, the digit separators, and the one negative
// magnitude that has no positive counterpart — so it lives here beside
// FloatLiteral.h rather than once inside each code generator.

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Rux {
// The type suffix `text` ends with, or an empty view when it carries none.
[[nodiscard]] inline std::string_view NumericLiteralSuffix(const std::string_view text) {
    static constexpr std::string_view suffixes[] = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "i", "u",
    };
    for (const auto suffix : suffixes) {
        if (text.size() > suffix.size() && text.substr(text.size() - suffix.size()) == suffix) {
            return suffix;
        }
    }
    return {};
}

// The bits `text` denotes, as the pattern a 64-bit register would hold: a
// negative literal comes back as its two's complement. Nothing is returned when
// the text is not an integer literal at all, or names a magnitude no 64-bit
// register holds, so a caller decides for itself what an unreadable constant
// means rather than being handed a zero that looks deliberate.
[[nodiscard]] inline std::optional<std::uint64_t> ParseIntegerLiteralBits(std::string_view text) {
    if (const std::string_view suffix = NumericLiteralSuffix(text); !suffix.empty()) {
        text.remove_suffix(suffix.size());
    }

    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }

    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char c : text) {
        if (c != '_') {
            cleaned.push_back(c);
        }
    }

    int base = 10;
    std::string_view digits(cleaned);
    if (digits.size() > 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    const auto *first = digits.data();
    const auto *last = first + digits.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, base);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    if (!negative) {
        return value;
    }

    constexpr std::uint64_t maxNegativeMagnitude =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    if (value > maxNegativeMagnitude) {
        return std::nullopt;
    }
    return std::uint64_t{0} - value;
}
} // namespace Rux
