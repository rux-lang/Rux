#include "Driver/BuildReport.h"

#include "BuildInfo/CompilerMetadata.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "Reporting/Reporting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace Rux::Driver {
namespace {
/// The triple a report names. An embedder that leaves it unset built for the host, which is what the report said before
/// it carried a target at all.
std::optional<Target::TargetTriple> ReportedTriple(const std::string_view targetTriple) {
    if (targetTriple.empty()) {
        return Target::TargetTriple::Host();
    }
    return Target::TargetTriple::Parse(targetTriple);
}

/// `Built App (Debug, Windows x86-64) in 22 ms`. Both the one-line summary and the statistics report open with it, so
/// `--stats` reads as that summary plus its sections rather than as a second, differently shaped report.
std::string FormatBuildStatusLine(const BuildReportInfo &info, const BuildStats &stats, const Reporting::Style &style) {
    const auto triple = ReportedTriple(info.targetTriple);
    const std::string context = triple ? FormatBuildContext(info.profile, *triple)
                                       : std::format("({}, {})", ToString(info.profile), info.targetTriple);
    return std::format("{} {} {} in {}", Reporting::RenderStatus(Reporting::StatusVerb::Built, style), info.packageName,
                       context, Reporting::FormatDuration(stats.total));
}

/// A label column followed by right-aligned value columns, under an optional header row. Column widths come from the
/// cells themselves, so no width is written down anywhere and adding a row cannot break the alignment.
std::string RenderGrid(const std::span<const std::string> headers,
                       const std::span<const std::vector<std::string>> rows) {
    std::size_t columns = headers.size() + 1;
    for (const auto &row : rows) {
        columns = std::max(columns, row.size());
    }

    std::vector<std::size_t> widths(columns, 0);
    for (const auto &row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }
    for (std::size_t i = 0; i < headers.size(); ++i) {
        widths[i + 1] = std::max(widths[i + 1], headers[i].size());
    }

    // Value columns are separated by the standard indent, except the first,
    // which the label column's `: ` already separates.
    const auto gapBefore = [](const std::size_t column) {
        return column == 1 ? std::size_t{0} : Reporting::indentation.size();
    };

    std::string rendered;
    if (!headers.empty()) {
        rendered.append(Reporting::indentation.size() + widths[0] + 2, ' ');
        for (std::size_t i = 0; i < headers.size(); ++i) {
            rendered.append(widths[i + 1] - headers[i].size() + gapBefore(i + 1), ' ');
            rendered += headers[i];
        }
        rendered += '\n';
    }

    for (const auto &row : rows) {
        rendered += Reporting::indentation;
        rendered += row[0];
        rendered += ':';
        rendered.append(widths[0] - row[0].size() + 1, ' ');
        for (std::size_t i = 1; i < row.size(); ++i) {
            rendered.append(widths[i] - row[i].size() + gapBefore(i), ' ');
            rendered += row[i];
        }
        rendered += '\n';
    }
    return rendered;
}

std::string RenderTimeSection(const BuildStats &stats, const Reporting::Style &style) {
    using Phase = CompilePhase;
    const std::array<std::pair<Phase, std::chrono::milliseconds>, 7> phases{{{Phase::Lexing, stats.lexing},
                                                                             {Phase::Parsing, stats.parsing},
                                                                             {Phase::Analyzing, stats.semantic},
                                                                             {Phase::LoweringToHir, stats.hir},
                                                                             {Phase::LoweringToLir, stats.lir},
                                                                             {Phase::EmittingObjects, stats.codegen},
                                                                             {Phase::Linking, stats.linking}}};

    // `total` is wall clock while the phases are individual timers, so the
    // difference is real work — resolving dependencies, reading manifests,
    // touching the filesystem — that no phase timer covers. Naming it keeps
    // the column an accounting of the whole build instead of a partial one.
    std::chrono::milliseconds measured{0};
    for (const auto &[phase, elapsed] : phases) {
        measured += elapsed;
    }
    const auto other = std::max(stats.total - measured, std::chrono::milliseconds{0});
    const double totalMs = static_cast<double>(stats.total.count());
    // `part` is a share of the build, not a duration being formatted: the text
    // still comes from Reporting::FormatDuration.
    const auto share = [totalMs](const std::chrono::milliseconds part) {
        return totalMs > 0.0 ? 100.0 * static_cast<double>(part.count()) / totalMs : 0.0;
    };

    std::vector<std::vector<std::string>> rows;
    rows.reserve(phases.size() + 2);
    for (const auto &[phase, elapsed] : phases) {
        rows.push_back(
            {std::string(CompilePhaseName(phase)), Reporting::FormatDuration(elapsed), FormatPercent(share(elapsed))});
    }
    rows.push_back({"Other", Reporting::FormatDuration(other), FormatPercent(share(other))});
    rows.push_back({"Total", Reporting::FormatDuration(stats.total), FormatPercent(totalMs > 0.0 ? 100.0 : 0.0)});

    std::string rendered(style.Bold());
    rendered += "Time";
    rendered += style.Reset();
    rendered += ":\n";
    rendered += RenderGrid({}, rows);
    return rendered;
}

std::string RenderSourceSection(const BuildStats &stats, const Reporting::Style &style) {
    const std::array<std::string, 3> headers{"Local", "Dependency", "Total"};
    const std::vector<std::vector<std::string>> rows{
        {"Files", FormatNumber(stats.localFiles), FormatNumber(stats.dependencyFiles),
         FormatNumber(stats.localFiles + stats.dependencyFiles)},
        {"Lines", FormatNumber(stats.localLines), FormatNumber(stats.dependencyLines),
         FormatNumber(stats.localLines + stats.dependencyLines)},
        {"Tokens", FormatNumber(stats.localTokens), FormatNumber(stats.dependencyTokens),
         FormatNumber(stats.localTokens + stats.dependencyTokens)},
        {"Size", FormatSize(stats.localSourceSize), FormatSize(stats.dependencySourceSize),
         FormatSize(stats.localSourceSize + stats.dependencySourceSize)}};

    std::string rendered(style.Bold());
    rendered += "Source";
    rendered += style.Reset();
    rendered += ":\n";
    rendered += RenderGrid(headers, rows);
    return rendered;
}
} // namespace

std::size_t CountLines(std::string_view source) {
    if (source.empty()) {
        return 0;
    }

    std::size_t lines = 0;
    for (const char ch : source) {
        if (ch == '\n') {
            ++lines;
        }
    }
    if (source.back() != '\n') {
        ++lines;
    }
    return lines;
}

std::size_t CountTokens(const LexerResult &result) {
    if (result.tokens.empty()) {
        return 0;
    }
    return result.tokens.back().IsEof() ? result.tokens.size() - 1 : result.tokens.size();
}

std::string FormatNumber(std::uintmax_t value) {
    std::string digits = std::to_string(value);
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), 1, ',');
    }
    return digits;
}

std::string FormatDecimal(double value, int decimals) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    std::string text = oss.str();
    auto dot = text.find('.');
    if (dot == std::string::npos) {
        return text;
    }

    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

std::string FormatCompactNumber(double value) {
    const double absValue = std::fabs(value);
    if (absValue >= 1'000'000.0) {
        return FormatDecimal(value / 1'000'000.0, 1) + "M";
    }
    if (absValue >= 1'000.0) {
        return FormatDecimal(value / 1'000.0, 1) + "K";
    }
    return FormatNumber(static_cast<std::uintmax_t>(std::llround(value)));
}

std::string FormatPercent(double share) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << share << '%';
    return output.str();
}

std::string FormatSize(std::uintmax_t bytes) {
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return FormatNumber(static_cast<std::uintmax_t>(std::llround(kb))) + " KB";
    }

    const double mb = kb / 1024.0;
    return FormatDecimal(mb, 2) + " MB";
}

std::string DisplayPath(const std::filesystem::path &path, const std::filesystem::path &packageRoot) {
    if (packageRoot.empty()) {
        return path.string();
    }
    const auto relative = path.lexically_relative(packageRoot);
    const auto text = relative.string();
    // An output root outside the package keeps its full path rather than a
    // chain of parent references that is longer than the path it replaces.
    return text.empty() || text.starts_with("..") ? path.string() : text;
}

std::string FormatBuildContext(const BuildProfile profile, const Target::TargetTriple &target) {
    return std::format("({}, {})", ToString(profile), target.DisplayName());
}

std::string FormatBuildContext(const Target::TargetTriple &target) {
    return std::format("({})", target.DisplayName());
}

std::string FormatBuildStats(const BuildReportInfo &info, const BuildStats &stats, const bool colorEnabled) {
    const double seconds = stats.totalSeconds;
    const std::size_t totalLines = stats.localLines + stats.dependencyLines;
    const std::size_t totalTokens = stats.localTokens + stats.dependencyTokens;
    const std::uintmax_t totalSourceSize = stats.localSourceSize + stats.dependencySourceSize;
    const double tokenThroughput = seconds > 0.0 ? static_cast<double>(totalTokens) / seconds : 0.0;
    const double compileSpeed = seconds > 0.0 ? static_cast<double>(totalLines) / seconds : 0.0;
    const double throughput = seconds > 0.0 ? static_cast<double>(totalSourceSize) / 1024.0 / 1024.0 / seconds : 0.0;

    const Reporting::Style style{colorEnabled};
    std::ostringstream output;
    output << FormatBuildStatusLine(info, stats, style) << '\n';

    // The status line already carries the profile and target, so these rows
    // hold only what it does not.
    const std::string version = std::format("{} v{}", info.packageName, info.packageVersion);
    const std::string compiler = std::format("Rux {}", CompilerBuild::compilerVersion);
    const std::string artifact =
        std::format("{} ({})", DisplayPath(info.artifactPath, info.packageRoot), FormatSize(stats.executableSize));
    const std::array identity{Reporting::TableRow{"Package", version}, Reporting::TableRow{"Compiler", compiler},
                              Reporting::TableRow{"Output", artifact}};
    output << Reporting::RenderRows(identity) << '\n';

    output << RenderTimeSection(stats, style) << '\n';
    output << RenderSourceSection(stats, style) << '\n';

    // The four pruning counts sum to the total, so the total closes the list
    // rather than heading it. The IR estimate is not one of the four, so it
    // sits outside the sum and says that it is an estimate.
    const std::size_t prunedDeclarations =
        stats.prunedFunctionDefinitions + stats.prunedConstants + stats.prunedVtables + stats.prunedExternDeclarations;
    const std::string functions = FormatNumber(stats.prunedFunctionDefinitions);
    const std::string constants = FormatNumber(stats.prunedConstants);
    const std::string vtables = FormatNumber(stats.prunedVtables);
    const std::string externs = FormatNumber(stats.prunedExternDeclarations);
    const std::string pruned = FormatNumber(prunedDeclarations);
    // "Estimated" qualifies the row, not the number, so it stays in the label
    // and leaves the value column holding bare counts that compare.
    const std::string irNodes = FormatNumber(stats.estimatedLirNodesEliminated);
    const std::array optimization{Reporting::TableRow{"Function definitions", functions},
                                  Reporting::TableRow{"Constants", constants},
                                  Reporting::TableRow{"Vtables", vtables},
                                  Reporting::TableRow{"Extern declarations", externs},
                                  Reporting::TableRow{"Declarations pruned", pruned},
                                  Reporting::TableRow{"Estimated IR nodes", irNodes}};
    output << Reporting::RenderSection("Optimization", optimization, style, Reporting::ValueAlign::Right) << '\n';

    // Compact counts here match the one-line build summary, which already
    // reports LOC/s and token totals that way. These four values carry four
    // different units, so right alignment would square up nothing.
    const std::string speed = FormatCompactNumber(compileSpeed) + " LOC/s";
    const std::string tokens = FormatCompactNumber(tokenThroughput) + " tok/s";
    const std::string source = FormatDecimal(throughput, 2) + " MB/s";
    const std::string memory = FormatSize(stats.peakMemoryBytes);
    const std::array performance{
        Reporting::TableRow{"Compile speed", speed}, Reporting::TableRow{"Token throughput", tokens},
        Reporting::TableRow{"Source throughput", source}, Reporting::TableRow{"Peak memory", memory}};
    output << Reporting::RenderSection("Performance", performance, style);
    return output.str();
}

std::string FormatBuildSummary(const std::string_view packageName, const std::filesystem::path &artifactPath,
                               const std::filesystem::path &packageRoot, const BuildProfile profile,
                               const std::string_view targetTriple, const BuildStats &stats, const bool colorEnabled) {
    const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
    const std::size_t totalLines = stats.localLines + stats.dependencyLines;
    const std::size_t totalTokens = stats.localTokens + stats.dependencyTokens;
    const double compileSpeed = stats.totalSeconds > 0.0 ? static_cast<double>(totalLines) / stats.totalSeconds : 0.0;
    const BuildReportInfo info{.packageName = packageName,
                               .packageVersion = {},
                               .artifactPath = {},
                               .packageRoot = {},
                               .profile = profile,
                               .targetTriple = targetTriple};

    const Reporting::Style style{colorEnabled};
    std::ostringstream output;
    output << FormatBuildStatusLine(info, stats, style) << '\n'
           << Reporting::indentation << "Output: " << style.Cyan() << DisplayPath(artifactPath, packageRoot)
           << style.Reset() << '\n'
           << Reporting::indentation << style.Dim() << FormatNumber(totalFiles) << ' '
           << Reporting::Pluralize(totalFiles, "file") << " | " << FormatNumber(totalLines) << " LOC | "
           << FormatCompactNumber(static_cast<double>(totalTokens)) << " tokens | " << FormatCompactNumber(compileSpeed)
           << " LOC/s | " << artifactPath.filename().string() << ' ' << FormatSize(stats.executableSize)
           << style.Reset() << '\n';
    return output.str();
}

std::string FormatBuildMatrixReport(const std::string_view packageName, const std::span<const BuildCellReport> cells,
                                    const std::filesystem::path &packageRoot, const bool includeStats,
                                    const bool colorEnabled) {
    const Reporting::Style style{colorEnabled};
    std::size_t succeeded = 0;
    std::chrono::milliseconds totalElapsed{0};
    BuildStats aggregate;
    for (const auto &cell : cells) {
        succeeded += cell.succeeded ? 1U : 0U;
        totalElapsed += cell.elapsed;
        aggregate.lexing += cell.stats.lexing;
        aggregate.parsing += cell.stats.parsing;
        aggregate.semantic += cell.stats.semantic;
        aggregate.hir += cell.stats.hir;
        aggregate.lir += cell.stats.lir;
        aggregate.codegen += cell.stats.codegen;
        aggregate.linking += cell.stats.linking;
        aggregate.localFiles += cell.stats.localFiles;
        aggregate.dependencyFiles += cell.stats.dependencyFiles;
        aggregate.localLines += cell.stats.localLines;
        aggregate.dependencyLines += cell.stats.dependencyLines;
        aggregate.localTokens += cell.stats.localTokens;
        aggregate.dependencyTokens += cell.stats.dependencyTokens;
        aggregate.localSourceSize += cell.stats.localSourceSize;
        aggregate.dependencySourceSize += cell.stats.dependencySourceSize;
        aggregate.executableSize += cell.stats.executableSize;
        aggregate.peakMemoryBytes = std::max(aggregate.peakMemoryBytes, cell.stats.peakMemoryBytes);
        aggregate.prunedFunctionDefinitions += cell.stats.prunedFunctionDefinitions;
        aggregate.prunedConstants += cell.stats.prunedConstants;
        aggregate.prunedVtables += cell.stats.prunedVtables;
        aggregate.prunedExternDeclarations += cell.stats.prunedExternDeclarations;
        aggregate.estimatedLirNodesEliminated += cell.stats.estimatedLirNodesEliminated;
    }

    std::ostringstream output;
    output << style.Bold() << "Build matrix for " << packageName << style.Reset() << '\n';
    output << style.Cyan() << style.Bold() << std::left << std::setw(8) << "Status" << std::setw(9) << "Profile"
           << std::setw(20) << "Target" << std::setw(10) << "Time";
    if (includeStats) {
        output << std::right << std::setw(8) << "Files" << std::setw(10) << "LOC" << std::setw(11) << "Tokens"
               << std::setw(10) << "Size" << "  ";
    }
    output << std::left << "Output" << style.Reset() << '\n';

    for (const auto &cell : cells) {
        const auto status = cell.succeeded ? Reporting::StatusVerb::Built : Reporting::StatusVerb::Failed;
        const auto outputPath = cell.succeeded ? cell.artifactPath : cell.outputDirectory;
        output << style.Color(Reporting::KindOf(status)) << style.Bold() << std::left << std::setw(8)
               << Reporting::StatusText(status) << style.Reset() << std::setw(9) << ToString(cell.profile)
               << std::setw(20) << cell.target.DisplayName() << std::setw(10)
               << Reporting::FormatDuration(cell.elapsed);
        if (includeStats) {
            output << std::right << std::setw(8) << FormatNumber(cell.stats.localFiles + cell.stats.dependencyFiles)
                   << std::setw(10) << FormatNumber(cell.stats.localLines + cell.stats.dependencyLines) << std::setw(11)
                   << FormatNumber(cell.stats.localTokens + cell.stats.dependencyTokens) << std::setw(10)
                   << FormatSize(cell.stats.executableSize) << "  ";
        }
        output << std::left << DisplayPath(outputPath, packageRoot) << '\n';
    }

    const std::size_t failed = cells.size() - succeeded;
    const auto overall = failed == 0 ? Reporting::StatusVerb::Built : Reporting::StatusVerb::Failed;
    output << '\n'
           << Reporting::RenderStatus(overall, style) << ' ' << Reporting::FormatCount(cells.size(), "cell") << " in "
           << style.Bold() << Reporting::FormatDuration(totalElapsed) << style.Reset() << " (" << style.Green()
           << succeeded << " succeeded" << style.Reset() << ", " << (failed == 0 ? style.Dim() : style.Red()) << failed
           << " failed" << style.Reset() << ")\n";
    if (includeStats) {
        output << "Aggregate statistics: " << FormatNumber(aggregate.localFiles + aggregate.dependencyFiles)
               << " files | " << FormatNumber(aggregate.localLines + aggregate.dependencyLines) << " LOC | "
               << FormatNumber(aggregate.localTokens + aggregate.dependencyTokens) << " tokens | "
               << FormatSize(aggregate.localSourceSize + aggregate.dependencySourceSize) << " source | "
               << FormatSize(aggregate.executableSize) << " artifacts | " << FormatSize(aggregate.peakMemoryBytes)
               << " peak memory | "
               << FormatNumber(aggregate.prunedFunctionDefinitions + aggregate.prunedConstants +
                               aggregate.prunedVtables + aggregate.prunedExternDeclarations)
               << " LIR declarations pruned\n";
    }
    return output.str();
}

} // namespace Rux::Driver
