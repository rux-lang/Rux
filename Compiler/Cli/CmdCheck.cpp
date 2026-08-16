// `rux check` — frontend-only compile with text or JSON diagnostics.

#include "Cli/Cli.h"
#include "Cli/CompilerProgress.h"
#include "Cli/DefineOption.h"
#include "Cli/ManifestInput.h"
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
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;
using namespace CliSupport;
using namespace Driver;

int Cli::RunCheck(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
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
                    output.Write(RenderDiagnosticsJson(std::array{ErrorDiagnostic(error)}, false),
                                 MessageVisibility::Always);
                }
                else {
                    diagnostics.Error(error);
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
            diagnostics.Write(RenderDiagnostic(diag, diagnosticColor, sourceLineLookup), MessageVisibility::Always);
        }
    };
    auto EmitFatal = [&](std::string message) {
        EmitDiag(ErrorDiagnostic(std::move(message)));
        hadErrors = true;
    };
    auto EmitManifestDiagnostic = [&](const ManifestDiagnostic &diagnostic) {
        Diagnostic converted{Diagnostic::Severity::Error,
                             diagnostic.path.string(),
                             {.line = diagnostic.line, .column = diagnostic.column, .offset = 0},
                             diagnostic.message,
                             diagnostic.notes,
                             diagnostic.help,
                             diagnostic.documentationUrl};
        const auto sourceLine = diagnostic.sourceLine;
        EmitDiag(std::move(converted), [sourceLine](std::string_view, std::size_t) -> std::optional<std::string_view> {
            return sourceLine ? std::optional<std::string_view>(*sourceLine) : std::nullopt;
        });
        hadErrors = true;
    };
    auto Finish = [&](const int exitCode) {
        if (jsonOutput) {
            output.Write(RenderDiagnosticsJson(jsonDiags, exitCode == 0), MessageVisibility::Always);
        }
        return exitCode;
    };
    const auto targetTriple =
        target.empty() ? std::optional{Target::TargetTriple::Host()} : Target::TargetTriple::Parse(target);
    if (!targetTriple) {
        EmitDiag(ErrorDiagnostic("target '" + std::string(target) + "' is not supported",
                                 {"supported targets are " + SupportedTargetTriples()},
                                 "try 'rux check --target linux-x86_64'", "https://rux-lang.dev/docs/cli"));
        hadErrors = true;
        return Finish(1);
    }
    const std::string targetName(targetTriple->CanonicalName());
    std::optional<std::filesystem::path> manifestPath;
    if (!jsonOutput) {
        manifestPath = RequireManifest(opts.manifest);
    }
    else if (opts.manifest.empty()) {
        manifestPath = Manifest::Find();
    }
    else if (std::filesystem::exists(opts.manifest)) {
        manifestPath = opts.manifest;
    }
    if (!manifestPath) {
        if (jsonOutput) {
            EmitFatal(opts.manifest.empty() ? "could not find 'Rux.toml' in current directory or any parent directory"
                                            : "specified manifest '" + opts.manifest.string() + "' was not found");
        }
        return Finish(1);
    }
    auto rootResult = Manifest::Load(*manifestPath);
    if (!rootResult.Ok()) {
        for (const auto &diagnostic : rootResult.diagnostics) {
            EmitManifestDiagnostic(diagnostic);
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
            copts.emitProgress = [&](const CompileProgress &event) { ReportCompileProgress(output, event); };
        }
        copts.checkOnly = true;
        copts.emitDiagnostic = [&](const Diagnostic &diagnostic, const SourceLineLookup &sourceLineLookup) {
            EmitDiag(diagnostic, sourceLineLookup);
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
        if (!jsonOutput) {
            output.Progress("Checking", std::format("workspace ({})", targetName));
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
                    EmitManifestDiagnostic(diagnostic);
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
            jobs.push_back({memberManifestPath, memberManifest->package.name.Text(), std::move(*memberManifest)});
        }
        localDependenciesOnly = true;
    }
    else {
        if (!jsonOutput) {
            output.Progress("Checking", std::format("{} v{} ({})", manifest->package.name.Text(),
                                                    manifest->package.version.Text(), targetName));
        }
        jobs.push_back({*manifestPath, manifest->package.name.Text(), std::move(*manifest)});
    }

    if (!jsonOutput) {
        output.Detail(std::format("Packages: {}", jobs.size()));
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    const auto suiteStart = std::chrono::steady_clock::now();
    for (auto &job : jobs) {
        const auto start = std::chrono::steady_clock::now();
        const bool packagePassed = job.manifest && CheckPackage(job.manifestPath, std::move(*job.manifest));
        const auto duration = ElapsedMs(start);

        if (packagePassed) {
            ++passed;
            if (!jsonOutput) {
                output.Success("Checked", std::format("{} in {}", job.label, Reporting::FormatDuration(duration)));
            }
        }
        else {
            ++failed;
            hadErrors = true;
            if (!jsonOutput) {
                output.Failure("Failed", std::format("{} in {}", job.label, Reporting::FormatDuration(duration)));
            }
        }
    }
    const auto elapsed = ElapsedMs(suiteStart);

    if (!jsonOutput && !opts.quiet) {
        const auto totals =
            std::format("{} in {} ({} passed, {} failed)", Reporting::FormatCount(passed + failed, "package"),
                        Reporting::FormatDuration(elapsed), passed, failed);
        if (failed == 0) {
            output.Success("Checked", totals);
        }
        else {
            output.Failure("Failed", totals);
        }
    }
    return Finish(hadErrors ? 1 : 0);
}
