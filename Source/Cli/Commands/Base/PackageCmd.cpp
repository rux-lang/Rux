#include "Rux/Cli/Cli.h"

#include "Rux/Asm.h"
#include "Rux/Ast.h"
#include "Rux/Cli/CliInternals.h"
#include "Rux/Hir.h"
#include "Rux/Lexer.h"
#include "Rux/Linker.h"
#include "Rux/Lir.h"
#include "Rux/Manifest.h"
#include "Rux/Package.h"
#include "Rux/Parser.h"
#include "Rux/Platform/Defines.h"
#include "Rux/Platform/Host.h"
#include "Rux/Rcu.h"
#include "Rux/Sema.h"
#include "Rux/Version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iomanip>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * This is separate from the other ifdef because otherwise clang-format attempts
 * to change the order, which makes MSVC cry.
 */

#if RUX_OS_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <windows.h>
#endif

#if RUX_OS_WINDOWS
    #include <psapi.h>
    #include <winhttp.h>
#else
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include "Rux/SourceLoader.h"

using namespace Rux;
using namespace Platform;
using namespace Misc;


int Cli::RunAdd(std::span<const std::string_view> args, const GlobalOptions& opts) {
    std::string_view spec;
    std::string_view pathArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpAdd();
            return 0;
        }
        if (arg == "--path") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--path' requires an argument\n");
                return 1;
            }
            pathArg = args[++i];
            continue;
        }
        if (!arg.starts_with('-') && spec.empty()) {
            spec = arg;
            continue;
        }
        PrintUnknownOption(arg, "add");
        return 1;
    }
    if (spec.empty()) {
        std::print(stderr, "error: missing package name\n\n");
        PrintHelpAdd();
        return 1;
    }
    auto manifestPath = RequireManifest();
    if (!manifestPath) return 1;
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) return 1;
    auto [pkgName, pkgVersion] = ParsePackageSpec(spec);

    if (!pathArg.empty()) {
        const bool changed = manifest->AddPathDependency(pkgName, std::string(pathArg));
        if (!manifest->Save(*manifestPath)) {
            std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
            return 1;
        }
        if (!opts.quiet) {
            if (changed)
                std::print("Added {} @ path '{}'\n", pkgName, pathArg);
            else
                std::print("Up-to-date {} @ path '{}'\n", pkgName, pathArg);
        }
        return 0;
    }

    if (!opts.quiet) std::print("     Fetching registry...\n");

    const auto jsonOpt = FetchUrl(std::string(kRegistryUrl));
    if (!jsonOpt) {
        std::print(stderr, "error: failed to fetch package registry\n");
        return 1;
    }

    if (JsonLookupString(*jsonOpt, pkgName).empty()) {
        std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
        return 1;
    }

    const bool changed = manifest->AddDependency(pkgName, pkgVersion);
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        const std::string ver = pkgVersion.empty() ? "latest" : pkgVersion;
        if (changed)
            std::print("Added {} @ {}\n", pkgName, ver);
        else
            std::print("Up-to-date {} @ {}\n", pkgName, ver);
    }
    return 0;
}

int Cli::RunRemove(std::span<const std::string_view> args, const GlobalOptions& opts) {
    std::string_view name;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpRemove();
            return 0;
        }
        if (!arg.starts_with('-') && name.empty()) {
            name = arg;
            continue;
        }
        PrintUnknownOption(arg, "remove");
        return 1;
    }
    if (name.empty()) {
        std::print(stderr, "error: missing package name\n\n");
        PrintHelpRemove();
        return 1;
    }
    auto manifestPath = RequireManifest();
    if (!manifestPath) return 1;
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) return 1;
    std::string pkgName(name);
    if (!manifest->RemoveDependency(pkgName)) {
        std::print(stderr, "error: package '{}' is not a dependency\n", pkgName);
        return 1;
    }
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) std::print("     Removed {}\n", pkgName);
    return 0;
}

int Cli::RunTest(std::span<const std::string_view> args, const GlobalOptions& opts) {
    bool isRelease = false;
    for (auto& arg : args) {
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpTest();
            return 0;
        }
        PrintUnknownOption(arg, "test");
        return 1;
    }
    auto manifestPath = RequireManifest();
    if (!manifestPath) return 1;
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) return 1;
    if (!opts.quiet) std::print("     Testing {} v{}\n", manifest->package.name, manifest->package.version);
    // TODO: build and run test targets
    std::println("Running executable...");
    std::println("Release: {}", isRelease);
    if (!opts.quiet) std::print("    Finished running tests\n");
    return 0;
}

int Cli::RunInit(std::span<const std::string_view> args, const GlobalOptions& opts) {
    bool bin = false;
    bool lib = false;
    for (auto& arg : args) {
        if (arg == "--bin") {
            bin = true;
            continue;
        }
        if (arg == "--lib") {
            lib = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpInit();
            return 0;
        }
        PrintUnknownOption(arg, "init");
        return 1;
    }
    const auto type = (lib && !bin) ? PackageType::SharedLibrary : PackageType::Executable;
    const auto root = std::filesystem::current_path();
    auto name = root.filename().string();
    if (!opts.quiet)
        std::print("  Initializing {} package '{}'\n", type == PackageType::Executable ? "binary" : "library", name);
    if (!ScaffoldPackage(root, name, type, /*initMode=*/true)) return 1;
    if (!opts.quiet) std::print("   Initialized package '{}'\n", name);
    return 0;
}