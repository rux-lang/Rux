#include "Cli/TerminalStyle.h"

#include "System/Os.h"

#include <format>

namespace Rux::CliSupport {
std::string Status(const std::string_view verb) {
    // A verb longer than the column keeps its own width rather than being cut,
    // so the line stays readable even where the alignment cannot hold.
    return std::format("{:>{}}", verb, statusWidth);
}

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
