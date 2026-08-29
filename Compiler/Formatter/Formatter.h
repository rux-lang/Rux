#pragma once

#include <string>
#include <string_view>

namespace Rux::Formatting {
/// Formatted source, and whether it differs from the input. `rux fmt --check` reports on `changed` rather than diffing
/// the text itself.
struct FormatResult {
    std::string text;
    bool changed = false;
};

/// Normalize line endings and documentation-comment spelling using lossless lexer ranges. Ordinary comments, literals,
/// Markdown structure, and unterminated blocks retain their authored content.
[[nodiscard]] FormatResult Format(std::string_view source);
} // namespace Rux::Formatting
