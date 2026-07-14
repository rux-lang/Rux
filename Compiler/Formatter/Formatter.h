#pragma once

#include <string>
#include <string_view>

namespace Rux::Formatting {
struct FormatResult {
    std::string text;
    bool changed = false;
};

// Conservative source normalization. Structural formatting will build on the
// syntax tree once the lexer preserves comments and other trivia.
[[nodiscard]] FormatResult Format(std::string_view source);
} // namespace Rux::Formatting
