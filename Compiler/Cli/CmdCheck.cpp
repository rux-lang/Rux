// `rux check` — frontend-only compile with text or JSON diagnostics.

#include "Cli/Cli.h"
#include "Cli/DefineOption.h"
#include "Cli/TerminalStyle.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
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

int Cli::RunCheck(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool jsonOutput = false;
    std::string_view target;
    std::map<std::string, std::string> defines;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-q" || arg == "--quiet") {
            continue;
        }
        if (arg == "-v" || arg == "--verbose") {
            continue;
        }
        if (arg == "--json") {
            jsonOutput = true;
            continue;
        }
        if (arg == "--target" && i + 1 < args.size()) {
            target = args[++i];
            continue;
        }
        if (arg == "--define" && i + 1 < args.size()) {
            std::string error;
            if (!AddCompileTimeDefine(args[++i], defines, error)) {
                std::print(stderr, "error: {}\n", error);
                return 1;
            }
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("check");
            return 0;
        }
        PrintUnknownOption(arg, "check");
        return 1;
    }
    std::vector<Diagnostic> jsonDiags;
    bool hadErrors = false;
    auto EmitDiag = [&](Diagnostic diag) {
        if (jsonOutput) {
            jsonDiags.push_back(std::move(diag));
        }
        else {
            PrintDiagnostic(diag);
        }
    };
    auto EmitFatal = [&](std::string message) {
        EmitDiag(ErrorDiagnostic(std::move(message)));
        hadErrors = true;
    };
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        if (jsonOutput) {
            EmitFatal("could not find 'Rux.toml' in current directory or any "
                      "parent directory");
        }
        return 1;
    }
    auto manifest = Manifest::Load(*manifestPath);
    if (!manifest) {
        EmitFatal("failed to parse '" + manifestPath->string() + "'");
        return 1;
    }
    std::string targetName = target.empty() ? HostTargetTriple() : CanonicalTargetTriple(target);
    if (!IsSupportedTargetTriple(targetName)) {
        if (jsonOutput) {
            EmitFatal("unsupported target '" + targetName + "'");
        }
        else {
            std::print(stderr, "error: unsupported target '{}'; supported targets are {}\n", targetName,
                       SupportedTargetTriples());
        }
        return 1;
    }
    const std::string hostTarget = HostTargetTriple();
    if (hostTarget != "unknown" && targetName != hostTarget) {
        if (jsonOutput) {
            EmitFatal("cross-target build from '" + hostTarget + "' to '" + targetName + "' is not supported yet");
        }
        else {
            std::print(stderr,
                       "error: cross-target build from '{}' to '{}' is not "
                       "supported yet\n",
                       hostTarget, targetName);
        }
        return 1;
    }
    std::map<std::string, std::filesystem::path> localPackageRoots;
    bool localDependenciesOnly = false;
    auto CheckPackage = [&](const std::filesystem::path &packageManifestPath, Manifest packageManifest) {
        if (packageManifest.IsWorkspace() || packageManifest.package.name.empty()) {
            EmitFatal("workspace member '" + packageManifestPath.parent_path().string() + "' is not a package");
            return false;
        }
        CompileOptions copts;
        copts.manifestPath = packageManifestPath;
        copts.manifest = std::move(packageManifest);
        copts.targetName = targetName;
        copts.profileName = "Debug";
        copts.defines = defines;
        copts.localPackageRoots = localPackageRoots;
        copts.localDependenciesOnly = localDependenciesOnly;
        copts.quiet = opts.quiet;
        copts.verbose = opts.verbose && !jsonOutput;
        copts.checkOnly = true;
        copts.emitDiagnostic = EmitDiag;
        copts.emitError = [&](std::string_view line) {
            if (jsonOutput) {
                EmitDiag(ErrorDiagnostic(std::string(line)));
            }
            else {
                std::print(stderr, "{}", line);
            }
        };
        CompilerDriver driver(std::move(copts));
        const bool passed = driver.Compile().ok;
        if (!passed) {
            hadErrors = true;
        }
        return passed;
    };

    struct CheckJob {
        std::filesystem::path manifestPath;
        std::string label;
        std::optional<Manifest> manifest;
    };

    std::vector<CheckJob> jobs;

    if (manifest->IsWorkspace()) {
        if (!opts.quiet && !jsonOutput) {
            std::print("Checking workspace\n");
        }
        const auto workspaceRoot = manifestPath->parent_path();
        for (const auto &member : manifest->workspace.packages) {
            const auto memberManifestPath = (workspaceRoot / member / "Rux.toml").lexically_normal();
            const auto label = std::filesystem::path(member).lexically_normal().generic_string();
            std::error_code ec;
            if (!std::filesystem::exists(memberManifestPath, ec)) {
                EmitFatal("workspace member '" + member + "' has no Rux.toml");
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            auto memberManifest = Manifest::Load(memberManifestPath);
            if (!memberManifest) {
                EmitFatal("failed to parse '" + memberManifestPath.string() + "'");
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            if (memberManifest->IsWorkspace() || memberManifest->package.name.empty()) {
                EmitFatal("workspace member '" + member + "' is not a package");
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            const auto [existing, inserted] =
                localPackageRoots.emplace(memberManifest->package.name, memberManifestPath.parent_path());
            if (!inserted && existing->second != memberManifestPath.parent_path()) {
                EmitFatal("duplicate workspace package name '" + memberManifest->package.name + "'");
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            if (IsPlatformPackageName(memberManifest->package.name) &&
                !PlatformPackageMatchesTarget(memberManifest->package.name, targetName)) {
                continue;
            }
            jobs.push_back({memberManifestPath, label, std::move(*memberManifest)});
        }
        localDependenciesOnly = true;
    }
    else {
        if (!opts.quiet && !jsonOutput) {
            std::println("Checking {} v{}", manifest->package.name, manifest->package.version);
        }
        jobs.push_back({*manifestPath, manifest->package.name, std::move(*manifest)});
    }

    const AnsiStyle style{ColorEnabled(opts.color)};
    if (!opts.quiet && !jsonOutput) {
        std::println("Checking {} {}\n", jobs.size(), jobs.size() == 1 ? "package" : "packages");
    }

    std::size_t labelWidth = 0;
    for (const auto &job : jobs) {
        labelWidth = std::max(labelWidth, job.label.size());
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    const auto suiteStart = std::chrono::steady_clock::now();
    for (auto &job : jobs) {
        const auto start = std::chrono::steady_clock::now();
        const bool packagePassed = job.manifest && CheckPackage(job.manifestPath, std::move(*job.manifest));
        const auto duration = ElapsedMs(start);

        std::string paddedLabel = job.label;
        paddedLabel.resize(labelWidth, ' ');
        if (packagePassed) {
            ++passed;
            if (!opts.quiet && !jsonOutput) {
                std::println("{}[PASSED]{} {} ({} ms)", style.Green(), style.Reset(), paddedLabel, duration.count());
            }
        }
        else {
            ++failed;
            hadErrors = true;
            if (!opts.quiet && !jsonOutput) {
                std::println("{}[FAILED]{} {} ({} ms)", style.Red(), style.Reset(), paddedLabel, duration.count());
            }
        }
    }
    const double elapsed = ElapsedSeconds(suiteStart);

    if ((!opts.quiet || failed > 0) && !jsonOutput) {
        std::println("\nCheck Result:");
        std::println("  Passed: {}{}{}", style.Green(), passed, style.Reset());
        if (failed > 0) {
            std::println("  Failed: {}{}{}", style.Red(), failed, style.Reset());
        }
        else {
            std::println("  Failed: {}", failed);
        }
        std::println("  Total : {}", passed + failed);
        std::println("  Time  : {:.2f}s", elapsed);
    }
    if (jsonOutput) {
        PrintDiagnosticsJson(jsonDiags, !hadErrors);
    }
    return hadErrors ? 1 : 0;
}
