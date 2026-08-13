#include "Driver/BuildReport.h"

#include "BuildInfo/CompilerMetadata.h"
#include "Driver/BuildTarget.h"

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <sstream>

namespace Rux::Driver {
namespace {
struct ReportStyle {
    std::string_view green;
    std::string_view cyan;
    std::string_view bold;
    std::string_view dim;
    std::string_view reset;
};

ReportStyle Style(const bool enabled) {
    if (!enabled) {
        return {};
    }
    return {.green = "\033[32m", .cyan = "\033[36m", .bold = "\033[1m", .dim = "\033[2m", .reset = "\033[0m"};
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
} // namespace Rux::Driver
