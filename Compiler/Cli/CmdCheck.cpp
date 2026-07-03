// `rux check` — frontend-only compile with text or JSON diagnostics.

#include "Cli/Cli.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"

#include <cstdio>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Diagnostics/Diagnostics.h"

using namespace Rux;
using namespace Driver;

int Cli::RunCheck(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool jsonOutput = false;
    std::string_view target;
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
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        if (jsonOutput) {
            EmitFatal("failed to parse 'Rux.toml'");
        }
        return 1;
    }
    std::string targetName = target.empty() ? HostTargetTriple() : std::string(target);
    if (!IsSupportedTargetTriple(targetName)) {
        if (jsonOutput) {
            EmitFatal("unsupported target '" + targetName + "'");
        }
        else {
            std::print(stderr,
                       "error: unsupported target '{}'; supported targets are "
                       "linux-x64, windows-x64, macos-x64, macos-aarch64, "
                       "freebsd-x64, openbsd-x64, netbsd-x64, dragonfly-x64, "
                       "illumos-x64\n",
                       targetName);
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
    if (!opts.quiet && !jsonOutput) {
        std::print("Checking {} v{} [{}]\n", manifest->package.name, manifest->package.version,
                   manifestPath->parent_path().string());
    }
    CompileOptions copts;
    copts.manifestPath = *manifestPath;
    copts.manifest = std::move(*manifest);
    copts.targetName = std::move(targetName);
    copts.profileName = "Debug";
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
    const CompileResult result = driver.Compile();
    if (!result.ok) {
        hadErrors = true;
    }
    if (jsonOutput) {
        PrintDiagnosticsJson(jsonDiags, !hadErrors);
    }
    return hadErrors ? 1 : 0;
}
