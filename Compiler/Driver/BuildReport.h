#pragma once

// Build statistics aggregation, human-readable number formatting, and the
// build-report printers used by the `build` command.

#include "Lexer/Lexer.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

// ---- Reporting --------------------------------------------------------------

void PrintBuildStats(const std::filesystem::path &exePath, std::string_view profileName, const BuildStats &stats);
void PrintBuildSummary(const std::filesystem::path &exePath, std::string_view profileName, const BuildStats &stats);
} // namespace Rux::Driver
