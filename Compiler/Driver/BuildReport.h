#pragma once

// Build statistics aggregation, human-readable number formatting, and the
// build-report printers used by the `build` command.

#include "BuildInfo/BuildProfile.h"
#include "Lexer/Lexer.h"
#include "Target/TargetTriple.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

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
[[nodiscard]] std::size_t CountTokens(const LexerResult &result);

// ---- Number formatting ------------------------------------------------------

[[nodiscard]] std::string FormatNumber(std::uintmax_t value);
[[nodiscard]] std::string FormatDecimal(double value, int decimals);
[[nodiscard]] std::string FormatCompactNumber(double value);
[[nodiscard]] std::string FormatTokenThroughput(double tokensPerSecond);
[[nodiscard]] std::string FormatSize(std::uintmax_t bytes);
[[nodiscard]] std::string FormatDuration(std::chrono::milliseconds elapsed);

// ---- Reporting --------------------------------------------------------------

// `targetTriple` is the "os-arch" triple the artifact was built for. The
// detailed report always names it; the one-line summary mentions it only when
// it is not the host, so an ordinary build reads as it always has.
[[nodiscard]] std::string FormatBuildStats(const std::filesystem::path &exePath, std::string_view profileName,
                                           std::string_view targetTriple, const BuildStats &stats, bool colorEnabled);
[[nodiscard]] std::string FormatBuildSummary(const std::filesystem::path &exePath, std::string_view profileName,
                                             std::string_view targetTriple, const BuildStats &stats, bool colorEnabled);
[[nodiscard]] std::string FormatBuildMatrixReport(std::span<const BuildCellReport> cells, bool includeStats,
                                                  bool colorEnabled);
void PrintBuildStats(const std::filesystem::path &exePath, std::string_view profileName, std::string_view targetTriple,
                     const BuildStats &stats, bool colorEnabled);
void PrintBuildSummary(const std::filesystem::path &exePath, std::string_view profileName,
                       std::string_view targetTriple, const BuildStats &stats, bool colorEnabled);
void PrintBuildMatrixReport(std::span<const BuildCellReport> cells, bool includeStats, bool colorEnabled);
} // namespace Rux::Driver
