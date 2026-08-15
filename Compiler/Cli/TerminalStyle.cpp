#include "Cli/TerminalStyle.h"

#include "System/Os.h"

namespace Rux::CliSupport {
bool ResolveColor(const ColorMode mode, const bool interactive, const bool noColor, const bool dumbTerminal) {
    if (mode == ColorMode::On) {
        return true;
    }
    if (mode == ColorMode::Off) {
        return false;
    }
    return interactive && !noColor && !dumbTerminal;
}

bool ColorEnabled(const ColorMode mode, std::FILE *output) {
    // The query also enables Windows virtual-terminal processing when the
    // stream is a console. Forced color still emits escapes when redirected.
    const bool interactive = System::OutputIsInteractive(output);
    const auto term = System::GetEnv("TERM");
    return ResolveColor(mode, interactive, System::HasEnv("NO_COLOR"), term && *term == "dumb");
}

bool ColorEnabled(const ColorMode mode, const OutputStream stream) {
    return ColorEnabled(mode, stream == OutputStream::Stderr ? stderr : stdout);
}
} // namespace Rux::CliSupport
