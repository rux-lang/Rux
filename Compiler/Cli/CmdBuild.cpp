// `rux build` and `rux clean`.

#include "Cli/Cli.h"
#include "Cli/DefineOption.h"
#include "Cli/TerminalStyle.h"
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
                    std::println(
                        stderr,
                        "error: unsupported --emit value '{}'; expected tokens, ast, sema, hir, lir, asm, or rcu",
                        value);
                    return 2;
                }
                if (comma == std::string_view::npos)
                    break;
                values.remove_prefix(comma + 1);
                if (values.empty()) {
                    std::println(stderr, "error: --emit contains an empty value");
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
                std::print(stderr, "error: {}\n", error);
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
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::string targetName = target.empty() ? HostTargetTriple() : CanonicalTargetTriple(target);
    if (!IsSupportedTargetTriple(targetName)) {
        std::print(stderr, "error: unsupported target '{}'; supported targets are {}\n", targetName,
                   SupportedTargetTriples());
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
    const std::string_view profileName = isRelease ? "Release" : "Debug";
    (void)isDebug;
    if (!opts.quiet && !showStats) {
        const AnsiStyle style{ColorEnabled(opts.color, OutputStream::Stderr)};
        std::print(stderr, "{}{}Compiling{} {}{}{} v{} [{}{}{}]\n", style.Cyan(), style.Bold(), style.Reset(),
                   style.Bold(), manifest->package.name.Text(), style.Reset(), manifest->package.version.Text(),
                   style.Cyan(), manifestPath->parent_path().string(), style.Reset());
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
        PrintBuildStats(result.primaryArtifactPath, profileName, result.stats,
                        ColorEnabled(opts.color, OutputStream::Stdout));
        return 0;
    }
    if (!opts.quiet) {
        PrintBuildSummary(result.primaryArtifactPath, profileName, result.stats,
                          ColorEnabled(opts.color, OutputStream::Stdout));
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
            std::print("Removed {}\n", dir.string());
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
