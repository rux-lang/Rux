#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"

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
    const std::filesystem::path root = "Workspace/App";
    const auto artifact =
        std::filesystem::path("Bin") / "Debug" / TargetOutputPath(Rux::Target::TargetTriple::Host()) / "App.exe";
    const auto executable = root / artifact;

    const auto plain =
        FormatBuildSummary("App", executable, root, Rux::BuildProfile::Debug, HostTargetTriple(), stats, false);
    CHECK_FALSE(plain.contains("\033["));
    CHECK(plain.contains("Built App (debug, " + HostTargetTriple() + ") in 275 ms"));
    CHECK(plain.contains("  Output: " + artifact.string()));

    const auto colored =
        FormatBuildSummary("App", executable, root, Rux::BuildProfile::Debug, HostTargetTriple(), stats, true);
    CHECK(colored.contains("\033[32m\033[1mBuilt\033[0m"));
    CHECK(colored.contains("\033[36m" + artifact.string() + "\033[0m"));
    CHECK(colored.contains("\033[2m93 files"));
}

TEST_CASE("Reported paths are relative to the package root only when they sit below it") {
    const std::filesystem::path root = "Workspace/App";
    CHECK(DisplayPath(root / "Bin" / "Debug" / "App.exe", root) ==
          (std::filesystem::path("Bin") / "Debug" / "App.exe").string());
    // A configured output root outside the package keeps its full path instead
    // of a parent chain longer than the path it replaces.
    CHECK(DisplayPath("Workspace/Shared/Bin/App.exe", root) == "Workspace/Shared/Bin/App.exe");
    CHECK(DisplayPath("Bin/App.exe", {}) == "Bin/App.exe");
}

TEST_CASE("Build summary always names its profile and canonical target") {
    BuildStats stats;
    stats.total = std::chrono::milliseconds(275);
    // The host is one triple, so naming a second one leaves a target that is
    // foreign wherever this runs — on an AArch64 machine as much as on x86-64.
    const std::string foreign = HostTargetTriple() == "windows-aarch64" ? "linux-aarch64" : "windows-aarch64";
    const std::filesystem::path executable = std::filesystem::path("Bin/Debug") / foreign / "App";

    CHECK(FormatBuildSummary("App", executable, {}, Rux::BuildProfile::Debug, foreign, stats, false)
              .contains("Built App (debug, " + foreign + ")"));
    // An unset target means the host, which is how the summary read before it
    // carried a target at all.
    CHECK(FormatBuildSummary("App", executable, {}, Rux::BuildProfile::Debug, {}, stats, false)
              .contains("Built App (debug, " + HostTargetTriple() + ")"));
}

TEST_CASE("Detailed build report styles success and section headings") {
    BuildStats stats;
    stats.prunedFunctionDefinitions = 2;
    stats.prunedConstants = 1;
    stats.prunedVtables = 1;
    stats.prunedExternDeclarations = 3;
    stats.estimatedLirNodesEliminated = 42;
    const auto plain =
        FormatBuildStats("App", "App/Bin/App.exe", "App", Rux::BuildProfile::Release, HostTargetTriple(), stats, false);
    const auto colored =
        FormatBuildStats("App", "App/Bin/App.exe", "App", Rux::BuildProfile::Release, HostTargetTriple(), stats, true);

    CHECK_FALSE(plain.contains("\033["));
    CHECK(colored.contains("\033[32m\033[1mBuilt\033[0m App (release,"));
    CHECK(colored.contains("\033[36m\033[1mOutput:\033[0m"));
    CHECK(colored.contains("\033[36m\033[1mPerformance:\033[0m"));
    CHECK(plain.contains("Artifact:                  " + (std::filesystem::path("Bin") / "App.exe").string() + '\n'));
    CHECK(plain.contains("LIR declarations pruned:     7\n"));
    CHECK(plain.contains("Estimated IR eliminated:   42 nodes\n"));
}

TEST_CASE("Detailed build report names the target it built for") {
    BuildStats stats;
    const auto report = FormatBuildStats("App", "Bin/Release/Windows/AArch64/App.exe", {}, Rux::BuildProfile::Release,
                                         "windows-arm64", stats, false);

    CHECK(report.contains("Target: Windows AArch64 (windows-aarch64)\n"));
}

TEST_CASE("Build matrix report retains ordered cell outcomes and aggregate status") {
    const auto freeBsd = Rux::Target::TargetTriple::Parse("freebsd-x86_64");
    const auto linux = Rux::Target::TargetTriple::Parse("linux-aarch64");
    REQUIRE(freeBsd);
    REQUIRE(linux);
    const std::filesystem::path root = "Workspace/App";
    const auto freeBsdDir = root / "Bin" / "Debug" / TargetOutputPath(*freeBsd);
    const auto linuxDir = root / "Bin" / "Debug" / TargetOutputPath(*linux);
    std::vector<BuildCellReport> cells{
        {.profile = Rux::BuildProfile::Debug,
         .target = *freeBsd,
         .outputDirectory = freeBsdDir,
         .succeeded = true,
         .artifactPath = freeBsdDir / "App",
         .stats = {},
         .elapsed = std::chrono::milliseconds(12)},
        {.profile = Rux::BuildProfile::Debug,
         .target = *linux,
         .outputDirectory = linuxDir,
         .succeeded = false,
         .artifactPath = {},
         .stats = {},
         .elapsed = std::chrono::milliseconds(8)},
    };

    const auto report = FormatBuildMatrixReport("App", cells, root, false, false);

    CHECK_FALSE(report.contains("\033["));
    CHECK(report.contains("Status  Profile  Target              Time"));
    CHECK(report.find("FreeBSD") < report.find("Linux"));
    CHECK(report.contains("Built   debug    freebsd-x86_64"));
    CHECK(report.contains("Failed  debug    linux-aarch64"));
    CHECK(report.contains((std::filesystem::path("Bin") / "Debug" / TargetOutputPath(*freeBsd) / "App").string()));
    CHECK(report.contains((std::filesystem::path("Bin") / "Debug" / TargetOutputPath(*linux)).string()));
    CHECK_FALSE(report.contains("Workspace"));
    CHECK(report.contains("Failed 2 cells in 20 ms (1 succeeded, 1 failed)"));
}

TEST_CASE("Successful build matrix uses the canonical completed summary and singular label") {
    const auto target = Rux::Target::TargetTriple::Parse("linux-x86_64");
    REQUIRE(target);
    const std::vector<BuildCellReport> cells{{.profile = Rux::BuildProfile::Release,
                                              .target = *target,
                                              .outputDirectory = "Bin/Release/Linux/x86-64",
                                              .succeeded = true,
                                              .artifactPath = "Bin/Release/Linux/x86-64/App",
                                              .stats = {},
                                              .elapsed = std::chrono::milliseconds(1230)}};

    const auto report = FormatBuildMatrixReport("App", cells, {}, false, false);
    CHECK(report.contains("Built 1 cell in 1.23 s (1 succeeded, 0 failed)"));
}

TEST_CASE("Build matrix stats report includes per-cell and aggregate values with semantic color") {
    const auto target = Rux::Target::TargetTriple::Parse("windows-aarch64");
    REQUIRE(target);
    BuildStats stats;
    stats.localFiles = 2;
    stats.dependencyFiles = 1;
    stats.localLines = 120;
    stats.dependencyLines = 30;
    stats.localTokens = 800;
    stats.dependencyTokens = 200;
    stats.localSourceSize = 6 * 1024;
    stats.dependencySourceSize = 2 * 1024;
    stats.executableSize = 16 * 1024;
    stats.peakMemoryBytes = 32 * 1024;
    stats.prunedFunctionDefinitions = 2;
    stats.prunedExternDeclarations = 1;
    const auto outputDir = std::filesystem::path("Bin") / "Release" / TargetOutputPath(*target);
    const std::vector<BuildCellReport> cells{{.profile = Rux::BuildProfile::Release,
                                              .target = *target,
                                              .outputDirectory = outputDir,
                                              .succeeded = true,
                                              .artifactPath = outputDir / "App.exe",
                                              .stats = stats,
                                              .elapsed = std::chrono::milliseconds(25)}};

    const auto report = FormatBuildMatrixReport("App", cells, {}, true, true);

    CHECK(report.contains("Files"));
    CHECK(report.contains("LOC"));
    CHECK(report.contains("Tokens"));
    CHECK(report.contains("Aggregate statistics: 3 files | 150 LOC | 1,000 tokens | 8 KB source | 16 KB artifacts | "
                          "32 KB peak memory | 3 LIR declarations pruned"));
    CHECK(report.contains("\033[32m\033[1mBuilt"));
    CHECK_FALSE(report.contains("\033[31m"));
}
