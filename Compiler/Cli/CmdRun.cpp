// `rux run` — build the package, then execute the resulting binary.

#include "Cli/Cli.h"
#include "Cli/DefineOption.h"
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
    // Build first (quiet unless verbose)
    const std::string_view profileName = isRelease ? "Release" : "Debug";
    std::string targetName = HostTargetTriple();
    if (!IsSupportedTargetTriple(targetName)) {
        std::print(stderr, "error: unsupported target '{}'; supported targets are {}\n", targetName,
                   SupportedTargetTriples());
        return 1;
    }
    const bool buildQuiet = !opts.verbose || opts.quiet;
    if (!buildQuiet) {
        std::print("Compiling {} v{} [{}]\n", manifest->package.name, manifest->package.version,
                   manifestPath->parent_path().string());
    }
    CompileOptions copts;
    copts.manifestPath = *manifestPath;
    copts.manifest = *manifest;
    copts.targetName = std::move(targetName);
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
        PrintBuildSummary(result.executablePath, profileName, result.stats);
    }
    const bool runDll = (manifest->package.type == "Dll" || manifest->package.type == "dll");
    if (runDll) {
        std::print(stderr, "error: cannot run a DLL package directly\n");
        return 1;
    }
    auto exePath = result.executablePath;
    if (!std::filesystem::exists(exePath)) {
        std::print(stderr, "error: executable not found: '{}'\n", exePath.string());
        return 1;
    }
    if (opts.verbose && !opts.quiet) {
        std::print("     Running `{}`\n", exePath.string());
    }
    const auto exitCode = RunInherited(exePath, runArgs);
    if (!exitCode) {
        std::print(stderr, "error: failed to launch '{}'\n", exePath.string());
        return 1;
    }
    return *exitCode;
}
