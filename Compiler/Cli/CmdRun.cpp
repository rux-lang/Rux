// `rux run` — build the package, then execute the resulting binary.

#include "Cli/Cli.h"
#include "Cli/CompilerProgress.h"
#include "Cli/DefineOption.h"
#include "Cli/ManifestInput.h"
#include "Cli/Reporter.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "System/Process.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;
using namespace CliSupport;
using namespace Driver;
using namespace System;

int Cli::RunRun(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const Reporter tool(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    bool isRelease = false;
    std::vector<std::string_view> runArgs;
    std::map<std::string, std::string> defines;
    bool passThroughMode = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (passThroughMode) {
            runArgs.push_back(arg);
            continue;
        }
        if (arg == "--") {
            passThroughMode = true;
            continue;
        }
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "--define" && i + 1 < args.size()) {
            std::string error;
            if (!AddCompileTimeDefine(args[++i], defines, error)) {
                tool.Error(error);
                return 1;
            }
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("run");
            return 0;
        }
        PrintUnknownOption(arg, "run");
        return 1;
    }
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    const std::string packageName(manifest->package.name.Text());
    if (manifest->package.type != ManifestPackageType::Executable) {
        if (manifest->package.type == ManifestPackageType::SourceLibrary) {
            tool.Error(std::format("package '{}' is a source library and cannot be run", packageName));
            tool.Note("source libraries are compiled into packages that depend on them");
            tool.Help("check it with 'rux check'");
        }
        else {
            const std::string_view kind =
                manifest->package.type == ManifestPackageType::SharedLibrary ? "shared library" : "static library";
            tool.Error(std::format("package '{}' produces a {} and cannot be run", packageName, kind));
            tool.Note("only executable packages have an entry point");
            tool.Help(std::format("build it with 'rux build{}'", isRelease ? " --release" : ""));
        }
        return 1;
    }
    // Build first (quiet unless verbose)
    const BuildProfile profile = isRelease ? BuildProfile::Release : BuildProfile::Debug;
    const std::string targetName = HostTargetTriple();
    const bool buildQuiet = !opts.verbose || opts.quiet;
    if (!buildQuiet) {
        tool.Progress("Compiling", std::format("{} v{} ({}, {})", packageName, manifest->package.version.Text(),
                                               profile == BuildProfile::Release ? "release" : "debug", targetName));
    }
    CompileOptions copts;
    copts.manifestPath = *manifestPath;
    copts.manifest = *manifest;
    copts.target = Target::TargetTriple::Host();
    copts.profile = profile;
    copts.defines = std::move(defines);
    ConfigureCompileDiagnostics(copts, tool);
    if (opts.verbose) {
        copts.emitProgress = [&](const CompileProgress &progress) { ReportCompileProgress(tool, progress); };
    }
    CompilerDriver driver(std::move(copts));
    const CompileResult result = driver.Compile();
    if (!result.ok) {
        return 1;
    }
    if (!buildQuiet) {
        tool.Write(FormatBuildSummary(packageName, result.primaryArtifactPath, manifestPath->parent_path(), profile,
                                      targetName, result.stats, tool.Style().enabled));
    }
    auto exePath = result.primaryArtifactPath;
    if (!std::filesystem::exists(exePath)) {
        tool.Error(std::format("built executable '{}' is missing", exePath.string()));
        tool.Help(std::format("rebuild it with 'rux build{}'", isRelease ? " --release" : ""));
        return 1;
    }
    if (opts.verbose && !opts.quiet) {
        std::string commandLine = "'" + exePath.string() + "'";
        for (const auto argument : runArgs) {
            commandLine += " '";
            commandLine += argument;
            commandLine += '\'';
        }
        tool.Progress("Running", packageName);
        tool.Verbose("Command: " + commandLine);
    }
    const auto started = std::chrono::steady_clock::now();
    std::error_code launchError;
    const auto exitCode = RunInherited(exePath, runArgs, &launchError);
    if (!exitCode) {
        tool.Error(std::format("could not launch executable '{}'", exePath.string()));
        if (launchError) {
            tool.Note(std::format("system error {}: {}", launchError.value(), launchError.message()));
        }
        tool.Help("check that the output exists and is executable on this host");
        return 1;
    }
    tool.Verbose(
        std::format("Process exited with code {} in {}", *exitCode, Reporting::FormatDuration(ElapsedMs(started))));
    return *exitCode;
}
