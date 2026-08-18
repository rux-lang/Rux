#pragma once

// Reading a Rux integer literal back out of the text LIR carries it in.
//
// A `const` instruction keeps the literal the source wrote rather than a
// decoded value, so every back end has to read it again before it can encode
// one. The language's rule for what that text means — the type suffixes, the
// four bases, the digit separators, and the one negative magnitude that has no
// positive counterpart — belongs to Numeric, which the front end reads the same
// rule from. What is left here is the machine's part: narrowing the decoded
// value to the register that will hold it.

#include "Numeric/IntegerLiteral.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Rux {
/// The type suffix `text` ends with, or an empty view when it carries none.
[[nodiscard]] inline std::string_view NumericLiteralSuffix(const std::string_view text) {
    return NumericLiteralSuffixOf(text);
}

/// The bits `text` denotes, as the pattern a 64-bit register would hold: a negative literal comes back as its two's
/// complement. Nothing is returned when the text is not an integer literal at all, or names a magnitude no 64-bit
/// register holds, so a caller decides for itself what an unreadable constant means rather than being handed a zero
/// that looks deliberate.
[[nodiscard]] inline std::optional<std::uint64_t> ParseIntegerLiteralBits(const std::string_view text) {
    const auto parts = SplitIntegerLiteral(text);
    if (!parts) {
        return std::nullopt;
    }
    const auto magnitude = WideInteger::Parse(parts->digits, parts->base, WideInteger::MaxBits);
    if (!magnitude) {
        return std::nullopt;
    }
    if (!parts->negative) {
        return magnitude->ToUnsigned();
    }
    // A negative literal is written as a magnitude, and the one magnitude with no positive counterpart is the most
    // negative value itself, so the bound is one past the largest positive value rather than equal to it.
    if (*magnitude > WideInteger::MinMagnitude(64, true)) {
        return std::nullopt;
    }
    const auto bits = magnitude->Truncated(64).Negated().ToUnsigned();
    return bits;
}
} // namespace Rux
