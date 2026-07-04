#include "System/Os.h"

#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <utility>
#include <vector>

#include "System/WinApi.h"
#include "Target/Platform.h"

#if RUX_OS_WINDOWS
    #include <psapi.h>
#else
    #include <sys/ioctl.h>
    #include <sys/resource.h>
    #include <unistd.h>
    #if defined(__has_include)
        #if __has_include(<termios.h>)
            #include <termios.h>
        #elif __has_include(<sys/termios.h>)
            #include <sys/termios.h>
        #endif
        #if __has_include(<stropts.h>)
            #include <stropts.h>
        #endif
    #endif
#endif

namespace Rux::System {

using namespace Target;

// ---- Environment --------------------------------------------------------------

std::optional<std::string> GetEnv(const char *name) {
#if RUX_OS_WINDOWS
    // Win32 instead of std::getenv: the CRT copy of the environment is not
    // refreshed by SetEnvironmentVariable, and getenv trips MSVC's C4996.
    const DWORD len = GetEnvironmentVariableA(name, nullptr, 0);
    if (len == 0) {
        return std::nullopt;
    }
    std::string value(len, '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), len);
    value.resize(written);
    return value;
#else
    const char *value = std::getenv(name);
    if (!value) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

bool HasEnv(const char *name) {
#if RUX_OS_WINDOWS
    return GetEnvironmentVariableA(name, nullptr, 0) != 0;
#else
    return std::getenv(name) != nullptr;
#endif
}

std::optional<std::filesystem::path> GetEnvPath(const char *name) {
#if RUX_OS_WINDOWS
    // Read the wide environment so non-ASCII paths survive the round trip.
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    const DWORD len = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (len == 0) {
        return std::nullopt;
    }
    std::wstring value(len, L'\0');
    const DWORD written = GetEnvironmentVariableW(wname.c_str(), value.data(), len);
    value.resize(written);
    return std::filesystem::path(std::move(value));
#else
    auto value = GetEnv(name);
    if (!value) {
        return std::nullopt;
    }
    return std::filesystem::path(std::move(*value));
#endif
}

// ---- Console ------------------------------------------------------------------

bool StdoutIsInteractive() {
#if RUX_OS_WINDOWS
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
    return isatty(fileno(stdout)) != 0;
#endif
}

std::size_t TerminalWidth() {
#if RUX_OS_WINDOWS
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) == TRUE) {
        return static_cast<std::size_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
    }
#else
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1 && w.ws_col > 0) {
        return static_cast<std::size_t>(w.ws_col);
    }
#endif
    return 0;
}

// ---- Directories ----------------------------------------------------------------

std::filesystem::path TempDirectory() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    return ec ? std::filesystem::path{} : dir;
}

std::filesystem::path WindowsSystemDirectory() {
#if RUX_OS_WINDOWS
    wchar_t sysDir[MAX_PATH];
    const UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(std::wstring(sysDir, len));
    }
#endif
    return {};
}

// ---- Process --------------------------------------------------------------------

std::uintmax_t PeakMemoryBytes() noexcept {
#if RUX_OS_WINDOWS
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return static_cast<std::uintmax_t>(counters.PeakWorkingSetSize);
    }
#elif RUX_IS_UNIX
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        // macOS reports bytes directly; other Unices report in KB.
        constexpr std::uintmax_t unitMultiplier = (HostOS == OS::MacOS) ? 1ULL : 1024ULL;
        return static_cast<std::uintmax_t>(usage.ru_maxrss) * unitMultiplier;
    }
#endif
    return 0;
}

// ---- Output file naming -----------------------------------------------------------

std::string ExecutableFileName(std::string name, OS os) {
    if (os == OS::Windows) {
        name += ".exe";
    }
    return name;
}

std::string SharedLibraryFileName(std::string name, OS os) {
    if (os == OS::Windows) {
        name += ".dll";
    }
    return name;
}

} // namespace Rux::System
