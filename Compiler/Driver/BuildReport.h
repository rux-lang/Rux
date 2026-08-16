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

// Everything a report needs to identify the build it describes. A struct
// rather than a parameter list: the fields are all strings and paths, so
// positional arguments would be easy to transpose and impossible to notice.
struct BuildReportInfo {
    std::string_view packageName;
    std::string_view packageVersion;
    std::filesystem::path artifactPath;
    std::filesystem::path packageRoot;
    BuildProfile profile = BuildProfile::Debug;
    std::string_view targetTriple;
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
[[nodiscard]] std::string FormatPercent(double share);
[[nodiscard]] std::string FormatSize(std::uintmax_t bytes);

// ---- Reporting --------------------------------------------------------------

// Artifact paths are printed relative to `packageRoot`, the directory holding
// the manifest, so a report reads `Bin/Debug/Linux/x86-64/App` rather than an
// absolute path. An output root outside that directory keeps its full path, and
// an empty `packageRoot` disables shortening altogether.
[[nodiscard]] std::string DisplayPath(const std::filesystem::path &path, const std::filesystem::path &packageRoot);

// The `(Debug, Windows x86-64)` suffix every progress and outcome line carries.
// Report prose uses display spellings; canonical target IDs are reserved for
// text naming a value the reader could pass back on the command line.
[[nodiscard]] std::string FormatBuildContext(BuildProfile profile, const Target::TargetTriple &target);
[[nodiscard]] std::string FormatBuildContext(const Target::TargetTriple &target);

// Every report opens with the same status line, so `--stats` is that summary
// plus its sections rather than a second, differently shaped report. Each fact
// appears once: the status line carries the profile and target, and the rows
// below it carry only what it does not.
[[nodiscard]] std::string FormatBuildStats(const BuildReportInfo &info, const BuildStats &stats, bool colorEnabled);
[[nodiscard]] std::string FormatBuildSummary(std::string_view packageName, const std::filesystem::path &artifactPath,
                                             const std::filesystem::path &packageRoot, BuildProfile profile,
                                             std::string_view targetTriple, const BuildStats &stats, bool colorEnabled);
[[nodiscard]] std::string FormatBuildMatrixReport(std::string_view packageName, std::span<const BuildCellReport> cells,
                                                  const std::filesystem::path &packageRoot, bool includeStats,
                                                  bool colorEnabled);
} // namespace Rux::Driver
