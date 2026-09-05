#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>

namespace Rux::System {
/// Open a binary artifact for replacement. Retry Windows sharing or mapping failures on an existing file within the
/// supplied budget, then return the failed stream to the caller. Other failures return immediately.
[[nodiscard]] std::ofstream OpenBinaryOutput(const std::filesystem::path &path,
                                             std::chrono::milliseconds retryBudget = std::chrono::seconds(5));
} // namespace Rux::System
