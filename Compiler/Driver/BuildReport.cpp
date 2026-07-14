#include "Driver/BuildReport.h"

#include "Driver/BuildTarget.h"
#include "Driver/Version.h"

#include <cmath>
#include <iomanip>
#include <print>
#include <sstream>

namespace Rux::Driver {
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

void PrintBuildStats(const std::filesystem::path &exePath, std::string_view profileName, const BuildStats &stats) {
    const auto totalMs = stats.total.count();
    const double seconds = stats.totalSeconds;
    const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
    const std::size_t totalLines = stats.localLines + stats.dependencyLines;
    const std::size_t totalTokens = stats.localTokens + stats.dependencyTokens;
    const std::uintmax_t totalSourceSize = stats.localSourceSize + stats.dependencySourceSize;
    const double tokenThroughput = seconds > 0.0 ? static_cast<double>(totalTokens) / seconds : 0.0;
    const double compileSpeed = seconds > 0.0 ? static_cast<double>(totalLines) / seconds : 0.0;
    const double throughput = seconds > 0.0 ? static_cast<double>(totalSourceSize) / 1024.0 / 1024.0 / seconds : 0.0;

    std::print("Rux Compiler {}\n"
               "Target: {}\n"
               "Mode: {}\n\n"
               "Build finished successfully.\n\n"
               "Total build time:            {} ms\n"
               "  Lexing:                    {} ms\n"
               "  Parsing:                   {} ms\n"
               "  Semantic:                  {} ms\n"
               "  HIR:                       {} ms\n"
               "  LIR:                       {} ms\n"
               "  Codegen:                   {} ms\n"
               "  Linking:                   {} ms\n\n"
               "Total files:                 {}\n"
               "  Local files:               {}\n"
               "  Dependency files:          {}\n\n"
               "Total lines:                 {}\n"
               "  Local lines:               {}\n"
               "  Dependency lines:          {}\n\n"
               "Total tokens:                {}\n"
               "  Local tokens:              {}\n"
               "  Dependency tokens:         {}\n\n"
               "Total source size:           {}\n"
               "  Local source size:         {}\n"
               "  Dependency source size:    {}\n\n"
               "Output:\n"
               "  Executable:                {}\n"
               "  Executable size:           {}\n"
               "  Peak memory:               {}\n\n"
               "Performance:\n"
               "  Compile speed:             {} LOC/s\n"
               "  Token throughput:          {}\n"
               "  Total throughput:          {} MB/s\n",
               RUX_VERSION, TargetName(), profileName, totalMs, stats.lexing.count(), stats.parsing.count(),
               stats.semantic.count(), stats.hir.count(), stats.lir.count(), stats.codegen.count(),
               stats.linking.count(), FormatNumber(totalFiles), FormatNumber(stats.localFiles),
               FormatNumber(stats.dependencyFiles), FormatNumber(totalLines), FormatNumber(stats.localLines),
               FormatNumber(stats.dependencyLines), FormatNumber(totalTokens), FormatNumber(stats.localTokens),
               FormatNumber(stats.dependencyTokens), FormatSize(totalSourceSize), FormatSize(stats.localSourceSize),
               FormatSize(stats.dependencySourceSize), exePath.filename().string(), FormatSize(stats.executableSize),
               FormatSize(stats.peakMemoryBytes), FormatNumber(static_cast<std::uintmax_t>(std::llround(compileSpeed))),
               FormatTokenThroughput(tokenThroughput), FormatDecimal(throughput, 2));
}

void PrintBuildSummary(const std::filesystem::path &exePath, std::string_view profileName, const BuildStats &stats) {
    const auto totalMs = stats.total.count();
    const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
    const std::size_t totalLines = stats.localLines + stats.dependencyLines;
    const std::size_t totalTokens = stats.localTokens + stats.dependencyTokens;
    const double compileSpeed = stats.totalSeconds > 0.0 ? static_cast<double>(totalLines) / stats.totalSeconds : 0.0;

    std::print("Built `{}` [{}] in {} ms\n", profileName, exePath.string(), totalMs);
    std::print("{} files | {} LOC | {} tokens | {} LOC/s | {} {}\n", FormatNumber(totalFiles), FormatNumber(totalLines),
               FormatCompactNumber(static_cast<double>(totalTokens)), FormatCompactNumber(compileSpeed),
               exePath.filename().string(), FormatSize(stats.executableSize));
}
} // namespace Rux::Driver
