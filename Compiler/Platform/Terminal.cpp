#include "Platform/Terminal.h"

#include "Platform/Platform.h"
#include "Platform/WinApi.h"

#include <cstdio>
#include <cstdlib>

#if !RUX_OS_WINDOWS
    #include <unistd.h>
#endif

namespace Rux::Misc {

bool ColorEnabled(ColorMode mode) {
    if (mode == ColorMode::On) {
        return true;
    }
    if (mode == ColorMode::Off) {
        return false;
    }
#if RUX_OS_WINDOWS
    if (GetEnvironmentVariableA("NO_COLOR", nullptr, 0) != 0) {
        return false;
    }
    HANDLE const handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (GetFileType(handle) != FILE_TYPE_CHAR) {
        return false; // redirected to a file or pipe
    }
    DWORD consoleMode = 0;
    if (!GetConsoleMode(handle, &consoleMode)) {
        return false;
    }
    SetConsoleMode(handle, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    return true;
#else
    if (std::getenv("NO_COLOR") != nullptr) {
        return false;
    }
    return isatty(fileno(stdout)) != 0;
#endif
}

} // namespace Rux::Misc
