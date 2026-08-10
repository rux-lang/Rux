#pragma once

// Terminal styling helpers for the CLI.

#include "Cli/Cli.h"

#include <string_view>

namespace Rux::CliSupport {
// Holds whether ANSI styling is active and hands out escape codes that collapse
// to empty strings when color is disabled, so call sites can interpolate them
// unconditionally.
struct AnsiStyle {
    bool enabled = false;

    [[nodiscard]] std::string_view Green() const {
        return enabled ? "\033[32m" : "";
    }

    [[nodiscard]] std::string_view Red() const {
        return enabled ? "\033[31m" : "";
    }

    [[nodiscard]] std::string_view Yellow() const {
        return enabled ? "\033[33m" : "";
    }

    [[nodiscard]] std::string_view Bold() const {
        return enabled ? "\033[1m" : "";
    }

    [[nodiscard]] std::string_view Dim() const {
        return enabled ? "\033[2m" : "";
    }

    [[nodiscard]] std::string_view Reset() const {
        return enabled ? "\033[0m" : "";
    }
};

// Decide whether ANSI styling should be emitted, honoring --color, the NO_COLOR
// convention, and whether stdout is an interactive terminal. On Windows this
// also enables virtual-terminal processing so the escape codes are interpreted.
[[nodiscard]] bool ColorEnabled(ColorMode mode);

// Progress lines print their verb at column 0 with no padding, so a run of them
// shares one left edge. Nothing right-aligns a verb; a helper that did was
// removed on purpose.

// Indent that lines a continuation up under the message of an `error: ` line,
// so the detail reads as belonging to the error rather than to the next thing.
inline constexpr std::string_view errorContinuation = "       ";
} // namespace Rux::CliSupport
