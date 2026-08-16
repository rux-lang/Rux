#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"

#include <doctest.h>

using namespace Rux::Driver;

namespace {
// Column alignment is pinned exactly by the Source grid and by the shared
// Reporting renderers. Rows whose padding shifts with the values they hold are
// checked for content instead, so a changed number does not rewrite a test.
std::string Squeeze(const std::string &text) {
    std::string squeezed;
    bool inRun = false;
    for (const char ch : text) {
        if (ch == ' ') {
            inRun = true;
            continue;
        }
        if (inRun && !squeezed.empty() && squeezed.back() != '\n') {
            squeezed += ' ';
        }
        inRun = false;
        squeezed += ch;
    }
    return squeezed;
}
} // namespace

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

TEST_CASE("FormatPercent always carries one decimal place") {
    CHECK(FormatPercent(0.0) == "0.0%");
    CHECK(FormatPercent(4.5) == "4.5%");
    CHECK(FormatPercent(63.64) == "63.6%");
    CHECK(FormatPercent(100.0) == "100.0%");
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
    CHECK(plain.contains("Built App (Debug, " + TargetDisplayName(Rux::Target::TargetTriple::Host()) + ") in 275 ms"));
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

TEST_CASE("Build summary always names its profile and target in display spelling") {
    BuildStats stats;
    stats.total = std::chrono::milliseconds(275);
    // The host is one triple, so naming a second one leaves a target that is
    // foreign wherever this runs — on an AArch64 machine as much as on x86-64.
    const std::string foreign = HostTargetTriple() == "windows-aarch64" ? "linux-aarch64" : "windows-aarch64";
    const std::string foreignDisplay = foreign == "windows-aarch64" ? "Windows AArch64" : "Linux AArch64";
    const std::filesystem::path executable = std::filesystem::path("Bin/Debug") / foreign / "App";

    // The canonical triple goes in, the display spelling comes out: report
    // prose never repeats a machine-facing ID.
    CHECK(FormatBuildSummary("App", executable, {}, Rux::BuildProfile::Debug, foreign, stats, false)
              .contains("Built App (Debug, " + foreignDisplay + ")"));
    // An unset target means the host, which is how the summary read before it
    // carried a target at all.
    CHECK(FormatBuildSummary("App", executable, {}, Rux::BuildProfile::Debug, {}, stats, false)
              .contains("Built App (Debug, " + TargetDisplayName(Rux::Target::TargetTriple::Host()) + ")"));
}

TEST_CASE("Detailed build report titles every section and states each fact once") {
    BuildStats stats;
    stats.prunedFunctionDefinitions = 2;
    stats.prunedConstants = 1;
    stats.prunedVtables = 1;
    stats.prunedExternDeclarations = 3;
    stats.estimatedLirNodesEliminated = 42;
    stats.executableSize = 91 * 1024;
    const std::string host = HostTargetTriple();
    const BuildReportInfo info{.packageName = "App",
                               .packageVersion = "0.1.0",
                               .artifactPath = "App/Bin/App.exe",
                               .packageRoot = "App",
                               .profile = Rux::BuildProfile::Release,
                               .targetTriple = host};
    const auto plain = FormatBuildStats(info, stats, false);
    const auto colored = FormatBuildStats(info, stats, true);

    CHECK_FALSE(plain.contains("\033["));
    CHECK(colored.contains("\033[32m\033[1mBuilt\033[0m App (Release,"));
    // Section titles are bold, matching the shared Reporting section renderer.
    CHECK(colored.contains("\033[1mTime\033[0m:\n"));
    CHECK(colored.contains("\033[1mPerformance\033[0m:\n"));

    CHECK(plain.contains("  Package:  App v0.1.0\n"));
    CHECK(plain.contains("  Output:   " + (std::filesystem::path("Bin") / "App.exe").string() + " (91 KB)\n"));
    // The profile and target belong to the status line, so the detail rows
    // below it must not repeat them.
    CHECK_FALSE(plain.contains("Profile:"));
    CHECK_FALSE(plain.contains("Target:"));

    // Four counts, then the total they sum to; the IR estimate sits outside
    // that sum and says so in its label rather than trailing the number.
    const auto squeezed = Squeeze(plain);
    CHECK(squeezed.contains("Function definitions: 2\n"));
    CHECK(squeezed.contains("Extern declarations: 3\n"));
    CHECK(squeezed.contains("Declarations pruned: 7\n"));
    CHECK(squeezed.contains("Estimated IR nodes: 42\n"));
    CHECK_FALSE(plain.contains("42 nodes"));
}

TEST_CASE("Detailed build report accounts for the whole build time") {
    BuildStats stats;
    stats.total = std::chrono::milliseconds(100);
    stats.lexing = std::chrono::milliseconds(10);
    stats.parsing = std::chrono::milliseconds(15);
    stats.semantic = std::chrono::milliseconds(20);
    stats.linking = std::chrono::milliseconds(5);
    const std::string host = HostTargetTriple();
    const BuildReportInfo info{.packageName = "App",
                               .packageVersion = "0.1.0",
                               .artifactPath = "Bin/App.exe",
                               .packageRoot = {},
                               .profile = Rux::BuildProfile::Debug,
                               .targetTriple = host};
    const auto report = FormatBuildStats(info, stats, false);

    // Phase timers cover 50 of the 100 ms; the rest is dependency resolution
    // and I/O that no phase measures, so `Other` names it rather than leaving
    // the column silently short of its own total.
    const auto squeezed = Squeeze(report);
    CHECK(squeezed.contains("Lexing: 10 ms 10.0%\n"));
    CHECK(squeezed.contains("Analyzing: 20 ms 20.0%\n"));
    CHECK(squeezed.contains("Other: 50 ms 50.0%\n"));
    CHECK(squeezed.contains("Total: 100 ms 100.0%\n"));
    // Phase names come from CompilePhaseName, so --stats and --verbose agree.
    CHECK(squeezed.contains("Lowering to HIR: 0 ms 0.0%\n"));
    CHECK(squeezed.contains("Emitting RCU objects: 0 ms 0.0%\n"));
}

TEST_CASE("Detailed build report names its target in display spelling") {
    BuildStats stats;
    const BuildReportInfo info{.packageName = "App",
                               .packageVersion = "0.1.0",
                               .artifactPath = "Bin/Release/Windows/AArch64/App.exe",
                               .packageRoot = {},
                               .profile = Rux::BuildProfile::Release,
                               .targetTriple = "windows-arm64"};

    // The alias normalizes, and the report says so in display spelling; the
    // canonical ID is reserved for text naming a value the reader can pass in.
    CHECK(FormatBuildStats(info, stats, false).contains("Built App (Release, Windows AArch64) in"));
    CHECK_FALSE(FormatBuildStats(info, stats, false).contains("windows-aarch64"));
}

TEST_CASE("Source statistics compare local and dependency input across one row") {
    BuildStats stats;
    stats.localFiles = 1;
    stats.dependencyFiles = 64;
    stats.localLines = 119;
    stats.dependencyLines = 1'559;
    stats.localTokens = 769;
    stats.dependencyTokens = 5'551;
    const std::string host = HostTargetTriple();
    const BuildReportInfo info{.packageName = "App",
                               .packageVersion = "0.1.0",
                               .artifactPath = "Bin/App.exe",
                               .packageRoot = {},
                               .profile = Rux::BuildProfile::Debug,
                               .targetTriple = host};
    const auto report = FormatBuildStats(info, stats, false);

    CHECK(report.contains("          Local  Dependency  Total\n"));
    CHECK(report.contains("  Files:      1          64     65\n"));
    CHECK(report.contains("  Lines:    119       1,559  1,678\n"));
    CHECK(report.contains("  Tokens:   769       5,551  6,320\n"));
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
    CHECK(report.contains("Built   Debug    FreeBSD x86-64"));
    CHECK(report.contains("Failed  Debug    Linux AArch64"));
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

TEST_CASE("Build matrix columns name each profile and target as they are displayed") {
    const auto windows = Rux::Target::TargetTriple::Parse("windows-x86_64");
    const auto freeBsd = Rux::Target::TargetTriple::Parse("freebsd-aarch64");
    REQUIRE(windows);
    REQUIRE(freeBsd);
    std::vector<BuildCellReport> cells;
    for (const auto profile : {Rux::BuildProfile::Debug, Rux::BuildProfile::Release}) {
        for (const auto &target : {*windows, *freeBsd}) {
            cells.push_back(BuildCellReport{.profile = profile,
                                            .target = target,
                                            .outputDirectory = {},
                                            .succeeded = true,
                                            .artifactPath = {},
                                            .stats = {},
                                            .elapsed = {}});
        }
    }

    const auto report = FormatBuildMatrixReport("App", cells, {}, false, false);

    // The two words of a display name are the two directory components the cell
    // writes to, so the column reads the same way as the path beside it.
    CHECK(report.contains("Debug    Windows x86-64"));
    CHECK(report.contains("Debug    FreeBSD AArch64"));
    CHECK(report.contains("Release  Windows x86-64"));
    CHECK(report.contains("Release  FreeBSD AArch64"));
    CHECK_FALSE(report.contains("windows-x86_64"));
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
