#pragma once

#include "Numeric/FloatEncoding.h"

#include <optional>
#include <string>
#include <string_view>

namespace Rux {
/// Round a decimal source literal or an exact hexadecimal internal literal without using the host floating-point
/// implementation.
[[nodiscard]] std::optional<FloatEncoding> ParseFloatEncoding(std::string_view literal, const FloatFormat &format);

/// Render a compact exact hexadecimal literal suitable for HIR and target-independent reparsing.
[[nodiscard]] std::string FormatFloatEncoding(const FloatEncoding &encoding);
} // namespace Rux
