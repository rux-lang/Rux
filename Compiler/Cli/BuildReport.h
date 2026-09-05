#pragma once

#include "Driver/BuildStats.h"

#include <span>
#include <string>

namespace Rux::CliSupport {
using Driver::BuildCellReport;
using Driver::BuildStats;

/// Everything a report needs to identify the build it describes. A struct rather than a parameter list: the fields are
/// all strings and paths, so positional arguments would be easy to transpose and impossible to notice.
struct BuildReportInfo {
    std::string_view packageName;
    std::string_view packageVersion;
    std::filesystem::path artifactPath;
    std::filesystem::path packageRoot;
    BuildProfile profile = BuildProfile::Debug;
    std::string_view targetTriple;
};

// ---- Number formatting ------------------------------------------------------

[[nodiscard]] std::string FormatNumber(std::uintmax_t value);
[[nodiscard]] std::string FormatDecimal(double value, int decimals);
[[nodiscard]] std::string FormatCompactNumber(double value);
[[nodiscard]] std::string FormatPercent(double share);
[[nodiscard]] std::string FormatSize(std::uintmax_t bytes);

// ---- Reporting --------------------------------------------------------------

/// Artifact paths are printed relative to `packageRoot`, the directory holding the manifest, so a report reads
/// `Bin/Debug/Linux/x86-64/App` rather than an absolute path. An output root outside that directory keeps its full
/// path, and an empty `packageRoot` disables shortening altogether.
[[nodiscard]] std::string DisplayPath(const std::filesystem::path &path, const std::filesystem::path &packageRoot);

/// The `(Debug, Windows x86-64)` suffix every progress and outcome line carries. Report prose uses display spellings;
/// canonical target IDs are reserved for text naming a value the reader could pass back on the command line.
[[nodiscard]] std::string FormatBuildContext(BuildProfile profile, const Target::TargetTriple &target);
[[nodiscard]] std::string FormatBuildContext(const Target::TargetTriple &target);

/// Every report opens with the same status line, so `--stats` is that summary plus its sections rather than a second,
/// differently shaped report. Each fact appears once: the status line carries the profile and target, and the rows
/// below it carry only what it does not.
[[nodiscard]] std::string FormatBuildStats(const BuildReportInfo &info, const BuildStats &stats, bool colorEnabled);
[[nodiscard]] std::string FormatBuildSummary(std::string_view packageName, const std::filesystem::path &artifactPath,
                                             const std::filesystem::path &packageRoot, BuildProfile profile,
                                             std::string_view targetTriple, const BuildStats &stats, bool colorEnabled);
[[nodiscard]] std::string FormatBuildMatrixReport(std::string_view packageName, std::span<const BuildCellReport> cells,
                                                  const std::filesystem::path &packageRoot, bool includeStats,
                                                  bool colorEnabled);
} // namespace Rux::CliSupport
