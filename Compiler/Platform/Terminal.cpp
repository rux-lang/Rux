#include "Platform/Terminal.h"

#include "Platform/Os.h"

namespace Rux::Misc {

bool ColorEnabled(ColorMode mode) {
    if (mode == ColorMode::On) {
        return true;
    }
    if (mode == ColorMode::Off) {
        return false;
    }
    if (Platform::HasEnv("NO_COLOR")) {
        return false;
    }
    return Platform::StdoutIsInteractive();
}

} // namespace Rux::Misc
