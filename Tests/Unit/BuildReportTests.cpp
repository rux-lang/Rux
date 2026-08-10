#include "Driver/BuildReport.h"

#include <doctest.h>

using namespace Rux::Driver;

TEST_CASE("CountLines counts newline-terminated and trailing lines") {
    CHECK(CountLines("") == 0);
    CHECK(CountLines("one line") == 1);
    CHECK(CountLines("a\nb") == 2);
    CHECK(CountLines("a\nb\n") == 2);
    CHECK(CountLines("\n\n") == 2);
}

TEST_CASE("FormatNumber groups digits with commas") {
    CHECK(FormatNumber(0) == "0");
    CHECK(FormatNumber(999) == "999");
    CHECK(FormatNumber(1000) == "1,000");
    CHECK(FormatNumber(1234567) == "1,234,567");
}

TEST_CASE("FormatDecimal trims trailing zeros") {
    CHECK(FormatDecimal(2.0, 2) == "2");
    CHECK(FormatDecimal(1.5, 2) == "1.5");
    CHECK(FormatDecimal(1.25, 2) == "1.25");
    CHECK(FormatDecimal(1.204, 2) == "1.2");
}

TEST_CASE("FormatCompactNumber abbreviates thousands and millions") {
    CHECK(FormatCompactNumber(950.0) == "950");
    CHECK(FormatCompactNumber(1500.0) == "1.5K");
    CHECK(FormatCompactNumber(2'000'000.0) == "2M");
}

TEST_CASE("FormatTokenThroughput picks a unit per magnitude") {
    CHECK(FormatTokenThroughput(500.0) == "500 tok/s");
    CHECK(FormatTokenThroughput(1500.0) == "1.5 K tok/s");
    CHECK(FormatTokenThroughput(2'000'000.0) == "2 M tok/s");
}

TEST_CASE("FormatDuration reports milliseconds below one second and seconds above") {
    using namespace std::chrono_literals;
    CHECK(FormatDuration(0ms) == "0 ms");
    CHECK(FormatDuration(842ms) == "842 ms");
    CHECK(FormatDuration(999ms) == "999 ms");
    CHECK(FormatDuration(1000ms) == "1s");
    CHECK(FormatDuration(1230ms) == "1.23s");
    CHECK(FormatDuration(12500ms) == "12.5s");
}

TEST_CASE("FormatSize reports KB below one MB and MB above") {
    CHECK(FormatSize(512) == "1 KB");
    CHECK(FormatSize(10 * 1024) == "10 KB");
    CHECK(FormatSize(2 * 1024 * 1024) == "2 MB");
    CHECK(FormatSize(1536 * 1024) == "1.5 MB");
}

TEST_CASE("Build summary applies semantic ANSI styling only when enabled") {
    BuildStats stats;
    stats.total = std::chrono::milliseconds(275);
    stats.totalSeconds = 0.275;
    stats.localFiles = 93;
    stats.localLines = 2'983;
    stats.localTokens = 11'800;
    stats.executableSize = 91 * 1024;
    const std::filesystem::path executable = "Bin/Debug/App.exe";

    const auto plain = FormatBuildSummary(executable, "Debug", stats, false);
    CHECK_FALSE(plain.contains("\033["));
    CHECK(plain.contains("Built Debug [Bin/Debug/App.exe] in 275 ms"));

    const auto colored = FormatBuildSummary(executable, "Debug", stats, true);
    CHECK(colored.contains("\033[32m\033[1mBuilt\033[0m"));
    CHECK(colored.contains("\033[36mBin/Debug/App.exe\033[0m"));
    CHECK(colored.contains("\033[2m93 files"));
}

TEST_CASE("Detailed build report styles success and section headings") {
    BuildStats stats;
    const auto plain = FormatBuildStats("Bin/App.exe", "Release", stats, false);
    const auto colored = FormatBuildStats("Bin/App.exe", "Release", stats, true);

    CHECK_FALSE(plain.contains("\033["));
    CHECK(colored.contains("\033[32m\033[1mBuild finished successfully.\033[0m"));
    CHECK(colored.contains("\033[36m\033[1mOutput:\033[0m"));
    CHECK(colored.contains("\033[36m\033[1mPerformance:\033[0m"));
}
