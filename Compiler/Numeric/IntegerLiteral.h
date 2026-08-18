#pragma once

#include "Numeric/WideInteger.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Rux {
/// One numeric literal suffix and the type it names.
///
/// `bits` is zero for a pointer-sized suffix (`i` and `u`), whose width the target supplies.
struct NumericLiteralSuffixInfo {
    std::string_view text;
    std::uint32_t bits = 0;
    bool isSigned = false;
    bool isFloat = false;
};

/// Every suffix a numeric literal may carry, in the order the diagnostic lists them.
///
/// One table, read by the lexer that validates a suffix, the analysis that types it, the lowering that strips it and
/// the code generation that decodes what is left. It lives here because this is the only component all four sit
/// above.
[[nodiscard]] std::span<const NumericLiteralSuffixInfo> NumericLiteralSuffixes() noexcept;

/// @return the entry for `suffix`, or nullptr when no suffix is spelled that way
[[nodiscard]] const NumericLiteralSuffixInfo *FindNumericLiteralSuffix(std::string_view suffix) noexcept;

/// The suffix `text` ends with, or an empty view when it carries none.
///
/// Only a suffix the table knows is recognized, and the longest match wins, so `1u128` ends in `u128` rather than in
/// `u1` followed by stray digits.
[[nodiscard]] std::string_view NumericLiteralSuffixOf(std::string_view text) noexcept;

/// The suffixes quoted the way a diagnostic lists them: `'i', 'i8', ..., 'f64'`.
[[nodiscard]] std::string NumericLiteralSuffixList();

/// A literal split into the parts that decode it.
struct IntegerLiteralParts {
    bool negative = false;
    unsigned base = 10;
    std::string_view digits; ///< no sign, no base prefix, no suffix; underscores may remain
    std::string_view suffix; ///< empty when the literal carries none
};

/// Split `text` into its sign, base, digits and suffix.
///
/// @return nullopt when `text` has no digits at all
[[nodiscard]] std::optional<IntegerLiteralParts> SplitIntegerLiteral(std::string_view text) noexcept;

/// Decode `text` into a magnitude held at `width` bits.
///
/// The magnitude is unsigned: a leading `-` is reported through `IntegerLiteralParts::negative` rather than folded in,
/// because the most negative value of a signed width has a magnitude that width cannot hold as a positive number.
/// Decode at `WideInteger::MaxBits` and range-check afterwards to tell an oversized literal from a malformed one.
///
/// @return nullopt when `text` is not an integer literal, or its magnitude needs more than `width` bits
[[nodiscard]] std::optional<WideInteger> DecodeIntegerLiteral(std::string_view text, std::uint32_t width);

/// Whether a literal of this magnitude and sign is one a `width`-bit integer of this signedness holds.
///
/// An unsigned width refuses a negative literal outright, `-0` aside. A signed one accepts a magnitude up to
/// 2^(width-1) when negative -- which is how the most negative value is written -- and up to 2^(width-1) - 1 when not.
[[nodiscard]] bool IntegerLiteralFits(const WideInteger &magnitude, bool negative, std::uint32_t width,
                                      bool isSigned) noexcept;
} // namespace Rux
