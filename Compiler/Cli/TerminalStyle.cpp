#include "Cli/TerminalStyle.h"

#include "System/Os.h"

namespace Rux::CliSupport {
bool ColorEnabled(ColorMode mode, OutputStream stream) {
    if (mode == ColorMode::On) {
        // On Windows, querying the console also enables virtual-terminal
        // processing. Explicit color must do that setup just like auto mode,
        // while still emitting escapes when the selected stream is redirected.
        if (stream == OutputStream::Stderr) {
            (void)System::StderrIsInteractive();
        }
        else {
            (void)System::StdoutIsInteractive();
        }
        return true;
    }
    if (mode == ColorMode::Off) {
        return false;
    }
    const auto noColor = System::GetEnv("NO_COLOR");
    if (noColor && !noColor->empty()) {
        return false;
    }
    const auto term = System::GetEnv("TERM");
    if (term && *term == "dumb") {
        return false;
    }
    return stream == OutputStream::Stderr ? System::StderrIsInteractive() : System::StdoutIsInteractive();
}
} // namespace Rux::CliSupport
