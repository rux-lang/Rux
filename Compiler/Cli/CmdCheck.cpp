// `rux check` — frontend-only compile with text or JSON diagnostics.

#include "Cli/Cli.h"
#include "Cli/CompilerProgress.h"
#include "Cli/DefineOption.h"
#include "Cli/Reporter.h"
#include "Cli/TerminalStyle.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"

#include <algorithm>
#include <array>
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
    const Reporter progress(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const bool jsonOutput = std::ranges::find(args, "--json") != args.end();
    const bool diagnosticColor = !jsonOutput && ColorEnabled(opts.color, OutputStream::Stderr);
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
            continue;
        }
        if (arg == "--target" && i + 1 < args.size()) {
            target = args[++i];
            continue;
        }
        if (arg == "--define" && i + 1 < args.size()) {
            std::string error;
            if (!AddCompileTimeDefine(args[++i], defines, error)) {
                if (jsonOutput) {
                    PrintDiagnosticsJson(std::array{ErrorDiagnostic(error)}, false);
                }
                else {
                    std::print(stderr, "error: {}\n", error);
                }
                return 2;
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
    auto EmitDiag = [&](Diagnostic diag, const SourceLineLookup &sourceLineLookup = {}) {
        if (jsonOutput) {
            jsonDiags.push_back(std::move(diag));
        }
        else {
            std::print(stderr, "{}", RenderDiagnostic(diag, diagnosticColor, sourceLineLookup));
        }
    };
    auto EmitFatal = [&](std::string message) {
        EmitDiag(ErrorDiagnostic(std::move(message)));
        hadErrors = true;
    };
    auto Finish = [&](const int exitCode) {
        if (jsonOutput)
            PrintDiagnosticsJson(jsonDiags, exitCode == 0);
        return exitCode;
    };
    const auto targetTriple =
        target.empty() ? std::optional{Target::TargetTriple::Host()} : Target::TargetTriple::Parse(target);
    if (!targetTriple) {
        EmitDiag(ErrorDiagnostic("target '" + std::string(target) + "' is not supported",
                                 {"supported targets are " + SupportedTargetTriples()},
                                 "try 'rux check --target linux-x86_64'", "https://rux-lang.dev/cli/"));
        hadErrors = true;
        return Finish(1);
    }
    const std::string targetName(targetTriple->CanonicalName());
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        if (jsonOutput) {
            EmitFatal("could not find 'Rux.toml' in current directory or any "
                      "parent directory");
        }
        return Finish(1);
    }
    auto rootResult = Manifest::Load(*manifestPath);
    if (!rootResult.Ok()) {
        for (const auto &diagnostic : rootResult.diagnostics) {
            EmitFatal(diagnostic.Format());
        }
        return Finish(1);
    }
    auto manifest = std::move(rootResult.manifest);
    std::map<std::string, std::filesystem::path> localPackageRoots;
    bool localDependenciesOnly = false;
    auto CheckPackage = [&](const std::filesystem::path &packageManifestPath, Manifest packageManifest) {
        if (packageManifest.IsWorkspace() || packageManifest.package.name.Empty()) {
            EmitFatal("workspace member '" + packageManifestPath.parent_path().string() + "' is not a package");
            return false;
        }
        CompileOptions copts;
        copts.manifestPath = packageManifestPath;
        copts.manifest = std::move(packageManifest);
        copts.target = *targetTriple;
        copts.defines = defines;
        copts.localPackageRoots = localPackageRoots;
        copts.localDependenciesOnly = localDependenciesOnly;
        if (opts.verbose && !jsonOutput) {
            copts.emitProgress = [&](const CompileProgress &event) { ReportCompileProgress(progress, event); };
        }
        copts.checkOnly = true;
        copts.emitDiagnostic = [&](const Diagnostic &diagnostic, const SourceLineLookup &sourceLineLookup) {
            EmitDiag(diagnostic, sourceLineLookup);
        };
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
            auto memberResult = Manifest::Load(memberManifestPath);
            if (!memberResult.Ok()) {
                for (const auto &diagnostic : memberResult.diagnostics) {
                    EmitFatal(diagnostic.Format());
                }
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            auto memberManifest = std::move(memberResult.manifest);
            if (memberManifest->IsWorkspace() || memberManifest->package.name.Empty()) {
                EmitFatal("workspace member '" + member + "' is not a package");
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            const auto [existing, inserted] =
                localPackageRoots.emplace(memberManifest->package.name.Normalized(), memberManifestPath.parent_path());
            if (!inserted && existing->second != memberManifestPath.parent_path()) {
                EmitFatal("duplicate workspace package name '" + memberManifest->package.name.Text() + "'");
                jobs.push_back({memberManifestPath, label, std::nullopt});
                continue;
            }
            if (IsPlatformPackageName(memberManifest->package.name.Text()) &&
                !PlatformPackageMatchesTarget(memberManifest->package.name.Text(), *targetTriple)) {
                continue;
            }
            jobs.push_back({memberManifestPath, label, std::move(*memberManifest)});
        }
        localDependenciesOnly = true;
    }
    else {
        if (!opts.quiet && !jsonOutput) {
            std::println("Checking {} v{}", manifest->package.name.Text(), manifest->package.version.Text());
        }
        jobs.push_back({*manifestPath, manifest->package.name.Text(), std::move(*manifest)});
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
    return Finish(hadErrors ? 1 : 0);
}
