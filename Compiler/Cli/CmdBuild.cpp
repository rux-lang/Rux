// `rux build` and `rux clean`.

#include "Cli/Cli.h"
#include "Cli/DefineOption.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"

#include <cstdio>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace Rux;
using namespace CliSupport;
using namespace Driver;

int Cli::RunBuild(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool isRelease = false;
    bool isDebug = false;
    std::string_view profile;
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
        if (arg == "--dump-tokens") {
            dumpTokens = true;
            continue;
        }
        if (arg == "--dump-ast") {
            dumpAst = true;
            continue;
        }
        if (arg == "--dump-sema") {
            dumpSema = true;
            continue;
        }
        if (arg == "--dump-hir") {
            dumpHir = true;
            continue;
        }
        if (arg == "--dump-lir") {
            dumpLir = true;
            continue;
        }
        if (arg == "--dump-asm") {
            dumpAsm = true;
            continue;
        }
        if (arg == "--dump-rcu") {
            dumpRcu = true;
            continue;
        }
        if (arg == "--profile" && i + 1 < args.size()) {
            profile = args[++i];
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
            PrintHelpFor("build");
            return 0;
        }
        PrintUnknownOption(arg, "build");
        return 1;
    }
    (void)isDebug; // Stop -Wunused-but-set-variable
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::string targetName = target.empty() ? HostTargetTriple() : std::string(target);
    if (!IsSupportedTargetTriple(targetName)) {
        std::print(stderr,
                   "error: unsupported target '{}'; supported targets are "
                   "linux-x64, windows-x64, macos-x64, macos-arm64, "
                   "freebsd-x64, openbsd-x64, netbsd-x64, illumos-x64\n",
                   targetName);
        return 1;
    }
    const std::string hostTarget = HostTargetTriple();
    if (hostTarget != "unknown" && targetName != hostTarget) {
        // Target selection is currently used for source/dependency choice.
        // Linking foreign executable formats is kept explicit until the
        // backends support it end-to-end.
        std::print(stderr,
                   "error: cross-target build from '{}' to '{}' is not "
                   "supported yet\n",
                   hostTarget, targetName);
        return 1;
    }
    std::string_view profileName = isRelease ? "Release" : "Debug";
    if (!profile.empty()) {
        profileName = profile;
    }
    if (!opts.quiet && !showStats) {
        std::print("Compiling {} v{} [{}]\n", manifest->package.name, manifest->package.version,
                   manifestPath->parent_path().string());
    }
    CompileOptions copts;
    copts.manifestPath = *manifestPath;
    copts.manifest = std::move(*manifest);
    copts.targetName = std::move(targetName);
    copts.profileName = std::string(profileName);
    copts.defines = std::move(defines);
    copts.quiet = opts.quiet;
    copts.verbose = opts.verbose;
    copts.dumpTokens = dumpTokens;
    copts.dumpAst = dumpAst;
    copts.dumpSema = dumpSema;
    copts.dumpHir = dumpHir;
    copts.dumpLir = dumpLir;
    copts.dumpAsm = dumpAsm;
    copts.dumpRcu = dumpRcu;
    CompilerDriver driver(std::move(copts));
    const CompileResult result = driver.Compile();
    if (!result.ok) {
        return 1;
    }
    if (!opts.quiet && showStats) {
        PrintBuildStats(result.executablePath, profileName, result.stats);
        return 0;
    }
    if (!opts.quiet) {
        PrintBuildSummary(result.executablePath, profileName, result.stats);
    }
    return 0;
}

int Cli::RunClean(std::span<const std::string_view> args, const GlobalOptions &opts) {
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
    const auto outputDir = manifest->build.output.empty() ? root / "Bin"
                                                          : (std::filesystem::path(manifest->build.output).is_relative()
                                                                 ? root / manifest->build.output
                                                                 : std::filesystem::path(manifest->build.output));
    auto removeDir = [&](const std::filesystem::path &dir) -> bool {
        std::error_code ec;
        if (!std::filesystem::exists(dir)) {
            return true;
        }
        std::filesystem::remove_all(dir, ec);
        if (ec) {
            std::print(stderr, "error: failed to remove '{}': {}\n", dir.string(), ec.message());
            return false;
        }
        if (!opts.quiet) {
            std::print("     Removed {}\n", dir.string());
        }
        return true;
    };
    bool ok = true;
    if (!tempOnly) {
        ok &= removeDir(outputDir);
    }
    ok &= removeDir(root / "Temp");
    return ok ? 0 : 1;
}
