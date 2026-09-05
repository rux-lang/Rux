#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace Rux::CliSupport {
struct TestTask {
    // Files or directory trees written while building/running this package.
    std::vector<std::filesystem::path> artifacts;
};

/// Run independent tasks concurrently and report on the calling thread in discovery order. Overlapping artifact
/// paths are serialized in discovery order, including aliases through existing symlinks and parent directories.
void RunTestTasks(std::span<const TestTask> tasks, std::size_t jobs, const std::function<void(std::size_t)> &execute,
                  const std::function<void(std::size_t)> &report);
} // namespace Rux::CliSupport
