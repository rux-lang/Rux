// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#include "Rux/Cli/Cli.h"
#include "Rux/Cli/CliInternals.h"
#include "Rux/Hir.h"
#include "Rux/Manifest.h"
#include "Rux/Package.h"
#include "Rux/Platform/Defines.h"
#include "Rux/Platform/Host.h"
#include "Rux/Version.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/*
 * This is separate from the other ifdef because otherwise clang-format attempts
 * to change the order, which makes MSVC cry.
 */

#if RUX_OS_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif

#  ifndef NOMINMAX
#    define NOMINMAX
#  endif

#  include <windows.h>
#endif

#if RUX_OS_WINDOWS
#  include <psapi.h>
#else
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

using namespace Rux;
using namespace Platform;
using namespace Misc;

int Cli::RunHelp(std::span<const std::string_view> args, const GlobalOptions&) {
    if (!args.empty()) {
        PrintHelpFor(args.front());
    }
    else {
        PrintHelp();
    }
    return 0;
}

int Cli::RunVersion(const GlobalOptions&) {
    PrintVersion();
    return 0;
}

int Cli::RunFmt(std::span<const std::string_view> args,
                const GlobalOptions& opts) {
    bool check = false;
    bool manifestOnly = false;
    for (auto& arg : args) {
        if (arg == "--check") {
            check = true;
            continue;
        }
        if (arg == "--manifest") {
            manifestOnly = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("fmt");
            return 0;
        }
        PrintUnknownOption(arg, "fmt");
        return 1;
    }
    auto manifestPath = RequireManifest();
    if (!manifestPath) {
        return 1;
    }
    auto root = manifestPath->parent_path();
    if (manifestOnly) {
        if (!opts.quiet) {
            std::print("  Formatting {}\n", manifestPath->string());
        }
        // TODO: TOML formatter
        return 0;
    }
    auto sourceDir = root / "Source";
    if (!std::filesystem::exists(sourceDir)) {
        if (!opts.quiet) {
            std::print("  No source directory found.\n");
        }
        return 0;
    }
    int fileCount = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(sourceDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".rux") {
            continue;
        }
        ++fileCount;
        if (!opts.quiet) {
            if (check) {
                std::print("  Checking   {}\n", entry.path().string());
            }
            else {
                std::print("  Formatting {}\n", entry.path().string());
            }
        }
        // TODO: source formatter
    }
    if (fileCount == 0 && !opts.quiet) {
        std::print("  No .rux files found.\n");
    }
    return 0;
}

int Cli::RunDoc(std::span<const std::string_view> args,
                const GlobalOptions& opts) {
    bool openAfter = false;
    for (auto& arg : args) {
        if (arg == "--open") {
            openAfter = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("doc");
            return 0;
        }
        PrintUnknownOption(arg, "doc");
        return 1;
    }
    const auto manifestPath = RequireManifest();
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("  Generating documentation for {} v{}\n",
                   manifest->package.name,
                   manifest->package.version);
    }

    // TODO: documentation generator

    if (openAfter && !opts.quiet) {
        std::print("     Opening documentation...\n");
    }

    return 0;
}

int Cli::RunList(std::span<const std::string_view> args,
                 const GlobalOptions& opts) {
    bool global = false;
    for (auto arg : args) {
        if (arg == "--global") {
            global = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("list");
            return 0;
        }
        PrintUnknownOption(arg, "list");
        return 1;
    }

    if (global) {
        const auto cacheDir = RegistryPackagesDir();
        std::vector<std::string> packages;
        std::error_code ec;
        if (std::filesystem::exists(cacheDir, ec)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(cacheDir, ec)) {
                if (entry.is_directory()) {
                    packages.push_back(entry.path().filename().string());
                }
            }
            std::ranges::sort(packages);
        }
        if (packages.empty()) {
            if (!opts.quiet) {
                std::print("  Global cache is empty ({})\n", cacheDir.string());
            }
            return 0;
        }
        std::print("Global cache ({} package{} at {}):\n",
                   packages.size(),
                   packages.size() == 1 ? "" : "s",
                   cacheDir.string());
        for (const auto& pkg : packages) {
            std::print("  {}\n", pkg);
        }
        return 0;
    }

    const auto manifestPath = RequireManifest();
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }

    if (manifest->dependencies.empty()) {
        if (!opts.quiet) {
            std::print("  No dependencies.\n");
        }
        return 0;
    }

    std::print("Dependencies ({}):\n", manifest->dependencies.size());
    for (const auto& dep : manifest->dependencies) {
        if (!dep.path.empty()) {
            std::print("  {} (path: {})\n", dep.name, dep.path);
        }
        else {
            const std::string ver =
                dep.version.empty() ? "latest" : dep.version;
            std::print("  {} @ {}\n", dep.name, ver);
        }
    }
    return 0;
}

int Cli::RunNew(const std::span<const std::string_view> args,
                const GlobalOptions& opts) {
    std::string_view name;
    bool bin = false;
    bool lib = false;
    std::string_view customPath;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "--bin") {
            bin = true;
            continue;
        }
        if (arg == "--lib") {
            lib = true;
            continue;
        }
        if (arg == "--path" && i + 1 < args.size()) {
            customPath = args[++i];
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("new");
            return 0;
        }
        if (!arg.starts_with('-') && name.empty()) {
            name = arg;
            continue;
        }
        PrintUnknownOption(arg, "new");
        return 1;
    }
    if (name.empty()) {
        std::print(stderr, "error: missing package name\n\n");
        PrintHelpFor("new");
        return 1;
    }
    const auto type =
        (lib && !bin) ? PackageType::SharedLibrary : PackageType::Executable;
    std::filesystem::path root;
    if (!customPath.empty()) {
        root = std::filesystem::path(customPath) / name;
    }
    else {
        root = std::filesystem::current_path() / name;
    }
    if (!opts.quiet) {
        std::print("Creating {} package '{}'\n",
                   type == PackageType::Executable ? "binary" : "library",
                   std::string(name));
    }
    if (!ScaffoldPackage(root, std::string(name), type, /*initMode=*/false)) {
        return 1;
    }
    if (!opts.quiet) {
        std::print(
            "Created package '{}' at {}\n", std::string(name), root.string());
    }
    return 0;
}

int Cli::RunUpdate(std::span<const std::string_view> args,
                   const GlobalOptions& opts) {
    bool global = false;
    for (auto& arg : args) {
        if (arg == "--global") {
            global = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("update");
            return 0;
        }
        PrintUnknownOption(arg, "update");
        return 1;
    }

    if (global) {
        const auto cacheDir = RegistryPackagesDir();
        std::vector<std::filesystem::path> pkgDirs;
        std::error_code ec;
        if (std::filesystem::exists(cacheDir, ec)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(cacheDir, ec)) {
                if (entry.is_directory()) {
                    pkgDirs.push_back(entry.path());
                }
            }
        }
        if (pkgDirs.empty()) {
            if (!opts.quiet) {
                std::print("  No packages in global cache to update.\n");
            }
            return 0;
        }
        int updated = 0;
        for (const auto& pkgDir : pkgDirs) {
            const std::string pkgName = pkgDir.filename().string();
            if (!opts.quiet) {
                std::print("    Updating {}...\n", pkgName);
            }
            if (!GitPull(pkgDir)) {
                std::print(stderr, "error: failed to update '{}'\n", pkgName);
                return 1;
            }
            ++updated;
        }
        if (!opts.quiet) {
            std::print("     Summary: {} updated\n", updated);
        }
        return 0;
    }

    const auto manifestPath = RequireManifest();
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }

    std::vector<std::string> queue;
    std::unordered_set<std::string> queued;
    const std::string updateTarget = HostTargetTriple();
    for (const auto& dep : manifest->EffectiveDependencies(updateTarget)) {
        const std::string packageName = DependencyPackageName(dep);
        if (dep.path.empty() && !queued.count(packageName)) {
            queue.push_back(packageName);
            queued.insert(packageName);
        }
    }

    if (queue.empty()) {
        if (!opts.quiet) {
            std::print("  No registry dependencies to update.\n");
        }
        return 0;
    }

    if (!opts.quiet) {
        std::print("     Fetching registry...\n");
    }

    const auto jsonOpt = FetchUrl(std::string(kRegistryUrl));
    if (!jsonOpt) {
        std::print(stderr, "error: failed to fetch package registry\n");
        return 1;
    }

    int updated = 0;
    int installed = 0;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const std::string& pkgName = queue[i];
        const std::string repoUrl = JsonLookupString(*jsonOpt, pkgName);
        if (repoUrl.empty()) {
            std::print(
                stderr, "error: package '{}' not found in registry\n", pkgName);
            return 1;
        }
        const std::filesystem::path pkgDir = RegistryPackagesDir() / pkgName;
        std::error_code ec;
        std::filesystem::create_directories(pkgDir.parent_path(), ec);

        if (std::filesystem::exists(pkgDir)) {
            if (!opts.quiet) {
                std::print("    Updating {}...\n", pkgName);
            }
            if (!GitPull(pkgDir)) {
                std::print(stderr, "error: failed to update '{}'\n", pkgName);
                return 1;
            }
            ++updated;
        }
        else {
            if (!opts.quiet) {
                std::print("  Downloading {} from {}...\n", pkgName, repoUrl);
            }
            if (!GitClone(repoUrl, pkgDir, false)) {
                std::print(stderr, "error: failed to clone '{}'\n", repoUrl);
                return 1;
            }
            if (!opts.quiet) {
                std::print(
                    "    Installed {} at {}\n", pkgName, pkgDir.string());
            }
            ++installed;
        }

        // Enqueue registry deps declared by this package
        if (const auto depManifest = Manifest::Load(pkgDir / "Rux.toml")) {
            for (const auto& dep :
                 depManifest->EffectiveDependencies(updateTarget)) {
                const std::string depPackageName = DependencyPackageName(dep);
                if (dep.path.empty() && !queued.count(depPackageName)) {
                    queue.push_back(depPackageName);
                    queued.insert(depPackageName);
                }
            }
        }
    }
    if (!opts.quiet) {
        std::print("     Summary: {} updated, {} newly installed\n",
                   updated,
                   installed);
    }
    return 0;
}


// TODO: Make this look in the registry instead of installed packages
// TODO: Extend Package manifest metadata support
int Cli::RunInfo(std::span<const std::string_view> args,
                 const GlobalOptions& opts) {
    (void)opts;
    std::string_view packageName;

    bool jsonOutput = false;

    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("info");
            return 0;
        }

        if (arg == "--json") {
            jsonOutput = true;
            continue;
        }

        if (!arg.starts_with('-') && packageName.empty()) {
            packageName = arg;
            continue;
        }

        PrintUnknownOption(arg, "info");
        return 1;
    }

    if (packageName.empty()) {
        std::print(stderr, "error: missing package name\n");
        return 1;
    }

    const auto packageDir = RegistryPackagesDir() / std::string(packageName);
    const auto manifestPath = packageDir / "Rux.toml";

    if (!std::filesystem::exists(manifestPath)) {
        std::print(
            stderr, "error: package '{}' is not installed\n", packageName);
        return 1;
    }

    auto manifest = Manifest::Load(manifestPath);

    if (!manifest) {
        std::print(
            stderr, "error: failed to parse '{}'\n", manifestPath.string());
        return 1;
    }

    // not using nlohmann/json.hpp to keep compiler as small and fast as
    // possible
    if (jsonOutput) {
        std::print("{}\n", "{");
        std::print("  \"name\": \"{}\",\n", manifest->package.name);
        std::print("  \"version\": \"{}\",\n", manifest->package.version);
        std::print("  \"type\": \"{}\",\n", manifest->package.type);
        std::print("  \"dependencies\": [\n");

        for (size_t i = 0; i < manifest->dependencies.size(); ++i) {
            const auto& dep = manifest->dependencies[i];
            std::print("    {}", "{");
            std::print("\"name\": \"{}\"", dep.name);

            if (!dep.path.empty()) {
                std::print(", \"path\": \"{}\"", dep.path);
            }
            else {
                std::print(", \"version\": \"{}\"",
                           dep.version.empty() ? "*" : dep.version);
            }

            // Only add a comma if this isn't the last element in the vector
            if (i + 1 < manifest->dependencies.size()) {
                std::print("    {},\n", "}");
            }
            else {
                std::print("    {}\n", "}");
            }
        }

        std::print("  ]\n");
        std::print("{}\n", "}");
    }
    else {
        std::print("Name:     {}\n"
                   "Version:  {}\n"
                   "Type:     {}\n",
                   manifest->package.name,
                   manifest->package.version,
                   manifest->package.type);

        if (!manifest->dependencies.empty()) {
            std::print("\nDependencies:\n");

            for (const auto& dep : manifest->dependencies) {
                if (!dep.path.empty()) {
                    std::print("  - {} (path: {})\n", dep.name, dep.path);
                }
                else {
                    std::print("  - {} @ {}\n",
                               dep.name,
                               dep.version.empty() ? "*" : dep.version);
                }
            }
        }
    }


    return 0;
}
