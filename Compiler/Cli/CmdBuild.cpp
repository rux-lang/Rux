// `rux build` and `rux clean`.

#include "Cli/BuildReport.h"
#include "Cli/Cli.h"
#include "Cli/CompilerProgress.h"
#include "Cli/DefineOption.h"
#include "Cli/ManifestInput.h"
#include "Cli/Reporter.h"
#include "Driver/BuildPlan.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "Reporting/Reporting.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace Rux;
using namespace CliSupport;
using namespace Driver;

int Cli::RunBuild(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const ReporterOptions reporterOptions{.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose};
    const Reporter output(stdout, reporterOptions);
    const Reporter diagnostics(stderr, reporterOptions);
    bool buildAll = false;
    bool isRelease = false;
    bool isDebug = false;
    std::string_view target;
    bool dumpTokens = false;
    bool dumpAst = false;
    bool dumpSema = false;
    bool dumpHir = false;
    bool dumpLir = false;
    bool dumpAsm = false;
    bool dumpRcu = false;
    bool showStats = false;
    std::map<std::string, std::string> defines;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "--all") {
            buildAll = true;
            continue;
        }
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "--debug") {
            isDebug = true;
            continue;
        }
        if (arg == "-q" || arg == "--quiet") {
            continue;
        }
        if (arg == "-v" || arg == "--verbose") {
            continue;
        }
        if (arg == "--stats") {
            showStats = true;
            continue;
        }
        if (arg == "--emit" && i + 1 < args.size()) {
            std::string_view values = args[++i];
            while (!values.empty()) {
                const auto comma = values.find(',');
                const auto value = values.substr(0, comma);
                if (value == "tokens")
                    dumpTokens = true;
                else if (value == "ast")
                    dumpAst = true;
                else if (value == "sema")
                    dumpSema = true;
                else if (value == "hir")
                    dumpHir = true;
                else if (value == "lir")
                    dumpLir = true;
                else if (value == "asm")
                    dumpAsm = true;
                else if (value == "rcu")
                    dumpRcu = true;
                else {
                    diagnostics.Error(std::format("value '{}' is not supported by option '--emit'", value));
                    diagnostics.Note("supported emission kinds are 'tokens', 'ast', 'sema', 'hir', 'lir', 'asm', and "
                                     "'rcu'");
                    return 2;
                }
                if (comma == std::string_view::npos)
                    break;
                values.remove_prefix(comma + 1);
                if (values.empty()) {
                    diagnostics.Error("option '--emit' contains an empty emission kind");
                    return 2;
                }
            }
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
                return 2;
            }
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("build");
            return 0;
        }
        PrintUnknownOption(arg, "build");
        return 1;
    }
    if (isDebug && isRelease) {
        diagnostics.Error("options '--debug' and '--release' cannot be used together");
        return 2;
    }
    const auto targetTriple =
        target.empty() ? std::optional{Target::TargetTriple::Host()} : Target::TargetTriple::Parse(target);
    if (!targetTriple) {
        diagnostics.Error(std::format("target '{}' is not supported", target));
        diagnostics.Note("supported targets are " + SupportedTargetTriples());
        return 1;
    }
    const std::string targetName(targetTriple->CanonicalName());
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    const BuildProfile profile = isRelease ? BuildProfile::Release : BuildProfile::Debug;
    WorkspaceSources workspaceSources;
    // Reported paths are relative to the directory the command was pointed at: a member's own root, or the workspace.
    std::filesystem::path displayRoot;

    // Builds the root package, or one workspace member against the sources the workspace offers.
    const auto BuildPackage = [&](const std::filesystem::path &packageManifestPath, Manifest packageManifest) {
        const std::string packageName(packageManifest.package.name.Text());
        const std::string packageVersion(packageManifest.package.version.Text());
        const auto packageRoot = packageManifestPath.parent_path();
        const auto shownRoot = displayRoot.empty() ? packageRoot : displayRoot;
        const auto Configure = [&](CompileOptions &copts) {
            copts.manifestPath = packageManifestPath;
            copts.defines = defines;
            copts.localPackageRoots = workspaceSources.packageRoots;
            copts.localNamespaces = workspaceSources.namespaces;
            ConfigureCompileDiagnostics(copts, diagnostics);
            if (opts.verbose) {
                copts.emitProgress = [&](const CompileProgress &progress) { ReportCompileProgress(output, progress); };
            }
        };
        if (buildAll) {
            const auto matrix = GenerateBuildMatrix(packageRoot, packageManifest);
            if (!opts.quiet) {
                diagnostics.Progress(
                    "Compiling", std::format("{} v{} ({} build cells)", packageName, packageVersion, matrix.size()));
            }

            std::vector<BuildCellReport> reports;
            reports.reserve(matrix.size());
            bool allSucceeded = true;
            for (const auto &cell : matrix) {
                output.Verbose(
                    std::format("Compiling '{}' {}", packageName, FormatBuildContext(cell.profile, cell.target)));
                CompileOptions copts;
                Configure(copts);
                copts.manifest = packageManifest;
                copts.target = cell.target;
                copts.profile = cell.profile;

                const auto started = std::chrono::steady_clock::now();
                CompilerDriver driver(std::move(copts));
                const CompileResult result = driver.Compile();
                const auto elapsed = ElapsedMs(started);
                reports.push_back(BuildCellReport{.profile = cell.profile,
                                                  .target = cell.target,
                                                  .outputDirectory = cell.outputDirectory,
                                                  .succeeded = result.ok,
                                                  .artifactPath = result.primaryArtifactPath,
                                                  .stats = result.stats,
                                                  .elapsed = elapsed});
                allSucceeded &= result.ok;
            }

            output.Write(FormatBuildMatrixReport(packageName, reports, shownRoot, showStats, output.Style().enabled));
            return allSucceeded;
        }
        if (!opts.quiet) {
            diagnostics.Progress("Compiling", std::format("{} v{} {}", packageName, packageVersion,
                                                          FormatBuildContext(profile, *targetTriple)));
        }
        CompileOptions copts;
        Configure(copts);
        copts.manifest = std::move(packageManifest);
        copts.target = *targetTriple;
        copts.profile = profile;
        copts.dumpTokens = dumpTokens;
        copts.dumpAst = dumpAst;
        copts.dumpSema = dumpSema;
        copts.dumpHir = dumpHir;
        copts.dumpLir = dumpLir;
        copts.dumpAsm = dumpAsm;
        copts.dumpRcu = dumpRcu;
        CompilerDriver driver(std::move(copts));
        const CompileResult result = driver.Compile();
        for (const auto &inspection : result.inspectionOutputs) {
            output.Success("Emitted", InspectionHeading(inspection.kind));
            output.Detail(std::format("Description: {}", InspectionDescription(inspection.kind)));
            output.Detail(std::format("Output: {}", DisplayPath(inspection.path, shownRoot)));
        }
        if (!result.ok) {
            return false;
        }
        if (!opts.quiet && showStats) {
            const BuildReportInfo info{.packageName = packageName,
                                       .packageVersion = packageVersion,
                                       .artifactPath = result.primaryArtifactPath,
                                       .packageRoot = shownRoot,
                                       .profile = profile,
                                       .targetTriple = targetName};
            output.Write(FormatBuildStats(info, result.stats, output.Style().enabled));
            return true;
        }
        output.Write(FormatBuildSummary(packageName, result.primaryArtifactPath, shownRoot, profile, targetName,
                                        result.stats, output.Style().enabled));
        return true;
    };

    if (!manifest->IsWorkspace()) {
        return BuildPackage(*manifestPath, std::move(*manifest)) ? 0 : 1;
    }

    // A workspace has no sources of its own, so it builds each member that produces an artifact. A source library is
    // compiled into the members that depend on it and has nothing to produce alone.
    if (!opts.quiet) {
        diagnostics.Progress("Compiling", buildAll ? std::string("workspace")
                                                   : "workspace " + FormatBuildContext(profile, *targetTriple));
    }
    auto members = LoadWorkspaceMembers(*manifestPath, *manifest);
    workspaceSources = CollectWorkspaceSources(members);
    displayRoot = manifestPath->parent_path();
    std::size_t built = 0;
    std::size_t failed = 0;
    std::size_t sourceLibraries = 0;
    const auto started = std::chrono::steady_clock::now();
    for (auto &member : members) {
        if (!member.manifest) {
            if (!member.problem.empty())
                diagnostics.Error(member.problem);
            ++failed;
            continue;
        }
        const std::string name(member.manifest->package.name.Text());
        if (member.manifest->package.type == ManifestPackageType::SourceLibrary) {
            ++sourceLibraries;
            output.Verbose(std::format("Skipping source library '{}'", name));
            continue;
        }
        if (!buildAll && IsPlatformPackageName(name) && !PlatformPackageMatchesTarget(name, *targetTriple)) {
            output.Verbose(std::format("Skipping '{}', which does not support {}", name,
                                       Driver::TargetDisplayName(*targetTriple)));
            continue;
        }
        if (BuildPackage(member.manifestPath, std::move(*member.manifest)))
            ++built;
        else
            ++failed;
    }
    if (built + failed == 0) {
        diagnostics.Error(std::format("workspace '{}' has no member to build", manifestPath->parent_path().string()));
        diagnostics.Note(
            sourceLibraries > 0
                ? "a source library is compiled into the packages that depend on it and builds nothing alone"
                : "no member produces an artifact for this target");
        diagnostics.Help("check the members with 'rux check'");
        return 1;
    }
    if (!opts.quiet) {
        const auto totals =
            std::format("{} in {} ({} succeeded, {} failed)", Reporting::FormatCount(built + failed, "package"),
                        Reporting::FormatDuration(ElapsedMs(started)), built, failed);
        if (failed == 0)
            output.Success("Built", totals);
        else
            output.Failure("Failed", totals);
    }
    return failed == 0 ? 0 : 1;
}

int Cli::RunClean(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const ReporterOptions reporterOptions{.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose};
    const Reporter output(stdout, reporterOptions);
    const Reporter diagnostics(stderr, reporterOptions);
    bool tempOnly = false;
    for (auto &arg : args) {
        if (arg == "--temp") {
            tempOnly = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("clean");
            return 0;
        }
        PrintUnknownOption(arg, "clean");
        return 1;
    }
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    const auto root = manifestPath->parent_path();
    const auto outputDir = ResolveRawOutputRoot(root, *manifest);
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> removed;
    auto removeDir = [&](const std::filesystem::path &dir) -> bool {
        std::error_code ec;
        if (!std::filesystem::exists(dir)) {
            output.Verbose(std::format("Nothing to remove at '{}'", DisplayPath(dir, root)));
            return true;
        }
        std::filesystem::remove_all(dir, ec);
        if (ec) {
            diagnostics.Error(std::format("could not remove directory '{}'", dir.string()));
            diagnostics.Note(std::format("system error {}: {}", ec.value(), ec.message()));
            return false;
        }
        removed.push_back(dir);
        return true;
    };
    bool ok = true;
    if (!tempOnly) {
        ok &= removeDir(outputDir);
    }
    ok &= removeDir(root / "Temp");
    if (ok) {
        output.Success("Removed", std::format("{} in {}", Reporting::FormatCount(removed.size(), "directory"),
                                              Reporting::FormatDuration(ElapsedMs(started))));
        for (const auto &dir : removed) {
            output.Detail("Path: " + DisplayPath(dir, root));
        }
    }
    return ok ? 0 : 1;
}
