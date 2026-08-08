#pragma once

// Thin wrappers around host operating-system services: environment variables,
// console queries, well-known directories, process memory statistics, and
// per-OS output file naming.
//
// All direct use of these OS facilities (std::getenv, <windows.h> console and
// environment calls, isatty/ioctl, getrusage, ...) must stay inside
// Compiler/System/ so the rest of the compiler is platform-agnostic; CI
// greps for strays (see Tests/Policy/PlatformIsolation/Check.sh).

#include "Target/Target.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

// Set the environment variable `name` for this process and the processes it
// starts. `value` must be ASCII: it round trips exactly as far as GetEnv does,
// which reads the narrow environment. Returns false when the update failed.
[[nodiscard]] bool SetEnv(const char *name, std::string_view value);

// Set the environment variable `name` to a filesystem path. The wide
// environment is written on Windows, so this is the exact counterpart of
// GetEnvPath and preserves non-ASCII paths.
[[nodiscard]] bool SetEnvPath(const char *name, const std::filesystem::path &value);

// Remove the environment variable `name` from this process. Returns false when
// the update failed; removing a variable that is not set succeeds.
[[nodiscard]] bool UnsetEnv(const char *name);

// ---- Console ------------------------------------------------------------------

// True when stdout is an interactive terminal (not redirected to a file or
// pipe). On Windows a positive answer also enables virtual-terminal processing
// on the console, so ANSI escape codes are interpreted.
[[nodiscard]] bool StdoutIsInteractive();

// True when stdin is an interactive terminal rather than a file or a pipe.
// Callers use this to decide between prompting and reading piped input.
[[nodiscard]] bool StdinIsInteractive();

// Read one line from stdin with terminal echo disabled, so a typed secret never
// appears on screen. The trailing newline is not returned. When stdin is not a
// terminal this degrades to an ordinary line read, which is what makes
// `echo $TOKEN | rux login` work. Returns nullopt at end of input.
[[nodiscard]] std::optional<std::string> ReadSecretLine();

// Current terminal width in columns, or 0 when it cannot be determined.
[[nodiscard]] std::size_t TerminalWidth();

// ---- Directories ----------------------------------------------------------------

// The OS directory for temporary files, or an empty path when unavailable.
[[nodiscard]] std::filesystem::path TempDirectory();

// Per-user directory for Rux state that outlives a single project: the package
// cache and the registry credentials file. `%LOCALAPPDATA%\Rux` on Windows,
// `$HOME/.rux` elsewhere. Falls back to an empty path (Windows) or `/tmp`
// (elsewhere) when the environment does not name a home.
[[nodiscard]] std::filesystem::path UserDataDir();

// The Windows system directory (e.g. C:\Windows\System32), the authoritative
// home of system DLLs. Empty on non-Windows hosts or on failure.
[[nodiscard]] std::filesystem::path WindowsSystemDirectory();

// ---- Files ----------------------------------------------------------------------

// Restrict `path` to its owner: mode 0600 on POSIX. No-op on Windows, where the
// per-user profile directories these files live in are already ACL'd to the
// owner. Returns false when the permissions could not be applied, which callers
// storing secrets must treat as a failure rather than ignore.
[[nodiscard]] bool SetPrivateFilePermissions(const std::filesystem::path &path);

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
