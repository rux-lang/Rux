#pragma once

// Terminal styling helpers for the CLI.

#include "Cli/Cli.h"
#include "Reporting/Reporting.h"

#include <cstdio>
#include <string_view>

namespace Rux::CliSupport {
/// Color-policy detection stays in the CLI. Presentation tokens and semantic style live in the host-independent
/// reporting component.
using AnsiStyle = Reporting::Style;

/// Decide whether ANSI styling should be emitted, honoring --color, the NO_COLOR convention, TERM=dumb, and whether the
/// selected stream is an interactive terminal. On Windows this also enables virtual-terminal processing.
enum class OutputStream {
    Stdout,
    Stderr
};
[[nodiscard]] bool ColorEnabled(ColorMode mode, OutputStream stream = OutputStream::Stdout);
[[nodiscard]] bool ColorEnabled(ColorMode mode, std::FILE *output);

/// Pure policy seam used by terminal detection and its tests. Explicit color wins; automatic color requires an
/// interactive stream and honors both NO_COLOR (even when empty) and TERM=dumb.
[[nodiscard]] bool ResolveColor(ColorMode mode, bool interactive, bool noColor, bool dumbTerminal);

// Progress lines print their verb at column 0 with no padding, so a run of them
// shares one left edge. Nothing right-aligns a verb; a helper that did was
// removed on purpose.

/// Indent that lines a continuation up under the message of an `error: ` line, so the detail reads as belonging to the
/// error rather than to the next thing.
inline constexpr std::string_view errorContinuation = Reporting::errorContinuation;
} // namespace Rux::CliSupport
