#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Rux {
enum class FloatLiteralKind : std::uint8_t {
    Finite,
    Infinity,
    QuietNaN,
    SignalingNaN,
};

/// A floating literal before binary rounding. A finite value is exactly `digits * 10^decimalExponent`; retaining that
/// decimal form prevents the host's `double` from deciding the result of wider target formats.
struct FloatLiteralParts {
    FloatLiteralKind kind = FloatLiteralKind::Finite;
    bool negative = false;
    std::string digits = "0";
    std::int64_t decimalExponent = 0;
    std::string_view suffix;
};

/// Split a decimal literal or an internal special spelling (`inf`, `nan`, `snan`) into a host-independent form.
/// Separators must occur between digits and any suffix must be one of the float suffixes.
[[nodiscard]] std::optional<FloatLiteralParts> SplitFloatLiteral(std::string_view text);
} // namespace Rux
