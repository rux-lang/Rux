// Package-manager commands: install, uninstall, add, remove, list, update, info.

#include "Cli/Cli.h"
#include "Driver/BuildTarget.h"
#include "Package/Manifest.h"
#include "System/Process.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Rux;
using namespace Driver;
using namespace System;

int Cli::RunInstall(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view packageSpec;
    bool packageFromDev = false;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("install");
            return 0;
        }
        if (arg == "--dev") {
            packageFromDev = true;
            continue;
        }
        if (!arg.starts_with('-') && packageSpec.empty()) {
            packageSpec = arg;
            continue;
        }
        PrintUnknownOption(arg, "install");
        return 1;
    }
    std::vector<std::string> queue;
    std::unordered_set<std::string> queued;
    // Seed the work queue: either the explicitly named package, or every
    // registry dependency declared by the current project's manifest. From
    // here both cases share the same transitive resolution loop below.
    if (!packageSpec.empty()) {
        auto [pkgName, pkgVersion] = ParsePackageSpec(packageSpec);
        queue.push_back(pkgName);
        queued.insert(pkgName);
    }
    else {
        const auto manifestPath = RequireManifest(opts.manifest);
        if (!manifestPath) {
            return 1;
        }
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            return 1;
        }
        for (const auto &dep : manifest->dependencies) {
            if (const std::string packageName = DependencyPackageName(dep);
                dep.path.empty() && !queued.contains(packageName)) {
                queue.push_back(packageName);
                queued.insert(packageName);
            }
        }
        if (queue.empty()) {
            if (!opts.quiet) {
                std::print("  No registry dependencies to install.\n");
            }
            return 0;
        }
    }
    if (!opts.quiet) {
        std::print("     Fetching registry...\n");
    }
    const auto jsonOptInstall = FetchUrl(std::string(kRegistryUrl));
    if (!jsonOptInstall) {
        std::print(stderr, "error: failed to fetch package registry\n");
        return 1;
    }
    int installed = 0;
    int upToDate = 0;
    // Breadth-first install: each downloaded package's own dependencies are
    // appended to the queue, so the whole transitive graph is resolved
    // (e.g. installing Io also pulls in its platform packages).
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const std::string &pkgName = queue[i];
        const std::string repoUrl = JsonFindPackageRepository(*jsonOptInstall, pkgName);
        if (repoUrl.empty()) {
            std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
            return 1;
        }
        // A non-empty folder means the package lives in a subdirectory of a
        // monorepo, so only that subdirectory is installed rather than the whole
        // repository.
        const std::string folder = JsonFindPackageField(*jsonOptInstall, pkgName, "folder");
        const std::filesystem::path pkgDir = RegistryPackagesDir() / pkgName;
        std::error_code ec;
        create_directories(pkgDir.parent_path(), ec);
        if (exists(pkgDir)) {
            if (!opts.quiet) {
                std::print("   Up-to-date {}\n", pkgName);
            }
            ++upToDate;
        }
        else {
            if (!opts.quiet) {
                std::print("  Downloading {} from {}...\n", pkgName, repoUrl);
            }
            const bool cloned = folder.empty() ? GitClone(repoUrl, pkgDir, packageFromDev)
                                               : GitCloneSubdir(repoUrl, folder, pkgDir, packageFromDev);
            if (!cloned) {
                std::print(stderr, "error: failed to clone '{}'\n", repoUrl);
                return 1;
            }
            if (!opts.quiet) {
                std::print("    Installed {} at {}\n", pkgName, pkgDir.string());
            }
            ++installed;
        }
        if (const auto depManifest = Manifest::Load(pkgDir / "Rux.toml")) {
            for (const auto &dep : depManifest->dependencies) {
                if (const std::string depPackageName = DependencyPackageName(dep);
                    dep.path.empty() && !queued.contains(depPackageName)) {
                    queue.push_back(depPackageName);
                    queued.insert(depPackageName);
                }
            }
        }
    }
    if (!opts.quiet) {
        std::print("     Summary: {} installed, {} already up-to-date\n", installed, upToDate);
    }
    return 0;
}

int Cli::RunUninstall(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view packageName;
    bool global = false;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("uninstall");
            return 0;
        }
        if (arg == "--global") {
            global = true;
            continue;
        }
        if (!arg.starts_with('-') && packageName.empty()) {
            packageName = arg;
            continue;
        }
        PrintUnknownOption(arg, "uninstall");
        return 1;
    }
    if (global && !packageName.empty()) {
        std::print(stderr, "error: '--global' cannot be combined with a package name\n");
        return 1;
    }

    if (global) {
        const auto cacheDir = RegistryPackagesDir();
        std::vector<std::filesystem::path> pkgDirs;
        std::error_code ec;
        if (std::filesystem::exists(cacheDir, ec)) {
            for (const auto &entry : std::filesystem::directory_iterator(cacheDir, ec)) {
                if (entry.is_directory()) {
                    pkgDirs.push_back(entry.path());
                }
            }
        }
        if (pkgDirs.empty()) {
            if (!opts.quiet) {
                std::print("  Global cache is empty ({})\n", cacheDir.string());
            }
            return 0;
        }
        std::ranges::sort(pkgDirs);
        int removed = 0;
        for (const auto &pkgDir : pkgDirs) {
            const std::string pkgName = pkgDir.filename().string();
            std::filesystem::remove_all(pkgDir, ec);
            if (ec) {
                std::print(stderr, "error: failed to remove '{}': {}\n", pkgDir.string(), ec.message());
                return 1;
            }
            if (!opts.quiet) {
                std::print("   Uninstalled {}\n", pkgName);
            }
            ++removed;
        }
        if (!opts.quiet) {
            std::print("     Summary: {} uninstalled\n", removed);
        }
        return 0;
    }

    if (!packageName.empty()) {
        const std::filesystem::path pkgDir = RegistryPackagesDir() / std::string(packageName);
        if (!std::filesystem::exists(pkgDir)) {
            std::print(stderr, "error: package '{}' is not installed\n", packageName);
            return 1;
        }
        std::error_code ec;
        std::filesystem::remove_all(pkgDir, ec);
        if (ec) {
            std::print(stderr, "error: failed to remove '{}': {}\n", pkgDir.string(), ec.message());
            return 1;
        }
        if (!opts.quiet) {
            std::print("   Uninstalled {}\n", packageName);
        }
        return 0;
    }
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::vector<std::string> toRemove;
    for (const auto &dep : manifest->dependencies) {
        if (dep.path.empty()) {
            toRemove.push_back(DependencyPackageName(dep));
        }
    }
    if (toRemove.empty()) {
        if (!opts.quiet) {
            std::print("  No registry dependencies to uninstall.\n");
        }
        return 0;
    }
    int removed = 0;
    int notFound = 0;
    for (const auto &pkgName : toRemove) {
        const std::filesystem::path pkgDir = RegistryPackagesDir() / pkgName;
        if (!std::filesystem::exists(pkgDir)) {
            if (!opts.quiet) {
                std::print("  Not installed {}\n", pkgName);
            }
            ++notFound;
            continue;
        }
        std::error_code ec;
        std::filesystem::remove_all(pkgDir, ec);
        if (ec) {
            std::print(stderr, "error: failed to remove '{}': {}\n", pkgDir.string(), ec.message());
            return 1;
        }
        if (!opts.quiet) {
            std::print("   Uninstalled {}\n", pkgName);
        }
        ++removed;
    }
    if (!opts.quiet) {
        std::print("     Summary: {} uninstalled, {} not installed\n", removed, notFound);
    }
    return 0;
}

int Cli::RunAdd(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view spec;
    std::string_view pathArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("add");
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
        PrintHelpFor("add");
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
    auto [pkgName, pkgVersion] = ParsePackageSpec(spec);

    if (!pathArg.empty()) {
        const bool changed = manifest->AddPathDependency(pkgName, std::string(pathArg));
        if (!manifest->Save(*manifestPath)) {
            std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
            return 1;
        }
        if (!opts.quiet) {
            if (changed) {
                std::print("Added {} @ path '{}'\n", pkgName, pathArg);
            }
            else {
                std::print("Up-to-date {} @ path '{}'\n", pkgName, pathArg);
            }
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
    if (JsonFindPackageRepository(*jsonOpt, pkgName).empty()) {
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
        if (changed) {
            std::print("Added {} @ {}\n", pkgName, ver);
        }
        else {
            std::print("Up-to-date {} @ {}\n", pkgName, ver);
        }
    }
    return 0;
}

int Cli::RunRemove(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view name;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("remove");
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
        PrintHelpFor("remove");
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
    std::string pkgName(name);
    if (!manifest->RemoveDependency(pkgName)) {
        std::print(stderr, "error: package '{}' is not a dependency\n", pkgName);
        return 1;
    }
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        std::print("     Removed {}\n", pkgName);
    }
    return 0;
}

int Cli::RunList(std::span<const std::string_view> args, const GlobalOptions &opts) {
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
            for (const auto &entry : std::filesystem::directory_iterator(cacheDir, ec)) {
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
        std::print("Global cache ({} package{} at {}):\n", packages.size(), packages.size() == 1 ? "" : "s",
                   cacheDir.string());
        for (const auto &pkg : packages) {
            std::print("  {}\n", pkg);
        }
        return 0;
    }
    const auto manifestPath = RequireManifest(opts.manifest);
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
    for (const auto &dep : manifest->dependencies) {
        if (!dep.path.empty()) {
            std::print("  {} (path: {})\n", dep.name, dep.path);
        }
        else {
            const std::string ver = dep.version.empty() ? "latest" : dep.version;
            std::print("  {} @ {}\n", dep.name, ver);
        }
    }
    return 0;
}

int Cli::RunUpdate(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool global = false;
    for (auto &arg : args) {
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
            for (const auto &entry : std::filesystem::directory_iterator(cacheDir, ec)) {
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
        for (const auto &pkgDir : pkgDirs) {
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
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::vector<std::string> queue;
    std::unordered_set<std::string> queued;
    for (const auto &dep : manifest->dependencies) {
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
        const std::string &pkgName = queue[i];
        const std::string repoUrl = JsonFindPackageRepository(*jsonOpt, pkgName);
        if (repoUrl.empty()) {
            std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
            return 1;
        }
        // A non-empty folder means the package lives in a subdirectory of a
        // monorepo; those are installed by copying files (no .git to pull), so
        // updating re-fetches the subdirectory instead.
        const std::string folder = JsonFindPackageField(*jsonOpt, pkgName, "folder");
        const std::filesystem::path pkgDir = RegistryPackagesDir() / pkgName;
        std::error_code ec;
        std::filesystem::create_directories(pkgDir.parent_path(), ec);

        if (std::filesystem::exists(pkgDir)) {
            if (!opts.quiet) {
                std::print("    Updating {}...\n", pkgName);
            }
            bool refreshed;
            if (folder.empty()) {
                refreshed = GitPull(pkgDir);
            }
            else {
                std::filesystem::remove_all(pkgDir, ec);
                refreshed = GitCloneSubdir(repoUrl, folder, pkgDir, false);
            }
            if (!refreshed) {
                std::print(stderr, "error: failed to update '{}'\n", pkgName);
                return 1;
            }
            ++updated;
        }
        else {
            if (!opts.quiet) {
                std::print("  Downloading {} from {}...\n", pkgName, repoUrl);
            }
            const bool cloned =
                folder.empty() ? GitClone(repoUrl, pkgDir, false) : GitCloneSubdir(repoUrl, folder, pkgDir, false);
            if (!cloned) {
                std::print(stderr, "error: failed to clone '{}'\n", repoUrl);
                return 1;
            }
            if (!opts.quiet) {
                std::print("    Installed {} at {}\n", pkgName, pkgDir.string());
            }
            ++installed;
        }
        // Enqueue registry deps declared by this package
        if (const auto depManifest = Manifest::Load(pkgDir / "Rux.toml")) {
            for (const auto &dep : depManifest->dependencies) {
                const std::string depPackageName = DependencyPackageName(dep);
                if (dep.path.empty() && !queued.count(depPackageName)) {
                    queue.push_back(depPackageName);
                    queued.insert(depPackageName);
                }
            }
        }
    }
    if (!opts.quiet) {
        std::print("     Summary: {} updated, {} newly installed\n", updated, installed);
    }
    return 0;
}

// TODO: Make this look in the registry instead of installed packages
// TODO: Extend Package manifest metadata support
int Cli::RunInfo(std::span<const std::string_view> args, const GlobalOptions &opts) {
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
    std::filesystem::path manifestPath;
    if (!opts.manifest.empty()) {
        manifestPath = opts.manifest;
        if (!std::filesystem::exists(manifestPath)) {
            std::print(stderr, "error: specified manifest '{}' not found\n", manifestPath.string());
            return 1;
        }
    }
    else if (packageName.empty()) {
        auto localManifestOpt = Manifest::Find(std::filesystem::current_path());
        if (!localManifestOpt) {
            std::print(stderr, "error: missing package name, and no Rux.toml found in current directory\n");
            return 1;
        }
        manifestPath = *localManifestOpt;
    }
    else {
        const auto packageDir = RegistryPackagesDir() / std::string(packageName);
        manifestPath = packageDir / "Rux.toml";

        if (!std::filesystem::exists(manifestPath)) {
            std::print(stderr, "error: package '{}' is not installed\n", packageName);
            return 1;
        }
    }
    auto manifest = Manifest::Load(manifestPath);
    if (!manifest) {
        std::print(stderr, "error: failed to parse '{}'\n", manifestPath.string());
        return 1;
    }
    // not using nlohmann/json.hpp to keep compiler as small and fast as
    // possible
    if (jsonOutput) {
        std::print("{}\n", "{");
        std::print("  \"name\": \"{}\",\n", manifest->package.name);
        std::print("  \"version\": \"{}\",\n", manifest->package.version);
        std::print("  \"type\": \"{}\",\n", manifest->package.type);
        if (!manifest->package.description.empty()) {
            std::print("  \"description\": \"{}\",\n", manifest->package.description);
        }
        if (!manifest->package.authors.empty()) {
            std::print("  \"authors\": \"{}\",\n", manifest->package.authors);
        }
        if (!manifest->package.license.empty()) {
            std::print("  \"license\": \"{}\",\n", manifest->package.license);
        }
        if (!manifest->package.repository.empty()) {
            std::print("  \"repository\": \"{}\",\n", manifest->package.repository);
        }
        if (!manifest->package.homepage.empty()) {
            std::print("  \"homepage\": \"{}\",\n", manifest->package.homepage);
        }
        std::print("  \"dependencies\": [\n");
        for (size_t i = 0; i < manifest->dependencies.size(); ++i) {
            const auto &dep = manifest->dependencies[i];
            std::print("    {}", "{");
            std::print("\"name\": \"{}\"", dep.name);

            if (!dep.path.empty()) {
                std::print(", \"path\": \"{}\"", dep.path);
            }
            else {
                std::print(", \"version\": \"{}\"", dep.version.empty() ? "*" : dep.version);
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
        std::print("Name:        {}\n"
                   "Version:     {}\n"
                   "Type:        {}\n",
                   manifest->package.name, manifest->package.version, manifest->package.type);
        if (!manifest->package.description.empty()) {
            std::print("Description: {}\n", manifest->package.description);
        }
        if (!manifest->package.authors.empty()) {
            std::print("Authors:     {}\n", manifest->package.authors);
        }
        if (!manifest->package.license.empty()) {
            std::print("License:     {}\n", manifest->package.license);
        }
        if (!manifest->package.repository.empty()) {
            std::print("Repository:  {}\n", manifest->package.repository);
        }
        if (!manifest->package.homepage.empty()) {
            std::print("Homepage:    {}\n", manifest->package.homepage);
        }
        if (!manifest->dependencies.empty()) {
            std::print("\nDependencies:\n");
            for (const auto &dep : manifest->dependencies) {
                if (!dep.path.empty()) {
                    std::print("  - {} (path: {})\n", dep.name, dep.path);
                }
                else {
                    std::print("  - {} @ {}\n", dep.name, dep.version.empty() ? "*" : dep.version);
                }
            }
        }
    }
    return 0;
}
