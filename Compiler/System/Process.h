#pragma once

// Host subprocess execution and capture.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Rux::System {
/// Outcome of a captured subprocess run.
struct RunResult {
    int exitCode = 0;
    std::string output; // combined stdout + stderr
};

/// Run `exe` with `args`, inheriting this process's stdin/stdout/stderr (used by `rux run`). Returns the child's exit
/// code, or nullopt when the process could not be launched.
[[nodiscard]] std::optional<int> RunInherited(const std::filesystem::path &exe,
                                              std::span<const std::string_view> args = {},
                                              std::error_code *launchError = nullptr);

/// Run `exe`, stdin redirected from the null device, and its combined stdout+stderr captured (used by tests and `rux
/// test`). Returns nullopt when the process could not be launched.
[[nodiscard]] std::optional<RunResult> RunCaptured(const std::filesystem::path &exe,
                                                   std::span<const std::string_view> args = {},
                                                   std::error_code *launchError = nullptr);

} // namespace Rux::System
