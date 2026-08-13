#include "System/Os.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

#if RUX_OS_WINDOWS
    #include <psapi.h>
    #include <shellapi.h>
#else
    #include <sys/ioctl.h>
    #include <sys/resource.h>
    #if RUX_OS_FREEBSD
        #include <sys/sysctl.h>
        #include <sys/user.h>
    #endif
    #include <sys/wait.h>
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

// termios is optional on the Unices we build for; TCSAFLUSH is the marker that
// the header was actually found above. Without it a secret is still read, just
// without echo suppression.
#if !RUX_OS_WINDOWS && defined(TCSAFLUSH)
    #define RUX_HAS_TERMIOS 1
#else
    #define RUX_HAS_TERMIOS 0
#endif

namespace Rux::System {
using namespace Target;

// ---- Environment --------------------------------------------------------------

std::optional<std::string> GetEnv(const char *name) {
#if RUX_OS_WINDOWS
    // Win32 instead of std::getenv: the CRT copy of the environment is not
    // refreshed by SetEnvironmentVariable, and the Microsoft CRT deprecates getenv.
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

#if RUX_OS_WINDOWS
namespace {
/// Widen an ASCII name for the wide environment entry points.
std::wstring WidenAscii(const std::string_view text) {
    return {text.begin(), text.end()};
}
} // namespace
#endif

bool SetEnv(const char *name, const std::string_view value) {
#if RUX_OS_WINDOWS
    return SetEnvironmentVariableW(WidenAscii(name).c_str(), WidenAscii(value).c_str()) != 0;
#else
    return setenv(name, std::string(value).c_str(), 1) == 0;
#endif
}

bool SetEnvPath(const char *name, const std::filesystem::path &value) {
#if RUX_OS_WINDOWS
    return SetEnvironmentVariableW(WidenAscii(name).c_str(), value.wstring().c_str()) != 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

bool UnsetEnv(const char *name) {
#if RUX_OS_WINDOWS
    // A null value deletes the variable; "already absent" reports success.
    return SetEnvironmentVariableW(WidenAscii(name).c_str(), nullptr) != 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND;
#else
    return unsetenv(name) == 0;
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

bool StderrIsInteractive() {
#if RUX_OS_WINDOWS
    HANDLE const handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_CHAR) {
        return false;
    }
    DWORD consoleMode = 0;
    if (!GetConsoleMode(handle, &consoleMode)) {
        return false;
    }
    SetConsoleMode(handle, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    return true;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

bool StdinIsInteractive() {
#if RUX_OS_WINDOWS
    HANDLE const handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (GetFileType(handle) != FILE_TYPE_CHAR) {
        return false; // redirected from a file or pipe
    }
    DWORD consoleMode = 0;
    return GetConsoleMode(handle, &consoleMode) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

std::optional<std::string> ReadSecretLine() {
    // Echo is disabled around the read and restored on every exit path, so an
    // interrupted or failed read cannot leave the terminal silent.
    bool restore = false;
#if RUX_OS_WINDOWS
    HANDLE const handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD savedMode = 0;
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &savedMode)) {
        restore = SetConsoleMode(handle, savedMode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) != 0;
    }
#elif RUX_HAS_TERMIOS
    termios saved{};
    if (isatty(fileno(stdin)) != 0 && tcgetattr(fileno(stdin), &saved) == 0) {
        termios muted = saved;
        muted.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        restore = tcsetattr(fileno(stdin), TCSAFLUSH, &muted) == 0;
    }
#endif

    std::string line;
    const bool read = static_cast<bool>(std::getline(std::cin, line));

    if (restore) {
#if RUX_OS_WINDOWS
        SetConsoleMode(handle, savedMode);
#elif RUX_HAS_TERMIOS
        tcsetattr(fileno(stdin), TCSAFLUSH, &saved);
#endif
        // The Enter that ended the line was swallowed with the rest of the echo.
        std::fputc('\n', stderr);
    }

    if (!read) {
        return std::nullopt;
    }
    if (line.ends_with('\r')) {
        line.pop_back(); // CRLF input arriving through a pipe
    }
    return line;
}

std::size_t TerminalWidth() {
#if RUX_OS_WINDOWS
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) == TRUE) {
        const long width = static_cast<long>(csbi.srWindow.Right) - static_cast<long>(csbi.srWindow.Left) + 1;
        return static_cast<std::size_t>(width);
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

std::filesystem::path UserDataDir() {
    if constexpr (HostOS == OS::Windows) {
        return GetEnvPath("LOCALAPPDATA").value_or(std::filesystem::path{}) / "Rux";
    }
    else {
        return GetEnvPath("HOME").value_or(std::filesystem::path("/tmp")) / ".rux";
    }
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

// ---- Files ----------------------------------------------------------------------

bool SetPrivateFilePermissions(const std::filesystem::path &path) {
    if constexpr (HostOS == OS::Windows) {
        return true; // %LOCALAPPDATA% is already ACL'd to the owner
    }
    else {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        return !ec;
    }
}

bool OpenInDefaultApplication(const std::filesystem::path &path) {
#if RUX_OS_WINDOWS
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        if constexpr (HostOS == OS::MacOS) {
            execlp("open", "open", path.c_str(), static_cast<char *>(nullptr));
        }
        else {
            execlp("xdg-open", "xdg-open", path.c_str(), static_cast<char *>(nullptr));
        }
        _exit(127);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
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
        const auto peakBytes = static_cast<std::uintmax_t>(usage.ru_maxrss) * unitMultiplier;
        if (peakBytes != 0) {
            return peakBytes;
        }
    }
    #if RUX_OS_FREEBSD
    kinfo_proc proc{};
    std::size_t procLen = sizeof(proc);
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &proc, &procLen, nullptr, 0) == 0 && procLen >= sizeof(proc) && proc.ki_rssize > 0) {
        const auto pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize > 0) {
            return static_cast<std::uintmax_t>(proc.ki_rssize) * static_cast<std::uintmax_t>(pageSize);
        }
    }
    #endif
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
    else if (os == OS::MacOS) {
        name = "lib" + name + ".dylib";
    }
    else {
        name = "lib" + name + ".so";
    }
    return name;
}

std::string StaticLibraryFileName(std::string name, OS os) {
    if (os == OS::Windows) {
        name += ".lib";
    }
    else {
        name = "lib" + name + ".a";
    }
    return name;
}
} // namespace Rux::System
