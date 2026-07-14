#pragma once

// Thin wrappers around host operating-system services: environment variables,
// console queries, well-known directories, process memory statistics, and
// per-OS output file naming.
//
// All direct use of these OS facilities (std::getenv, <windows.h> console and
// environment calls, isatty/ioctl, getrusage, ...) must stay inside
// Compiler/System/ so the rest of the compiler is platform-agnostic; CI
// greps for strays (see Tools/PlatformIsolation/Check.sh).

#include "Target/Target.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Rux::System {
// ---- Environment --------------------------------------------------------------

// Value of the environment variable `name`, or nullopt when it is not set.
[[nodiscard]] std::optional<std::string> GetEnv(const char *name);

// True when the environment variable `name` is set (used for flag-style
// variables such as NO_COLOR, where any value counts).
[[nodiscard]] bool HasEnv(const char *name);

// Environment variable interpreted as a filesystem path. On Windows this reads
// the wide (UTF-16) environment so non-ASCII paths survive the round trip.
[[nodiscard]] std::optional<std::filesystem::path> GetEnvPath(const char *name);

// ---- Console ------------------------------------------------------------------

// True when stdout is an interactive terminal (not redirected to a file or
// pipe). On Windows a positive answer also enables virtual-terminal processing
// on the console, so ANSI escape codes are interpreted.
[[nodiscard]] bool StdoutIsInteractive();

// Current terminal width in columns, or 0 when it cannot be determined.
[[nodiscard]] std::size_t TerminalWidth();

// ---- Directories ----------------------------------------------------------------

// The OS directory for temporary files, or an empty path when unavailable.
[[nodiscard]] std::filesystem::path TempDirectory();

// The Windows system directory (e.g. C:\Windows\System32), the authoritative
// home of system DLLs. Empty on non-Windows hosts or on failure.
[[nodiscard]] std::filesystem::path WindowsSystemDirectory();

// ---- Process --------------------------------------------------------------------

// Peak resident memory of the current process, in bytes (0 if unavailable).
[[nodiscard]] std::uintmax_t PeakMemoryBytes() noexcept;

// ---- Output file naming -----------------------------------------------------------

// Executable file name for `os`: appends ".exe" on Windows, unchanged elsewhere.
[[nodiscard]] std::string ExecutableFileName(std::string name, Target::OS os = Target::HostOS);

// Shared-library file name for `os`: appends ".dll" on Windows; unchanged
// elsewhere (ELF/Mach-O outputs currently carry no prefix or extension).
[[nodiscard]] std::string SharedLibraryFileName(std::string name, Target::OS os = Target::HostOS);
} // namespace Rux::System
