// Package installation commands: install and update.
//
// Both commands resolve a complete registry dependency graph before fetching
// artifacts, then verify and stage every selected package through the same
// cache installation path.

#include "Cli/Cli.h"
#include "Cli/TerminalStyle.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Driver/Credentials.h"
#include "Driver/PackageResolution.h"
#include "Driver/Registry.h"
#include "Package/Artifact.h"
#include "Package/Checksum.h"
#include "Package/Manifest.h"
#include "Reporting/Reporting.h"
#include "System/Process.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Driver;
using namespace System;
using namespace CliSupport;

namespace {
enum class InstallCommand {
    Install,
    Update,
};

struct InstallOptions {
    std::string_view packageSpec;
    std::string_view registry;
    std::string_view target;
    bool global = false;
};

/// Parse the option surface shared by install and update.
std::optional<InstallOptions> ParseInstallOptions(const std::span<const std::string_view> args,
                                                  const InstallCommand command) {
    InstallOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--global" && command == InstallCommand::Update) {
            options.global = true;
            continue;
        }
        if (arg == "--registry" || arg == "--target") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '{}' requires an argument\n", arg);
                return std::nullopt;
            }
            (arg == "--registry" ? options.registry : options.target) = args[++i];
            continue;
        }
        if (command == InstallCommand::Install && !arg.starts_with('-') && options.packageSpec.empty()) {
            options.packageSpec = arg;
            continue;
        }

        const std::string_view commandName = command == InstallCommand::Install ? "install" : "update";
        std::print(stderr, "error: unknown option '{}' for command '{}'\n", arg, commandName);
        return std::nullopt;
    }
    return options;
}

std::optional<Target::TargetTriple> ResolvePackageTarget(const std::string_view requested) {
    const auto target =
        requested.empty() ? std::optional{Target::TargetTriple::Host()} : Target::TargetTriple::Parse(requested);
    if (!target) {
        std::print(stderr, "error: unsupported target '{}'; supported targets are {}\n", requested,
                   SupportedTargetTriples());
        return std::nullopt;
    }
    return target;
}

/// Turn a command-line package spec into one registry requirement.
std::optional<PackageRequirement> RequirementFromSpec(const std::string_view spec) {
    const auto parsed = ParsePackageSpec(spec);
    if (!parsed) {
        std::print(stderr, "error: {}\n", parsed.error());
        return std::nullopt;
    }
    if (!parsed->ns) {
        std::print(stderr, "error: a registry package needs a namespace; write 'rux install Namespace/{}'\n",
                   parsed->name.Text());
        return std::nullopt;
    }
    return PackageRequirement{
        .ns = *parsed->ns, .package = parsed->name, .range = parsed->version.value_or(*VersionRange::Parse("*"))};
}

/**
 * @brief Seed the resolver from the project the command was run in.
 *
 * Mirrors what `rux build` would resolve: a package manifest contributes its own
 * dependencies, a workspace manifest contributes every member's, and a
 * workspace without a root manifest contributes every discovered member's.
 */
std::optional<std::vector<PackageRequirement>> SeedFromProject(const GlobalOptions &opts,
                                                               const Target::TargetTriple target) {
    std::vector<PackageRequirement> seeds;
    std::optional<std::filesystem::path> manifestPath;
    if (!opts.manifest.empty()) {
        manifestPath = RequireManifest(opts.manifest);
        if (!manifestPath) {
            return std::nullopt;
        }
    }
    else {
        manifestPath = Manifest::Find();
    }

    if (!manifestPath) {
        const auto workspaceManifests = DiscoverManifestlessWorkspaceManifests();
        if (workspaceManifests.empty()) {
            static_cast<void>(RequireManifest());
            return std::nullopt;
        }
        if (!opts.quiet) {
            std::print("Installing workspace\n");
        }
        for (const auto &memberPath : workspaceManifests) {
            auto memberManifest = LoadManifest(memberPath);
            if (!memberManifest) {
                return std::nullopt;
            }
            if (memberManifest->IsWorkspace() || memberManifest->package.name.Empty()) {
                std::print(stderr, "error: workspace member '{}' is not a package\n",
                           memberPath.parent_path().string());
                return std::nullopt;
            }
            auto requirements = CollectPackageRequirements(*memberManifest, target);
            seeds.insert(seeds.end(), requirements.begin(), requirements.end());
        }
        return seeds;
    }

    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return std::nullopt;
    }
    if (!manifest->IsWorkspace()) {
        return CollectPackageRequirements(*manifest, target);
    }

    if (!opts.quiet) {
        std::print("Installing workspace\n");
    }
    const std::filesystem::path workspaceRoot = manifestPath->parent_path();
    for (const auto &member : manifest->workspace.packages) {
        const auto memberPath = (workspaceRoot / member / "Rux.toml").lexically_normal();
        std::error_code ec;
        if (!std::filesystem::exists(memberPath, ec)) {
            std::print(stderr, "error: workspace member '{}' has no Rux.toml\n", member);
            return std::nullopt;
        }
        auto memberManifest = LoadManifest(memberPath);
        if (!memberManifest) {
            return std::nullopt;
        }
        if (memberManifest->IsWorkspace() || memberManifest->package.name.Empty()) {
            std::print(stderr, "error: workspace member '{}' is not a package\n", member);
            return std::nullopt;
        }
        auto requirements = CollectPackageRequirements(*memberManifest, target);
        seeds.insert(seeds.end(), requirements.begin(), requirements.end());
    }
    return seeds;
}

/// Every valid `<namespace>/<name>` pair the cache holds, in normalized order.
std::vector<std::pair<IdentitySegment, IdentitySegment>> CachedPackages() {
    std::vector<std::pair<IdentitySegment, IdentitySegment>> packages;
    const std::filesystem::path cacheDir = RegistryPackagesDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(cacheDir, ec)) {
        return packages;
    }
    for (const auto &namespaceDir : std::filesystem::directory_iterator(cacheDir, ec)) {
        if (!namespaceDir.is_directory(ec)) {
            continue;
        }
        auto ns = IdentitySegment::Parse(namespaceDir.path().filename().string());
        if (!ns) {
            continue;
        }
        for (const auto &packageDir : std::filesystem::directory_iterator(namespaceDir.path(), ec)) {
            if (!packageDir.is_directory(ec)) {
                continue;
            }
            auto name = IdentitySegment::Parse(packageDir.path().filename().string());
            if (name) {
                packages.emplace_back(*ns, *name);
            }
        }
    }
    std::ranges::sort(packages, [](const auto &left, const auto &right) {
        return left.first == right.first ? left.second < right.second : left.first < right.first;
    });
    return packages;
}

/// Remove cache entries left by the pre-registry flat layout before resolution.
void RemoveLegacyCacheEntries(const GlobalOptions &opts) {
    const std::filesystem::path cacheDir = RegistryPackagesDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(cacheDir, ec)) {
        return;
    }
    for (const auto &entry : std::filesystem::directory_iterator(cacheDir, ec)) {
        if (!entry.is_directory(ec) || !std::filesystem::exists(entry.path() / "Rux.toml", ec)) {
            continue;
        }
        std::error_code removeError;
        std::filesystem::remove_all(entry.path(), removeError);
        if (!removeError && !opts.quiet) {
            std::print("Removed legacy cache entry {}\n", entry.path().filename().string());
        }
    }
}

bool CacheEntryMatches(const std::filesystem::path &root, const ResolvedPackage &resolution) {
    const auto loaded = Manifest::Load(root / "Rux.toml");
    if (!loaded.Ok() || loaded.manifest->IsWorkspace()) {
        return false;
    }
    const Package &package = loaded.manifest->package;
    return package.ns && *package.ns == resolution.ns && package.name == resolution.package &&
           package.version.Text() == resolution.version.Text();
}

struct InstallTally {
    int installed = 0;
    int upToDate = 0;
};

/**
 * @brief Download, verify, stage, and atomically install every selected package.
 *
 * The checksum is fetched before the artifact. Nothing reaches its final cache
 * directory until the archive and embedded manifest have both been verified.
 */
bool InstallResolved(const PackageResolver &resolver, const std::vector<ResolvedPackage> &resolved,
                     const GlobalOptions &opts, InstallTally &tally) {
    for (const auto &resolution : resolved) {
        const std::string identity = QualifiedIdentity(resolution.ns, resolution.package);
        const std::filesystem::path packageDir =
            RegistryPackageDir(resolution.ns, resolution.package, resolution.version);

        if (CacheEntryMatches(packageDir, resolution)) {
            if (!opts.quiet) {
                std::print("Up-to-date {} {}\n", identity, resolution.version.Text());
            }
            ++tally.upToDate;
            continue;
        }

        if (!opts.quiet) {
            std::print("Downloading {} {}\n", identity, resolution.version.Text());
        }
        auto digest = FetchArtifactChecksum(resolver.Base(), resolution.ns, resolution.package, resolution.version);
        if (!digest) {
            std::print(stderr, "error: {}\n", Describe(digest.error(), resolver.Base(), identity));
            return false;
        }
        auto archive = DownloadArtifact(resolver.Base(), resolution.ns, resolution.package, resolution.version);
        if (!archive) {
            std::print(stderr, "error: {}\n", Describe(archive.error(), resolver.Base(), identity));
            return false;
        }
        if (const std::string actual = Sha256Hex(*archive); !DigestsEqual(actual, *digest)) {
            std::print(stderr, "error: the archive for {} {} does not match the checksum {} published for it\n",
                       identity, resolution.version.Text(), *digest);
            return false;
        }

        std::filesystem::path staging = packageDir;
        staging += ".download";
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        std::filesystem::create_directories(packageDir.parent_path(), ec);
        if (ec) {
            std::print(stderr, "error: failed to create '{}'\n", packageDir.parent_path().generic_string());
            return false;
        }

        auto extracted = ExtractPackageArtifact(*archive, staging);
        if (!extracted) {
            std::filesystem::remove_all(staging, ec);
            std::print(stderr, "error: the archive for {} {} was rejected: {}\n", identity, resolution.version.Text(),
                       extracted.error());
            return false;
        }

        const auto staged = Manifest::Load(staging / "Rux.toml");
        if (!staged.Ok() || staged.manifest->IsWorkspace() || !staged.manifest->package.ns ||
            *staged.manifest->package.ns != resolution.ns || staged.manifest->package.name != resolution.package ||
            staged.manifest->package.version.Text() != resolution.version.Text()) {
            std::filesystem::remove_all(staging, ec);
            std::print(stderr, "error: the archive published as {} {} contains a different package\n", identity,
                       resolution.version.Text());
            return false;
        }

        if (!CommitDownloadedPackage(staging, packageDir)) {
            std::filesystem::remove_all(staging, ec);
            std::print(stderr, "error: failed to install {} into '{}'\n", identity, packageDir.generic_string());
            return false;
        }
        if (!opts.quiet) {
            std::print("Installed {} {} ({} files)\n", identity, resolution.version.Text(), extracted->fileCount);
        }
        ++tally.installed;
    }
    return true;
}

void ReportResolutionFailure(const ResolutionFailure &failure) {
    std::print(stderr, "error: {}\n", failure.message);
    for (const auto &detail : failure.details) {
        std::print(stderr, "{}{}\n", errorContinuation, detail);
    }
}

int ResolveAndInstall(const InstallCommand command, const std::vector<PackageRequirement> &seeds,
                      const Target::TargetTriple target, const std::string_view registryArg, const GlobalOptions &opts,
                      const std::chrono::steady_clock::time_point started = {}) {
    RemoveLegacyCacheEntries(opts);
    const std::string registry = ResolveRegistryBase(registryArg);
    if (!opts.quiet) {
        std::print("Resolving from {}\n", registry);
    }
    PackageResolver resolver(registry);
    const auto resolved = resolver.Resolve(seeds, target);
    if (!resolved) {
        ReportResolutionFailure(resolved.error());
        return 1;
    }

    InstallTally tally;
    if (!InstallResolved(resolver, *resolved, opts, tally)) {
        return 1;
    }
    if (!opts.quiet) {
        if (command == InstallCommand::Install) {
            std::print("Summary: {} installed, {} already up-to-date in {}\n", tally.installed, tally.upToDate,
                       Reporting::FormatDuration(ElapsedMs(started)));
        }
        else {
            std::print("Summary: {} newly installed, {} already current\n", tally.installed, tally.upToDate);
        }
    }
    return 0;
}
} // namespace

int Cli::RunInstall(const std::span<const std::string_view> args, const GlobalOptions &opts) {
    const auto options = ParseInstallOptions(args, InstallCommand::Install);
    if (!options) {
        return 1;
    }

    const auto installStart = std::chrono::steady_clock::now();
    const auto target = ResolvePackageTarget(options->target);
    if (!target) {
        return 1;
    }

    std::vector<PackageRequirement> seeds;
    if (!options->packageSpec.empty()) {
        auto requirement = RequirementFromSpec(options->packageSpec);
        if (!requirement) {
            return 1;
        }
        seeds.push_back(std::move(*requirement));
    }
    else {
        auto seeded = SeedFromProject(opts, *target);
        if (!seeded) {
            return 1;
        }
        seeds = std::move(*seeded);
        if (seeds.empty()) {
            if (!opts.quiet) {
                std::print("  No registry dependencies to install.\n");
            }
            return 0;
        }
    }

    return ResolveAndInstall(InstallCommand::Install, seeds, *target, options->registry, opts, installStart);
}

int Cli::RunUpdate(const std::span<const std::string_view> args, const GlobalOptions &opts) {
    const auto options = ParseInstallOptions(args, InstallCommand::Update);
    if (!options) {
        return 1;
    }

    const auto target = ResolvePackageTarget(options->target);
    if (!target) {
        return 1;
    }

    std::vector<PackageRequirement> seeds;
    if (options->global) {
        const VersionRange any = *VersionRange::Parse("*");
        for (const auto &[ns, name] : CachedPackages()) {
            seeds.push_back(PackageRequirement{.ns = ns, .package = name, .range = any});
        }
        if (seeds.empty()) {
            if (!opts.quiet) {
                std::print("  No packages in global cache to update.\n");
            }
            return 0;
        }
    }
    else {
        auto seeded = SeedFromProject(opts, *target);
        if (!seeded) {
            return 1;
        }
        seeds = std::move(*seeded);
        if (seeds.empty()) {
            if (!opts.quiet) {
                std::print("  No registry dependencies to update.\n");
            }
            return 0;
        }
    }

    return ResolveAndInstall(InstallCommand::Update, seeds, *target, options->registry, opts);
}
