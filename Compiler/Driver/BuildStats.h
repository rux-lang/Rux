#pragma once

#include "BuildInfo/BuildProfile.h"
#include "Target/TargetTriple.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace Rux {
struct Token;
}

namespace Rux::Driver {
struct BuildStats {
    std::chrono::milliseconds lexing{0};
    std::chrono::milliseconds parsing{0};
    std::chrono::milliseconds semantic{0};
    std::chrono::milliseconds hir{0};
    std::chrono::milliseconds lir{0};
    std::chrono::milliseconds codegen{0};
    std::chrono::milliseconds linking{0};
    std::chrono::milliseconds total{0};
    double totalSeconds = 0.0;
    std::size_t localFiles = 0;
    std::size_t dependencyFiles = 0;
    std::size_t localLines = 0;
    std::size_t dependencyLines = 0;
    std::size_t localTokens = 0;
    std::size_t dependencyTokens = 0;
    std::uintmax_t localSourceSize = 0;
    std::uintmax_t dependencySourceSize = 0;
    std::uintmax_t executableSize = 0;
    std::uintmax_t peakMemoryBytes = 0;
    std::size_t prunedFunctionDefinitions = 0;
    std::size_t prunedConstants = 0;
    std::size_t prunedVtables = 0;
    std::size_t prunedExternDeclarations = 0;
    std::size_t estimatedLirNodesEliminated = 0;
};

struct BuildCellReport {
    BuildProfile profile;
    Target::TargetTriple target;
    std::filesystem::path outputDirectory;
    bool succeeded = false;
    std::filesystem::path artifactPath;
    BuildStats stats;
    std::chrono::milliseconds elapsed{0};
};

inline std::chrono::milliseconds
ElapsedMs(const std::chrono::steady_clock::time_point start,
          const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
}

inline double ElapsedSeconds(const std::chrono::steady_clock::time_point start,
                             const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
    return std::chrono::duration<double>(end - start).count();
}

// ---- Counting ---------------------------------------------------------------

[[nodiscard]] std::size_t CountLines(std::string_view source);
[[nodiscard]] std::size_t CountTokens(std::span<const Token> tokens);

} // namespace Rux::Driver
