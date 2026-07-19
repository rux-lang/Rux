// `rux lint`: run source-level lint checks for the current package.

#include "Cli/Cli.h"
#include "Cli/TerminalStyle.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Linter/Linter.h"
#include "Source/SourceLoader.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
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
        if (packageManifest.IsWorkspace() || packageManifest.package.name.empty()) {
            std::print(stderr, "error: workspace member '{}' is not a package\n",
                       packageManifestPath.parent_path().string());
            outcome.errors = 1;
            return outcome;
        }
        const auto sources = SourceLoader::Load(packageManifestPath.parent_path());
        if (!sources) {
            outcome.errors = 1;
            return outcome;
        }

        outcome.files = sources->files.size();
        for (const auto &error : sources->errors) {
            std::print(stderr, "{}", error);
            ++outcome.errors;
        }
        for (const auto &file : sources->files) {
            if (opts.verbose) {
                std::println("  Linting {}", file.path.string());
            }
            auto result = Linting::Lint(file.source, file.path.string());
            for (const auto &diagnostic : result.diagnostics) {
                PrintDiagnostic(diagnostic);
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
        if (!opts.quiet) {
            std::println("Linting workspace");
        }
        const auto workspaceRoot = manifestPath->parent_path();
        const auto targetName = HostTargetTriple();
        for (const auto &member : rootManifest->workspace.packages) {
            const auto memberManifestPath = (workspaceRoot / member / "Rux.toml").lexically_normal();
            const auto label = std::filesystem::path(member).lexically_normal().generic_string();
            std::error_code ec;
            if (!std::filesystem::exists(memberManifestPath, ec)) {
                std::print(stderr, "error: workspace member '{}' has no Rux.toml\n", member);
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            auto memberManifest = LoadManifest(memberManifestPath);
            if (!memberManifest) {
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            if (IsPlatformPackageName(memberManifest->package.name) &&
                !PlatformPackageMatchesTarget(memberManifest->package.name, targetName)) {
                continue;
            }
            jobs.push_back({memberManifestPath, label, std::move(*memberManifest)});
        }
    }
    else {
        if (!opts.quiet) {
            std::println("Linting {} v{}", rootManifest->package.name, rootManifest->package.version);
        }
        jobs.push_back({*manifestPath, rootManifest->package.name, std::move(*rootManifest)});
    }

    const AnsiStyle style{ColorEnabled(opts.color)};
    if (!opts.quiet) {
        std::println("Linting {} {}\n", jobs.size(), jobs.size() == 1 ? "package" : "packages");
    }

    std::size_t labelWidth = 0;
    for (const auto &job : jobs) {
        labelWidth = std::max(labelWidth, job.label.size());
    }

    std::size_t passed = 0;
    std::size_t warned = 0;
    std::size_t failed = 0;
    for (const auto &job : jobs) {
        const Manifest emptyManifest;
        const auto outcome =
            LintPackage(job.manifestPath, job.manifest ? *job.manifest : emptyManifest, job.manifest.has_value());
        std::string paddedLabel = job.label;
        paddedLabel.resize(labelWidth, ' ');

        std::string detail = std::format("{} {}", outcome.files, outcome.files == 1 ? "file" : "files");
        if (outcome.warnings > 0) {
            detail += std::format(", {} {}", outcome.warnings, outcome.warnings == 1 ? "warning" : "warnings");
        }
        if (outcome.errors > 0) {
            detail += std::format(", {} {}", outcome.errors, outcome.errors == 1 ? "error" : "errors");
        }

        if (outcome.Failed()) {
            ++failed;
            if (!opts.quiet) {
                std::println("{}[FAILED]{} {} ({})", style.Red(), style.Reset(), paddedLabel, detail);
            }
        }
        else if (outcome.warnings > 0) {
            ++warned;
            if (!opts.quiet) {
                std::println("{}[WARNING]{} {} ({})", style.Yellow(), style.Reset(), paddedLabel, detail);
            }
        }
        else {
            ++passed;
            if (!opts.quiet) {
                std::println("{}[PASSED]{} {} ({})", style.Green(), style.Reset(), paddedLabel, detail);
            }
        }
    }

    if (!opts.quiet || failed > 0) {
        std::println("\nLint Result:");
        std::println("  Passed  : {}{}{}", style.Green(), passed, style.Reset());
        if (warned > 0) {
            std::println("  Warnings: {}{}{}", style.Yellow(), warned, style.Reset());
        }
        else {
            std::println("  Warnings: {}", warned);
        }
        if (failed > 0) {
            std::println("  Failed  : {}{}{}", style.Red(), failed, style.Reset());
        }
        else {
            std::println("  Failed  : {}", failed);
        }
        std::println("  Total   : {}", passed + warned + failed);
    }
    return failed == 0 ? 0 : 1;
}
