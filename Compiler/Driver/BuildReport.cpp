#include "Driver/BuildReport.h"

#include "BuildInfo/CompilerMetadata.h"
#include "Driver/BuildTarget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <sstream>

namespace Rux::Driver {
namespace {
struct ReportStyle {
    std::string_view green;
    std::string_view red;
    std::string_view cyan;
    std::string_view bold;
    std::string_view dim;
    std::string_view reset;
};

ReportStyle Style(const bool enabled) {
    if (!enabled) {
        return {};
    }
    return {.green = "\033[32m",
            .red = "\033[31m",
            .cyan = "\033[36m",
            .bold = "\033[1m",
            .dim = "\033[2m",
            .reset = "\033[0m"};
}

// The triple a report names. An embedder that leaves it unset built for the
// host, which is what the report said before it carried a target at all.
std::optional<Target::TargetTriple> ReportedTriple(const std::string_view targetTriple) {
    if (targetTriple.empty()) {
        return Target::TargetTriple::Host();
    }
    return Target::TargetTriple::Parse(targetTriple);
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

std::string FormatTokenThroughput(double tokensPerSecond) {
    const double absValue = std::fabs(tokensPerSecond);
    if (absValue >= 1'000'000.0) {
        return FormatDecimal(tokensPerSecond / 1'000'000.0, 1) + " M tok/s";
    }
    if (absValue >= 1'000.0) {
        return FormatDecimal(tokensPerSecond / 1'000.0, 1) + " K tok/s";
    }
    return FormatNumber(static_cast<std::uintmax_t>(std::llround(tokensPerSecond))) + " tok/s";
}

std::string FormatSize(std::uintmax_t bytes) {
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return FormatNumber(static_cast<std::uintmax_t>(std::llround(kb))) + " KB";
    }

    const double mb = kb / 1024.0;
    return FormatDecimal(mb, 2) + " MB";
}

std::string FormatDuration(std::chrono::milliseconds elapsed) {
    // Milliseconds below a second, seconds above: a four-digit millisecond count
    // is harder to read than the same span written as `1.23s`.
    if (elapsed < std::chrono::seconds(1)) {
        return FormatNumber(static_cast<std::uintmax_t>(elapsed.count())) + " ms";
    }
    return FormatDecimal(static_cast<double>(elapsed.count()) / 1000.0, 2) + "s";
}

std::string FormatBuildStats(const std::filesystem::path &exePath, std::string_view profileName,
                             const std::string_view targetTriple, const BuildStats &stats, const bool colorEnabled) {
    const auto totalMs = stats.total.count();
    const double seconds = stats.totalSeconds;
    const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
    const std::size_t totalLines = stats.localLines + stats.dependencyLines;
    const std::size_t totalTokens = stats.localTokens + stats.dependencyTokens;
    const std::uintmax_t totalSourceSize = stats.localSourceSize + stats.dependencySourceSize;
    const double tokenThroughput = seconds > 0.0 ? static_cast<double>(totalTokens) / seconds : 0.0;
    const double compileSpeed = seconds > 0.0 ? static_cast<double>(totalLines) / seconds : 0.0;
    const double throughput = seconds > 0.0 ? static_cast<double>(totalSourceSize) / 1024.0 / 1024.0 / seconds : 0.0;
    const std::size_t prunedDeclarations =
        stats.prunedFunctionDefinitions + stats.prunedConstants + stats.prunedVtables + stats.prunedExternDeclarations;

    const auto triple = ReportedTriple(targetTriple);
    const std::string canonical = triple ? std::string(triple->CanonicalName()) : std::string(targetTriple);
    const std::string display = triple ? triple->DisplayName() : canonical;
    const auto style = Style(colorEnabled);
    std::ostringstream output;
    output << style.bold << "Rux Compiler " << CompilerBuild::compilerVersion << style.reset << '\n'
           << "Target: " << display << " (" << canonical << ")\n"
           << "Mode: " << style.bold << profileName << style.reset << "\n\n"
           << style.green << style.bold << "Build finished successfully." << style.reset << "\n\n"
           << "Total build time:            " << style.bold << totalMs << " ms" << style.reset << '\n'
           << "  Lexing:                    " << stats.lexing.count() << " ms\n"
           << "  Parsing:                   " << stats.parsing.count() << " ms\n"
           << "  Semantic:                  " << stats.semantic.count() << " ms\n"
           << "  HIR:                       " << stats.hir.count() << " ms\n"
           << "  LIR:                       " << stats.lir.count() << " ms\n"
           << "  Codegen:                   " << stats.codegen.count() << " ms\n"
           << "  Linking:                   " << stats.linking.count() << " ms\n\n"
           << "Total files:                 " << FormatNumber(totalFiles) << '\n'
           << "  Local files:               " << FormatNumber(stats.localFiles) << '\n'
           << "  Dependency files:          " << FormatNumber(stats.dependencyFiles) << "\n\n"
           << "Total lines:                 " << FormatNumber(totalLines) << '\n'
           << "  Local lines:               " << FormatNumber(stats.localLines) << '\n'
           << "  Dependency lines:          " << FormatNumber(stats.dependencyLines) << "\n\n"
           << "Total tokens:                " << FormatNumber(totalTokens) << '\n'
           << "  Local tokens:              " << FormatNumber(stats.localTokens) << '\n'
           << "  Dependency tokens:         " << FormatNumber(stats.dependencyTokens) << "\n\n"
           << "Total source size:           " << FormatSize(totalSourceSize) << '\n'
           << "  Local source size:         " << FormatSize(stats.localSourceSize) << '\n'
           << "  Dependency source size:    " << FormatSize(stats.dependencySourceSize) << "\n\n"
           << "LIR declarations pruned:     " << FormatNumber(prunedDeclarations) << '\n'
           << "  Function definitions:      " << FormatNumber(stats.prunedFunctionDefinitions) << '\n'
           << "  Constants:                 " << FormatNumber(stats.prunedConstants) << '\n'
           << "  Vtables:                   " << FormatNumber(stats.prunedVtables) << '\n'
           << "  Extern declarations:       " << FormatNumber(stats.prunedExternDeclarations) << '\n'
           << "  Estimated IR eliminated:   " << FormatNumber(stats.estimatedLirNodesEliminated) << " nodes\n\n"
           << style.cyan << style.bold << "Output:" << style.reset << '\n'
           << "  Executable:                " << style.cyan << exePath.filename().string() << style.reset << '\n'
           << "  Executable size:           " << FormatSize(stats.executableSize) << '\n'
           << "  Peak memory:               " << FormatSize(stats.peakMemoryBytes) << "\n\n"
           << style.cyan << style.bold << "Performance:" << style.reset << '\n'
           << "  Compile speed:             " << FormatNumber(static_cast<std::uintmax_t>(std::llround(compileSpeed)))
           << " LOC/s\n"
           << "  Token throughput:          " << FormatTokenThroughput(tokenThroughput) << '\n'
           << "  Total throughput:          " << FormatDecimal(throughput, 2) << " MB/s\n";
    return output.str();
}

std::string FormatBuildSummary(const std::filesystem::path &exePath, std::string_view profileName,
                               const std::string_view targetTriple, const BuildStats &stats, const bool colorEnabled) {
    const auto triple = ReportedTriple(targetTriple);
    const std::string canonical = triple ? std::string(triple->CanonicalName()) : std::string(targetTriple);
    const std::string crossTarget =
        triple && *triple == Target::TargetTriple::Host() ? std::string{} : " for " + canonical;
    const auto totalMs = stats.total.count();
    const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
    const std::size_t totalLines = stats.localLines + stats.dependencyLines;
    const std::size_t totalTokens = stats.localTokens + stats.dependencyTokens;
    const double compileSpeed = stats.totalSeconds > 0.0 ? static_cast<double>(totalLines) / stats.totalSeconds : 0.0;

    const auto style = Style(colorEnabled);
    std::ostringstream output;
    output << style.green << style.bold << "Built" << style.reset << ' ' << style.bold << profileName << style.reset
           << crossTarget << " [" << style.cyan << exePath.string() << style.reset << "] in " << style.bold << totalMs
           << " ms" << style.reset << '\n'
           << style.dim << FormatNumber(totalFiles) << " files | " << FormatNumber(totalLines) << " LOC | "
           << FormatCompactNumber(static_cast<double>(totalTokens)) << " tokens | " << FormatCompactNumber(compileSpeed)
           << " LOC/s | " << exePath.filename().string() << ' ' << FormatSize(stats.executableSize) << style.reset
           << '\n';
    return output.str();
}

std::string FormatBuildMatrixReport(const std::span<const BuildCellReport> cells, const bool includeStats,
                                    const bool colorEnabled) {
    const auto style = Style(colorEnabled);
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
    output << style.bold << "Build matrix" << style.reset << '\n';
    output << style.cyan << style.bold << std::left << std::setw(8) << "Status" << std::setw(9) << "Profile"
           << std::setw(19) << "Target" << std::setw(10) << "Time";
    if (includeStats) {
        output << std::right << std::setw(8) << "Files" << std::setw(10) << "LOC" << std::setw(11) << "Tokens"
               << std::setw(10) << "Size" << "  ";
    }
    output << std::left << "Output" << style.reset << '\n';

    for (const auto &cell : cells) {
        const auto statusColor = cell.succeeded ? style.green : style.red;
        const auto outputPath = cell.succeeded ? cell.artifactPath : cell.outputDirectory;
        output << statusColor << style.bold << std::left << std::setw(8) << (cell.succeeded ? "Built" : "Failed")
               << style.reset << std::setw(9) << ToString(cell.profile) << std::setw(19) << cell.target.CanonicalName()
               << std::setw(10) << FormatDuration(cell.elapsed);
        if (includeStats) {
            output << std::right << std::setw(8) << FormatNumber(cell.stats.localFiles + cell.stats.dependencyFiles)
                   << std::setw(10) << FormatNumber(cell.stats.localLines + cell.stats.dependencyLines) << std::setw(11)
                   << FormatNumber(cell.stats.localTokens + cell.stats.dependencyTokens) << std::setw(10)
                   << FormatSize(cell.stats.executableSize) << "  ";
        }
        output << std::left << outputPath.string() << '\n';
    }

    const std::size_t failed = cells.size() - succeeded;
    output << '\n'
           << style.bold << cells.size() << " cells: " << style.green << succeeded << " succeeded" << style.reset
           << ", " << (failed == 0 ? style.dim : style.red) << failed << " failed" << style.reset << " in "
           << FormatDuration(totalElapsed) << style.reset << '\n';
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

void PrintBuildStats(const std::filesystem::path &exePath, std::string_view profileName,
                     const std::string_view targetTriple, const BuildStats &stats, const bool colorEnabled) {
    const auto report = FormatBuildStats(exePath, profileName, targetTriple, stats, colorEnabled);
    std::fwrite(report.data(), sizeof(char), report.size(), stdout);
}

void PrintBuildSummary(const std::filesystem::path &exePath, std::string_view profileName,
                       const std::string_view targetTriple, const BuildStats &stats, const bool colorEnabled) {
    const auto report = FormatBuildSummary(exePath, profileName, targetTriple, stats, colorEnabled);
    std::fwrite(report.data(), sizeof(char), report.size(), stdout);
}

void PrintBuildMatrixReport(const std::span<const BuildCellReport> cells, const bool includeStats,
                            const bool colorEnabled) {
    const auto report = FormatBuildMatrixReport(cells, includeStats, colorEnabled);
    std::fwrite(report.data(), sizeof(char), report.size(), stdout);
}
} // namespace Rux::Driver
