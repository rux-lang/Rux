#include "Cli/TerminalStyle.h"

#include "System/Os.h"

namespace Rux::CliSupport {
bool ColorEnabled(ColorMode mode) {
    if (mode == ColorMode::On) {
        return true;
    }
    if (mode == ColorMode::Off) {
        return false;
    }
    if (System::HasEnv("NO_COLOR")) {
        return false;
    }
    return System::StdoutIsInteractive();
}
} // namespace Rux::CliSupport
