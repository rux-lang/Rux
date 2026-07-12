// `rux test` — build and run every test package under Tests/.

#include "Cli/Cli.h"
#include "Cli/TerminalStyle.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "Package/Manifest.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "System/Process.h"

using namespace Rux;
using namespace CliSupport;
using namespace Driver;
using namespace System;

int Cli::RunTest(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool isRelease = false;
    for (auto &arg : args) {
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("test");
            return 0;
        }
        PrintUnknownOption(arg, "test");
        return 1;
    }
    std::optional<std::filesystem::path> manifestPath;
    if (!opts.manifest.empty()) {
        manifestPath = opts.manifest;
        if (!std::filesystem::exists(*manifestPath)) {
            std::print(stderr, "error: specified manifest '{}' not found\n", manifestPath->string());
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
    struct TestRoot {
        std::filesystem::path dir;
        std::string labelPrefix;
    };

    std::filesystem::path projectRoot;
    std::vector<TestRoot> testRoots;
    if (manifestPath) {
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            return 1;
        }
        if (!opts.quiet) {
            std::print("Testing {} v{}\n", manifest->package.name, manifest->package.version);
        }
        projectRoot = manifestPath->parent_path();
        testRoots.push_back({projectRoot / "Tests", {}});
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
        if (!opts.quiet) {
            std::print("Testing workspace\n");
        }
    }
    const std::string_view profileName = isRelease ? "Release" : "Debug";

    // Collect test package directories: any directory under a test root that
    // contains a Rux.toml with Type = "bin". A directory without a manifest is
    // a group (e.g. Tests/Integration/) and is searched recursively, a few levels
    // deep so build-output trees don't get walked.
    struct TestPackage {
        std::filesystem::path dir;
        std::string label;
    };

    std::vector<TestPackage> testPackages;
    bool anyRootExists = false;
    {
        std::error_code ec;
        constexpr int maxGroupDepth = 3;
        for (const auto &root : testRoots) {
            if (!std::filesystem::exists(root.dir, ec)) {
                continue;
            }
            anyRootExists = true;
            std::vector<std::pair<std::filesystem::path, int>> pendingDirs;
            pendingDirs.emplace_back(root.dir, 0);
            while (!pendingDirs.empty()) {
                const auto [dir, depth] = std::move(pendingDirs.back());
                pendingDirs.pop_back();
                for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (!entry.is_directory()) {
                        continue;
                    }
                    const auto toml = entry.path() / "Rux.toml";
                    if (!std::filesystem::exists(toml)) {
                        if (depth + 1 < maxGroupDepth) {
                            pendingDirs.emplace_back(entry.path(), depth + 1);
                        }
                        continue;
                    }
                    auto pkgManifest = Manifest::Load(toml);
                    if (!pkgManifest) {
                        continue;
                    }
                    // Only run binary packages (not DLLs / shared libraries).
                    const auto &type = pkgManifest->package.type;
                    if (type != "bin" && type != "Bin") {
                        continue;
                    }
                    auto label = entry.path().lexically_relative(root.dir).generic_string();
                    if (!root.labelPrefix.empty()) {
                        label = root.labelPrefix + "/" + label;
                    }
                    testPackages.push_back({entry.path(), std::move(label)});
                }
            }
        }
        std::sort(testPackages.begin(), testPackages.end(),
                  [](const TestPackage &a, const TestPackage &b) { return a.label < b.label; });
    }
    if (!anyRootExists) {
        if (!opts.quiet) {
            std::print("  No Tests/ directory found — nothing to run.\n");
        }
        return 0;
    }
    if (testPackages.empty()) {
        if (!opts.quiet) {
            std::print("  No test packages found in Tests/.\n");
        }
        return 0;
    }

    // Outcome of running a single test package. On failure the combined
    // stdout/stderr of the test binary is captured so it can be replayed in a
    // dedicated `failures:` section after the whole suite has run.
    enum class TestStatus {
        Passed,
        Failed,
        BuildError,
        LaunchError
    };

    struct TestOutcome {
        TestStatus status = TestStatus::Passed;
        int exitCode = 0;
        std::string output;
        std::chrono::milliseconds duration{0};
    };

    // Helper: build a test package quietly, then execute the resulting binary
    // with its output captured.
    auto runOne = [&](const std::filesystem::path &pkgDir) -> TestOutcome {
        TestOutcome outcome;

        // Load the package manifest to derive the executable name and output path.
        auto pkgManifest = Manifest::Load(pkgDir / "Rux.toml");
        if (!pkgManifest) {
            std::print(stderr, "error: failed to parse '{}'\n", (pkgDir / "Rux.toml").string());
            outcome.status = TestStatus::BuildError;
            return outcome;
        }

        // Build the package quietly (suppress per-file build output for tests).
        CompileOptions copts;
        copts.manifestPath = pkgDir / "Rux.toml";
        copts.manifest = std::move(*pkgManifest);
        copts.targetName = HostTargetTriple();
        copts.profileName = std::string(profileName);
        copts.quiet = true;
        CompilerDriver driver(std::move(copts));
        const CompileResult result = driver.Compile();
        if (!result.ok) {
            outcome.status = TestStatus::BuildError;
            return outcome;
        }
        const auto exePath = result.executablePath;

        if (!std::filesystem::exists(exePath)) {
            std::print(stderr, "error: built executable not found at '{}'\n", exePath.string());
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }

        if (opts.verbose)
            std::print("     Running `{}`\n", exePath.string());

        const auto start = std::chrono::steady_clock::now();

        // Execute the test binary, capturing its combined stdout/stderr.
        auto run = RunCaptured(exePath);
        if (!run) {
            std::print(stderr, "error: failed to launch '{}'\n", exePath.string());
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        outcome.exitCode = run->exitCode;
        outcome.output = std::move(run->output);

        outcome.duration = ElapsedMs(start);
        outcome.status = outcome.exitCode == 0 ? TestStatus::Passed : TestStatus::Failed;
        return outcome;
    };
    const AnsiStyle style{ColorEnabled(opts.color)};
    if (!opts.quiet) {
        std::print("Running {} {}\n\n", testPackages.size(), testPackages.size() == 1 ? "test" : "tests");
    }
    // Width of the test-name column, so the trailing timings line up.
    std::size_t nameWidth = 0;
    for (const auto &pkg : testPackages) {
        nameWidth = std::max(nameWidth, pkg.label.size());
    }

    // Run every discovered test package and tally results, buffering the output
    // of any failures for replay after the run.
    struct Failure {
        std::string label;
        TestStatus status;
        int exitCode;
        std::string output;
    };

    std::vector<Failure> failures;
    int passed = 0;
    int failed = 0;
    const auto suiteStart = std::chrono::steady_clock::now();
    for (const auto &pkg : testPackages) {
        const std::string &label = pkg.label;
        std::string paddedLabel = label;
        paddedLabel.resize(nameWidth, ' ');
        TestOutcome outcome = runOne(pkg.dir);
        // Trailing detail: a timing for tests that ran, otherwise the failure kind.
        std::string detail;
        switch (outcome.status) {
        case TestStatus::BuildError:
            detail = "build error";
            break;
        case TestStatus::LaunchError:
            detail = "launch error";
            break;
        default:
            detail = std::format("{} ms", outcome.duration.count());
            break;
        }
        if (outcome.status == TestStatus::Passed) {
            ++passed;
            if (!opts.quiet) {
                std::print("{}[PASSED]{} {} ({})\n", style.Green(), style.Reset(), paddedLabel, detail);
            }
        }
        else {
            ++failed;
            if (!opts.quiet) {
                std::print("{}[FAILED]{} {} ({})\n", style.Red(), style.Reset(), paddedLabel, detail);
            }
            failures.push_back({label, outcome.status, outcome.exitCode, std::move(outcome.output)});
        }
    }
    const double elapsed = ElapsedSeconds(suiteStart);
    // Detail each failure, replaying any captured output.
    if (!failures.empty() && !opts.quiet) {
        std::print("\nFailures:\n\n");
        int index = 0;
        for (const auto &f : failures) {
            ++index;
            std::print("{}) {}\n", index, f.label);
            switch (f.status) {
            case TestStatus::Failed:
                std::print("   Exit code: {}\n", f.exitCode);
                break;
            case TestStatus::BuildError:
                std::print("   Build error\n");
                break;
            default:
                std::print("   Launch error\n");
                break;
            }
            if (!f.output.empty()) {
                std::print("   Output:\n");
                std::string_view out{f.output};
                std::size_t pos = 0;
                while (pos < out.size()) {
                    const auto nl = out.find('\n', pos);
                    const auto line = out.substr(pos, nl == std::string_view::npos ? out.size() - pos : nl - pos);
                    std::print("     {}\n", line);
                    if (nl == std::string_view::npos) {
                        break;
                    }
                    pos = nl + 1;
                }
            }
            std::print("\n");
        }
    }
    // Summary block.
    const int total = passed + failed;
    if (!opts.quiet || failed > 0) {
        std::print("Test Result:\n");
        std::print("  Passed: {}{}{}\n", style.Green(), passed, style.Reset());
        if (failed > 0) {
            std::print("  Failed: {}{}{}\n", style.Red(), failed, style.Reset());
        }
        else {
            std::print("  Failed: {}\n", failed);
        }
        std::print("  Total : {}\n", total);
        std::print("  Time  : {:.2f}s\n", elapsed);
    }
    return failed == 0 ? 0 : 1;
}
