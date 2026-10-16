// `rux test` — build and run every test package under Tests/.

#include "Cli/BuildReport.h"
#include "Cli/Cli.h"
#include "Cli/DefineOption.h"
#include "Cli/JobsOption.h"
#include "Cli/ManifestInput.h"
#include "Cli/Reporter.h"
#include "Cli/Testing/TestPackages.h"
#include "Cli/Testing/TestScheduler.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "Package/Manifest.h"
#include "Parallel/WorkerPool.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Rux;
using namespace CliSupport;
using namespace Driver;
using namespace System;

int Cli::RunTest(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const ReporterOptions reporterOptions{.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose};
    const Reporter output(stdout, reporterOptions);
    const Reporter diagnostics(stderr, reporterOptions);
    bool isRelease = false;
    bool useBuildCache = true;
    // Test packages are independent, so by default one runs per logical core.
    std::size_t jobs = Parallel::DefaultJobs();
    std::string_view target;
    std::map<std::string, std::string> defines;
    std::vector<std::string_view> testArguments;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        // Everything after `--` goes to each test program, where a generated runner reads it as filters.
        if (arg == "--") {
            testArguments.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        }
        if (arg == "--jobs") {
            if (i + 1 == args.size() || !ParseJobCount(args[++i], jobs)) {
                diagnostics.Error("option '--jobs' requires a positive integer");
                return 1;
            }
            continue;
        }
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "--no-cache") {
            useBuildCache = false;
            continue;
        }
        if (arg == "--target" && i + 1 < args.size()) {
            target = args[++i];
            continue;
        }
        if (arg == "--define" && i + 1 < args.size()) {
            std::string error;
            if (!AddCompileTimeDefine(args[++i], defines, error)) {
                diagnostics.Error(error);
                return 1;
            }
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("test");
            return 0;
        }
        PrintUnknownOption(arg, "test");
        return 1;
    }
    const auto targetTriple =
        target.empty() ? std::optional{Target::TargetTriple::Host()} : Target::TargetTriple::Parse(target);
    if (!targetTriple) {
        diagnostics.Error(std::format("target '{}' is not supported", target));
        diagnostics.Note("supported targets are " + SupportedTargetTriples());
        diagnostics.Help("try 'rux test --target linux-x86_64'");
        return 1;
    }
    const std::string targetName(targetTriple->CanonicalName());
    const BuildProfile profile = isRelease ? BuildProfile::Release : BuildProfile::Debug;
    // A test passes only by executing on this OS and either the compiler
    // process architecture or the native architecture underneath translation.
    if (!HostCanExecuteTarget(*targetTriple)) {
        diagnostics.Error(std::format("target '{}' cannot be executed on host '{}'", targetName, HostTargetTriple()));
        diagnostics.Note(
            std::format("this host can build and check target '{}' but cannot run its test programs", targetName));
        diagnostics.Help(std::format("run 'rux build --target {}' or 'rux check --target {}' on this host, then "
                                     "transfer the source tree and run 'rux test --target {}' on a native '{}' host",
                                     targetName, targetName, targetName, targetName));
        return 1;
    }
    const auto commandStart = std::chrono::steady_clock::now();
    std::optional<std::filesystem::path> manifestPath;
    if (!opts.manifest.empty()) {
        manifestPath = opts.manifest;
        if (!std::filesystem::exists(*manifestPath)) {
            diagnostics.Error(std::format("specified manifest '{}' was not found", manifestPath->string()));
            return 1;
        }
    }
    else {
        manifestPath = Manifest::Find();
    }

    // A directory to search for test packages, plus the prefix its tests are
    // labeled with. A package has a single root (its own Tests/). A workspace
    // has the root Tests/ *and* each member package's Tests/, so tests may sit
    // either centrally or next to the code they cover.

    std::filesystem::path projectRoot;
    std::vector<TestRoot> testRoots;
    // Packages whose own sources hold '#Test()' functions, run beside the packages under Tests/.
    std::vector<TestPackage> sourceTestPackages;
    std::vector<std::filesystem::path> localPackageRoots;
    std::set<std::string> localNamespaces;
    if (manifestPath) {
        auto manifestResult = Manifest::Load(*manifestPath);
        if (!manifestResult.Ok()) {
            for (const auto &diagnostic : manifestResult.diagnostics) {
                diagnostics.Write(diagnostic.Render(), MessageVisibility::Always);
            }
            return 1;
        }
        auto manifest = std::move(manifestResult.manifest);
        projectRoot = manifestPath->parent_path();
        if (manifest->IsWorkspace()) {
            // Workspace manifest: gather the root Tests/ (for centrally placed
            // tests) plus each declared member package's own Tests/, so a
            // member's tests read as "Member/...".
            output.Progress("Testing", std::format("workspace {}", FormatBuildContext(profile, *targetTriple)));
            std::error_code ec;
            if (std::filesystem::exists(projectRoot / "Tests", ec)) {
                testRoots.push_back({projectRoot / "Tests", {}});
            }
            for (const auto &member : manifest->workspace.packages) {
                const auto memberDir = (projectRoot / member).lexically_normal();
                const auto memberManifestPath = memberDir / "Rux.toml";
                if (!std::filesystem::exists(memberManifestPath, ec)) {
                    diagnostics.Warning(
                        std::format("workspace member '{}' has no 'Rux.toml' and will be skipped", member));
                    continue;
                }
                auto memberResult = Manifest::Load(memberManifestPath);
                if (!memberResult.Ok() || memberResult.manifest->package.name.Empty()) {
                    for (const auto &diagnostic : memberResult.diagnostics) {
                        diagnostics.Write(diagnostic.Render(), MessageVisibility::Always);
                    }
                    diagnostics.Error(std::format("workspace member '{}' is not a package", member));
                    return 1;
                }
                localPackageRoots.push_back(memberDir);
                if (memberResult.manifest->package.ns)
                    localNamespaces.insert(memberResult.manifest->package.ns->Normalized());
                if (SourcesDeclareTests(memberDir)) {
                    sourceTestPackages.push_back(
                        {memberDir, std::filesystem::path(member).generic_string(), *memberResult.manifest});
                }
                if (auto memberTests = memberDir / "Tests"; std::filesystem::exists(memberTests, ec)) {
                    testRoots.push_back({std::move(memberTests), std::filesystem::path(member).generic_string()});
                }
            }
        }
        else {
            output.Progress("Testing",
                            std::format("{} v{} {}", manifest->package.name.Text(), manifest->package.version.Text(),
                                        FormatBuildContext(profile, *targetTriple)));
            testRoots.push_back({projectRoot / "Tests", {}});
            if (SourcesDeclareTests(projectRoot)) {
                sourceTestPackages.push_back({projectRoot, manifest->package.name.Text(), *manifest});
            }
        }
    }
    else {
        projectRoot = std::filesystem::current_path();
        std::error_code ec;
        if (std::filesystem::exists(projectRoot / "Tests", ec)) {
            testRoots.push_back({projectRoot / "Tests", {}});
        }
        // Members are the immediate subdirectories holding a Rux.toml. Tests
        // found under a member are labeled with the member's name, so
        // Text/Tests/Compare reads as "Text/Compare" — the same label a
        // centrally placed Tests/Text/Compare would get.
        for (const auto &entry : std::filesystem::directory_iterator(projectRoot, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            if (!std::filesystem::exists(entry.path() / "Rux.toml")) {
                continue;
            }
            if (auto memberTests = entry.path() / "Tests"; std::filesystem::exists(memberTests, ec)) {
                testRoots.push_back({std::move(memberTests), entry.path().filename().generic_string()});
            }
        }
        if (testRoots.empty()) {
            static_cast<void>(RequireManifest()); // Prints standard "Rux.toml not found" error
            return 1;
        }
        output.Progress("Testing", std::format("workspace {}", FormatBuildContext(profile, *targetTriple)));
    }

    auto discovered = DiscoverTestPackages(testRoots);
    for (const auto &diagnostic : discovered.diagnostics)
        diagnostics.Write(diagnostic.Render(), MessageVisibility::Always);
    if (!discovered.diagnostics.empty()) {
        return 1;
    }
    discovered.packages.insert(discovered.packages.begin(), std::make_move_iterator(sourceTestPackages.begin()),
                               std::make_move_iterator(sourceTestPackages.end()));
    const auto &testPackages = discovered.packages;
    if (!discovered.anyRootExists && testPackages.empty()) {
        output.Success("Passed",
                       std::format("0 test packages in {}", Reporting::FormatDuration(ElapsedMs(commandStart))));
        output.Detail("No test directory found at 'Tests/'");
        return 0;
    }
    if (testPackages.empty()) {
        output.Success("Passed",
                       std::format("0 test packages in {}", Reporting::FormatDuration(ElapsedMs(commandStart))));
        output.Detail("No executable test packages found under 'Tests/'");
        return 0;
    }

    // What runs is a package each, however many tests each package holds.
    output.Progress("Running", Reporting::FormatCount(testPackages.size(), "test package"));

    auto ReportCapturedOutput = [&](const std::string_view captured) {
        if (captured.empty()) {
            return;
        }
        std::string rendered = "  Output:\n" + Reporting::Indent(captured, 2);
        if (!rendered.ends_with('\n')) {
            rendered += '\n';
        }
        output.Write(rendered, MessageVisibility::Always);
    };

    // Run every discovered test package and report each failure next to the
    // captured diagnostics or program output that explains it.
    int passed = 0;
    int failed = 0;
    int unmatched = 0;
    // Runners that had a test their filters matched, which is what a run with filters is for.
    int matchedRunners = 0;
    const auto suiteStart = std::chrono::steady_clock::now();
    std::vector<TestTask> tasks;
    for (const auto &package : testPackages) {
        const auto artifact =
            ResolveTestOutputDir(package.dir, package.manifest, *targetTriple) /
            OutputFileName(package.manifest.package.name.Text(), ArtifactKind::Executable, targetTriple->Os());
        tasks.push_back({{artifact, package.dir / "Temp"}});
    }
    CompileOptions compileOptions;
    compileOptions.target = *targetTriple;
    compileOptions.profile = profile;
    compileOptions.defines = defines;
    compileOptions.localPackageRoots = localPackageRoots;
    compileOptions.localNamespaces = localNamespaces;
    compileOptions.useBuildCache = useBuildCache;
    // Packages built side by side share the cores rather than each starting a thread per core.
    compileOptions.jobs = std::max<std::size_t>(1, Parallel::DefaultJobs() / jobs);
    std::vector<TestOutcome> outcomes(testPackages.size());
    RunTestTasks(
        tasks, jobs,
        [&](const std::size_t index) {
            outcomes[index] =
                RunTestPackage(testPackages[index], compileOptions, diagnostics.Style().enabled, testArguments);
        },
        [&](const std::size_t index) {
            const std::string &label = testPackages[index].label;
            const TestOutcome &outcome = outcomes[index];
            if (!outcome.executable.empty())
                output.Verbose(std::format("Running '{}'", outcome.executable.string()));
            const auto detail = std::format("{} in {}", label, Reporting::FormatDuration(outcome.duration));
            if (outcome.runner && outcome.status != TestStatus::NoMatch && outcome.status != TestStatus::BuildError) {
                ++matchedRunners;
            }
            if (outcome.status == TestStatus::NoMatch) {
                ++unmatched;
                output.Success("Skipped", std::format("{} (no test matches)", detail));
            }
            else if (outcome.status == TestStatus::Passed) {
                ++passed;
                output.Success("Passed", detail);
                // Arguments for the runner ask it for something, such as `--list` or a filter, so what it said is
                // the answer and is shown even when the package passed.
                if (!testArguments.empty()) {
                    ReportCapturedOutput(outcome.output);
                }
            }
            else {
                ++failed;
                output.Failure("Failed", detail);
                output.Flush();
                switch (outcome.status) {
                case TestStatus::Failed:
                    diagnostics.Note(std::format("test '{}' exited with code {}", label, outcome.exitCode));
                    break;
                case TestStatus::BuildError:
                    diagnostics.Note("the test package did not compile");
                    break;
                case TestStatus::LaunchError:
                    diagnostics.Note("the test executable could not be launched");
                    break;
                case TestStatus::Passed:
                case TestStatus::NoMatch:
                    break;
                }
                diagnostics.Write(outcome.diagnostics, MessageVisibility::Always);
                diagnostics.Flush();
                ReportCapturedOutput(outcome.output);
            }
            output.Flush();
        });
    const auto elapsed = ElapsedMs(suiteStart);
    const int total = passed + failed + unmatched;
    // Filters that no package had a test for ran nothing, which a misspelt name must not pass for.
    const bool nothingMatched = unmatched > 0 && matchedRunners == 0;
    if (nothingMatched) {
        diagnostics.Error("no test matches the filter in any package");
        diagnostics.Help("run 'rux test -- --list' to see the test names");
    }
    if (!opts.quiet) {
        std::string totals =
            std::format("{} in {} ({} passed, {} failed", Reporting::FormatCount(total, "test package"),
                        Reporting::FormatDuration(elapsed), passed, failed);
        totals += unmatched > 0 ? std::format(", {} without a matching test)", unmatched) : ")";
        if (failed == 0 && !nothingMatched) {
            output.Success("Passed", totals);
        }
        else {
            output.Failure("Failed", totals);
        }
    }
    return failed == 0 && !nothingMatched ? 0 : 1;
}
