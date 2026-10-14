#pragma once

#include "BuildInfo/BuildProfile.h"
#include "Optimization/Statistics.h"
#include "Target/TargetTriple.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace Rux {
struct Token;
}

namespace Rux::Driver {
struct BuildStats {
    std::chrono::milliseconds lexing{0};
    std::chrono::milliseconds parsing{0};
    /// Resolving, reading and folding the dependencies, apart from their lexing and parsing, which count above.
    std::chrono::milliseconds dependencies{0};
    /// Hashing sources and reading packs to choose the modules to reuse, and writing the packs a build keeps.
    std::chrono::milliseconds reuse{0};
    std::chrono::milliseconds semantic{0};
    std::chrono::milliseconds hir{0};
    /// Lowering HIR to LIR and preparing its pruning; the optimization passes count below.
    std::chrono::milliseconds lir{0};
    std::chrono::milliseconds lirOptimization{0};
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
    /// Source files whose compiled code an earlier build recorded and this one reused (Driver/ModuleCache.h).
    std::size_t localReusedFiles = 0;
    std::size_t dependencyReusedFiles = 0;
    std::uintmax_t executableSize = 0;
    std::uintmax_t peakMemoryBytes = 0;
    std::size_t prunedFunctionDefinitions = 0;
    std::size_t prunedConstants = 0;
    std::size_t prunedVtables = 0;
    std::size_t prunedExternDeclarations = 0;
    std::size_t estimatedLirNodesEliminated = 0;
    std::vector<Optimization::PassStatistics> optimizationPasses;
    /// The artifact of an earlier build was reused because nothing it was built from changed; the source counts are
    /// that build's.
    bool upToDate = false;
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
