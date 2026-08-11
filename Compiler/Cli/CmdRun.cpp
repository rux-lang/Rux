// `rux run` — build the package, then execute the resulting binary.

#include "Cli/Cli.h"
#include "Cli/DefineOption.h"
#include "Cli/TerminalStyle.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "System/Process.h"

#include <cstdio>
#include <filesystem>
#include <print>
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
    bool isRelease = false;
    std::string_view target;
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
    // Reject artifact-producing libraries before spending a build on them.
    // SourceLibrary is rejected by the driver itself because it has no artifact.
    if (manifest->package.type == ManifestPackageType::SharedLibrary ||
        manifest->package.type == ManifestPackageType::StaticLibrary) {
        std::print(stderr, "error: cannot run a {} package; only an Executable package has an entry point\n",
                   ToString(manifest->package.type));
        return 1;
    }
    // Build first (quiet unless verbose)
    const std::string_view profileName = isRelease ? "Release" : "Debug";
    std::string targetName = target.empty() ? HostTargetTriple() : CanonicalTargetTriple(target);
    if (!IsSupportedTargetTriple(targetName)) {
        std::print(stderr, "error: unsupported target '{}'; supported targets are {}\n", targetName,
                   SupportedTargetTriples());
        return 1;
    }
    if (const auto reason = UnsupportedBackendReason(targetName); !reason.empty()) {
        std::print(stderr, "error: {}\n", reason);
        return 1;
    }
    // Executing what was built needs the host to be the target. An emulator for
    // a foreign target is BACKLOG.md task 19.
    if (!HostCanExecuteTarget(targetName)) {
        std::print(stderr,
                   "error: cannot run a '{}' artifact on '{}'; build it with 'rux build --target {}' and run "
                   "it on that target\n",
                   targetName, HostTargetTriple(), targetName);
        return 1;
    }
    const bool buildQuiet = !opts.verbose || opts.quiet;
    if (!buildQuiet) {
        const AnsiStyle style{ColorEnabled(opts.color, OutputStream::Stderr)};
        std::print(stderr, "{}{}Compiling{} {}{}{} v{} [{}{}{}]\n", style.Cyan(), style.Bold(), style.Reset(),
                   style.Bold(), manifest->package.name.Text(), style.Reset(), manifest->package.version.Text(),
                   style.Cyan(), manifestPath->parent_path().string(), style.Reset());
    }
    CompileOptions copts;
    copts.manifestPath = *manifestPath;
    copts.manifest = *manifest;
    copts.targetName = targetName;
    copts.profileName = std::string(profileName);
    copts.defines = std::move(defines);
    copts.quiet = buildQuiet;
    copts.verbose = opts.verbose;
    CompilerDriver driver(std::move(copts));
    const CompileResult result = driver.Compile();
    if (!result.ok) {
        return 1;
    }
    if (!buildQuiet) {
        const auto report = FormatBuildSummary(result.primaryArtifactPath, profileName, targetName, result.stats,
                                               ColorEnabled(opts.color, OutputStream::Stderr));
        std::fwrite(report.data(), sizeof(char), report.size(), stderr);
    }
    auto exePath = result.primaryArtifactPath;
    if (!std::filesystem::exists(exePath)) {
        std::print(stderr, "error: executable not found: '{}'\n", exePath.string());
        return 1;
    }
    if (opts.verbose && !opts.quiet) {
        std::print("Running `{}`\n", exePath.string());
    }
    const auto exitCode = RunInherited(exePath, runArgs);
    if (!exitCode) {
        std::print(stderr, "error: failed to launch '{}'\n", exePath.string());
        return 1;
    }
    return *exitCode;
}
