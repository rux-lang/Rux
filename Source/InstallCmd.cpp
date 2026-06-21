#include "Rux/BuildReport.h"
#include "Rux/BuildTarget.h"
#include "Rux/Cli.h"
#include "Rux/Hir.h"
#include "Rux/Manifest.h"
#include "Rux/Platform.h"
#include "Rux/Process.h"
#include "Rux/Target.h"
#include "Rux/Terminal.h"
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
#else
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace Rux;
using namespace Platform;
using namespace Misc;

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

    const std::string installTarget = HostTargetTriple();

    // Seed the work queue: either the explicitly named package, or every
    // registry dependency declared by the current project's manifest. From
    // here both cases share the same transitive resolution loop below.
    if (!packageSpec.empty()) {
        auto [pkgName, pkgVersion] = ParsePackageSpec(packageSpec);
        queue.push_back(pkgName);
        queued.insert(pkgName);
    }
    else {
        const auto manifestPath = RequireManifest();
        if (!manifestPath) {
            return 1;
        }

        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            return 1;
        }

        for (const auto &dep : manifest->EffectiveDependencies(installTarget)) {
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
    // (e.g. installing Std also pulls in its platform packages).
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const std::string &pkgName = queue[i];

        const std::string repoUrl = JsonLookupString(*jsonOptInstall, pkgName);
        if (repoUrl.empty()) {
            std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
            return 1;
        }

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

            if (!GitClone(repoUrl, pkgDir, packageFromDev)) {
                std::print(stderr, "error: failed to clone '{}'\n", repoUrl);
                return 1;
            }

            if (!opts.quiet) {
                std::print("    Installed {} at {}\n", pkgName, pkgDir.string());
            }

            ++installed;
        }

        if (const auto depManifest = Manifest::Load(pkgDir / "Rux.toml")) {
            for (const auto &dep : depManifest->EffectiveDependencies(installTarget)) {
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
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("uninstall");
            return 0;
        }
        if (!arg.starts_with('-') && packageName.empty()) {
            packageName = arg;
            continue;
        }
        PrintUnknownOption(arg, "uninstall");
        return 1;
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

    const auto manifestPath = RequireManifest();
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }

    std::vector<std::string> toRemove;
    for (const auto &dep : manifest->EffectiveDependencies(HostTargetTriple())) {
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
