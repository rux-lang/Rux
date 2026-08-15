// `rux lint`: run source-level lint checks for the current package.

#include "Cli/Cli.h"
#include "Cli/Reporter.h"
#include "Cli/TerminalStyle.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Linter/Linter.h"
#include "Reporting/Reporting.h"
#include "Source/SourceLoader.h"

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

using namespace Rux;
using namespace CliSupport;
using namespace Driver;

int Cli::RunLint(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    for (const auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("lint");
            return 0;
        }
        PrintUnknownOption(arg, "lint");
        return 1;
    }

    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto rootManifest = LoadManifest(*manifestPath);
    if (!rootManifest) {
        return 1;
    }

    struct LintOutcome {
        std::size_t files = 0;
        std::size_t warnings = 0;
        std::size_t errors = 0;

        [[nodiscard]] bool Failed() const noexcept {
            return errors > 0;
        }
    };

    auto LintPackage = [&](const std::filesystem::path &packageManifestPath, const Manifest &packageManifest,
                           const bool manifestValid = true) {
        LintOutcome outcome;
        if (!manifestValid) {
            outcome.errors = 1;
            return outcome;
        }
        if (packageManifest.IsWorkspace() || packageManifest.package.name.Empty()) {
            diagnostics.Error(
                std::format("workspace member '{}' is not a package", packageManifestPath.parent_path().string()));
            outcome.errors = 1;
            return outcome;
        }
        const auto sources = SourceLoader::Load(packageManifestPath.parent_path());
        const SourceLineLookup sourceLineLookup = [&](const std::string_view sourceName,
                                                      const std::size_t lineNumber) -> std::optional<std::string_view> {
            const auto source = std::ranges::find_if(
                sources.files, [&](const SourceFile &file) { return file.path.string() == sourceName; });
            if (source == sources.files.end()) {
                return std::nullopt;
            }
            return FindSourceLine(source->source, lineNumber);
        };
        const bool diagnosticColor = ColorEnabled(opts.color, OutputStream::Stderr);
        outcome.files = sources.files.size();
        for (const auto &diagnostic : sources.diagnostics) {
            std::print(stderr, "{}", RenderDiagnostic(diagnostic, diagnosticColor, sourceLineLookup));
            if (diagnostic.IsError()) {
                ++outcome.errors;
            }
            else {
                ++outcome.warnings;
            }
        }
        for (const auto &file : sources.files) {
            output.Verbose(std::format("File: {}", file.path.string()));
            auto result = Linting::Lint(file.source, file.path.string());
            for (const auto &diagnostic : result.diagnostics) {
                std::print(stderr, "{}", RenderDiagnostic(diagnostic, diagnosticColor, sourceLineLookup));
                if (diagnostic.IsError()) {
                    ++outcome.errors;
                }
                else {
                    ++outcome.warnings;
                }
            }
        }
        return outcome;
    };

    struct LintJob {
        std::filesystem::path manifestPath;
        std::string label;
        std::optional<Manifest> manifest;
    };

    std::vector<LintJob> jobs;
    if (rootManifest->IsWorkspace()) {
        output.Progress("Linting", "workspace");
        const auto workspaceRoot = manifestPath->parent_path();
        for (const auto &member : rootManifest->workspace.packages) {
            const auto memberManifestPath = (workspaceRoot / member / "Rux.toml").lexically_normal();
            const auto label = std::filesystem::path(member).lexically_normal().generic_string();
            std::error_code ec;
            if (!std::filesystem::exists(memberManifestPath, ec)) {
                diagnostics.Error(std::format("workspace member '{}' has no 'Rux.toml'", member));
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            auto memberManifest = LoadManifest(memberManifestPath);
            if (!memberManifest) {
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            if (IsPlatformPackageName(memberManifest->package.name.Text()) &&
                !PlatformPackageMatchesTarget(memberManifest->package.name.Text(), Target::TargetTriple::Host())) {
                continue;
            }
            jobs.push_back({memberManifestPath, label, std::move(*memberManifest)});
        }
    }
    else {
        output.Progress("Linting",
                        std::format("{} v{}", rootManifest->package.name.Text(), rootManifest->package.version.Text()));
        jobs.push_back({*manifestPath, rootManifest->package.name.Text(), std::move(*rootManifest)});
    }

    output.Detail(std::format("Packages: {}", jobs.size()));

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t files = 0;
    std::size_t warnings = 0;
    std::size_t errors = 0;
    const auto suiteStart = std::chrono::steady_clock::now();
    for (const auto &job : jobs) {
        const auto start = std::chrono::steady_clock::now();
        const Manifest emptyManifest;
        const auto outcome =
            LintPackage(job.manifestPath, job.manifest ? *job.manifest : emptyManifest, job.manifest.has_value());
        files += outcome.files;
        warnings += outcome.warnings;
        errors += outcome.errors;
        const auto detail = std::format("{} in {} ({}, {}, {})", job.label, Reporting::FormatDuration(ElapsedMs(start)),
                                        Reporting::FormatCount(outcome.files, "file"),
                                        Reporting::FormatCount(outcome.warnings, "warning"),
                                        Reporting::FormatCount(outcome.errors, "error"));

        if (outcome.Failed()) {
            ++failed;
            output.Failure("Failed", detail);
        }
        else {
            ++passed;
            output.Success("Linted", detail);
        }
    }

    const auto totals = std::format(
        "{} in {} ({} passed, {} failed, {}, {}, {})", Reporting::FormatCount(jobs.size(), "package"),
        Reporting::FormatDuration(ElapsedMs(suiteStart)), passed, failed, Reporting::FormatCount(files, "file"),
        Reporting::FormatCount(warnings, "warning"), Reporting::FormatCount(errors, "error"));
    if (failed > 0) {
        if (!opts.quiet) {
            output.Failure("Failed", totals);
        }
    }
    else {
        output.Success("Linted", totals);
    }
    return failed == 0 ? 0 : 1;
}
