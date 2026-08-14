// Package-manager commands: install, uninstall, add, remove, list, update, info.
//
// Every command that reaches the network goes through the versioned registry
// API in Driver/Registry: the resolver index chooses versions, and the download
// route supplies the published .ruxpkg. Installed packages are cached per exact
// version, so one host can hold several versions of the same package and a
// build picks the one its manifest asks for without contacting the registry.

#include "Cli/Cli.h"
#include "Cli/TerminalStyle.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Driver/Credentials.h"
#include "Driver/PackageResolution.h"
#include "Driver/Registry.h"
#include "Package/Artifact.h"
#include "Package/Checksum.h"
#include "Package/Manifest.h"
#include "System/Process.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
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
std::string Qualified(const IdentitySegment &ns, const IdentitySegment &package) {
    return QualifiedIdentity(ns, package);
}

std::string TargetSuffix(const ManifestDependency &dependency) {
    if (dependency.targetOS.empty()) {
        return {};
    }
    std::string suffix = " [targets: ";
    for (std::size_t i = 0; i < dependency.targetOS.size(); ++i) {
        suffix += (i == 0 ? "" : ", ");
        suffix += ManifestTargetOSName(dependency.targetOS[i]);
    }
    return suffix + ']';
}

/// Whether the manifest in a cache directory really is the package it claims.
bool CacheEntryMatches(const std::filesystem::path &root, const ResolvedPackage &resolution) {
    const auto loaded = Manifest::Load(root / "Rux.toml");
    if (!loaded.Ok() || loaded.manifest->IsWorkspace()) {
        return false;
    }
    const Package &package = loaded.manifest->package;
    return package.ns && *package.ns == resolution.ns && package.name == resolution.package &&
           package.version.Text() == resolution.version.Text();
}

/// Outcome counters shared by the install and update reports.
struct InstallTally {
    int installed = 0;
    int upToDate = 0;
};

/**
 * @brief Download, verify and unpack everything the resolver selected.
 *
 * The digest is fetched before the artifact so a registry that cannot vouch for
 * the bytes fails before they are transferred. Nothing reaches the cache
 * directory until the archive has matched its digest and passed the artifact
 * contract, and the swap into place is atomic.
 */
bool InstallResolved(const PackageResolver &resolver, const std::vector<ResolvedPackage> &resolved,
                     const GlobalOptions &opts, InstallTally &tally) {
    for (const auto &resolution : resolved) {
        const std::string identity = Qualified(resolution.ns, resolution.package);
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

/**
 * @brief Remove cache entries left by the pre-registry flat layout.
 *
 * That layout stored one unversioned directory per bare package name, so its
 * manifest sits directly below the cache root. Nothing reads those directories
 * any more, and leaving them would only clutter `rux list --global`.
 *
 * Callers must run this before resolving, not merely early: the intrinsics
 * package used to be named `Rux`, so `<cache>/Rux` can be one of these flat
 * entries rather than a namespace directory, and a package lookup matching
 * directory names would otherwise adopt it and install inside it.
 */
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

/**
 * @brief Seed the resolver from the project the command was run in.
 *
 * Mirrors what `rux build` would resolve: a package manifest contributes its own
 * dependencies, a workspace manifest contributes every member's, and a
 * workspace without a root manifest contributes every discovered member's.
 *
 * @return The requirements, or nullopt after the reason has been printed
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
            static_cast<void>(RequireManifest()); // Print the standard missing-manifest error.
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
        seeds = CollectPackageRequirements(*manifest, target);
        return seeds;
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

/// A requirement read off the command line, and whether it was written down.
struct SpecRequirement {
    PackageRequirement requirement;

    /// False when the spec named no `@requirement` and the wildcard stood in.
    /// `uninstall` distinguishes the two: naming a package removes every
    /// installed version, while naming a requirement removes only its matches.
    bool explicitRange = false;
};

/// Turn a command-line spec into one requirement, or explain why it cannot be.
std::optional<SpecRequirement> RequirementFromSpec(const std::string_view spec, const std::string_view command) {
    const auto parsed = ParsePackageSpec(spec);
    if (!parsed) {
        std::print(stderr, "error: {}\n", parsed.error());
        return std::nullopt;
    }
    if (!parsed->ns) {
        std::print(stderr, "error: a registry package needs a namespace; write 'rux {} Namespace/{}'\n", command,
                   parsed->name.Text());
        return std::nullopt;
    }
    // An omitted requirement accepts any stable release, matching `rux add`.
    return SpecRequirement{.requirement = {.ns = *parsed->ns,
                                           .package = parsed->name,
                                           .range = parsed->version.value_or(*VersionRange::Parse("*"))},
                           .explicitRange = parsed->version.has_value()};
}

/// Every `<namespace>/<name>` pair the cache holds, in normalized order, each
/// carrying the spelling its directory uses. Unlike the lookup in BuildTarget,
/// this parses rather than normalizes: enumeration must not turn a stray
/// directory name into an identity, while a lookup has to find the directory
/// whichever way it was spelled.
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
            if (!name) {
                continue;
            }
            packages.emplace_back(*ns, *name);
        }
    }
    std::ranges::sort(packages, [](const auto &left, const auto &right) {
        return left.first == right.first ? left.second < right.second : left.first < right.first;
    });
    return packages;
}

/// Read `--registry <url>`, leaving `index` on its value. Returns false on a
/// missing operand, which every caller reports the same way.
bool ReadRegistryOption(const std::span<const std::string_view> args, std::size_t &index,
                        std::string_view &registryArg) {
    if (index + 1 >= args.size()) {
        std::print(stderr, "error: '--registry' requires an argument\n");
        return false;
    }
    registryArg = args[++index];
    return true;
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

void ReportResolutionFailure(const ResolutionFailure &failure) {
    std::print(stderr, "error: {}\n", failure.message);
    for (const auto &detail : failure.details) {
        std::print(stderr, "{}{}\n", errorContinuation, detail);
    }
}
} // namespace

int Cli::RunInstall(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view packageSpec;
    std::string_view registryArg;
    std::string_view targetArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("install");
            return 0;
        }
        if (arg == "--registry") {
            if (!ReadRegistryOption(args, i, registryArg)) {
                return 1;
            }
            continue;
        }
        if (arg == "--target") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--target' requires an argument\n");
                return 1;
            }
            targetArg = args[++i];
            continue;
        }
        if (!arg.starts_with('-') && packageSpec.empty()) {
            packageSpec = arg;
            continue;
        }
        PrintUnknownOption(arg, "install");
        return 1;
    }

    // Timed from here rather than from entry, so the reported span is the work
    // the network and disk did, not the argument parsing above it.
    const auto installStart = std::chrono::steady_clock::now();
    const auto target = ResolvePackageTarget(targetArg);
    if (!target) {
        return 1;
    }

    std::vector<PackageRequirement> seeds;
    if (!packageSpec.empty()) {
        auto spec = RequirementFromSpec(packageSpec, "install");
        if (!spec) {
            return 1;
        }
        seeds.push_back(std::move(spec->requirement));
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

    // Must precede resolution, not merely run early; see the function.
    RemoveLegacyCacheEntries(opts);
    if (!opts.quiet) {
        std::print("Resolving from {}\n", ResolveRegistryBase(registryArg));
    }
    PackageResolver resolver(ResolveRegistryBase(registryArg));
    const auto resolved = resolver.Resolve(seeds, *target);
    if (!resolved) {
        ReportResolutionFailure(resolved.error());
        return 1;
    }

    InstallTally tally;
    if (!InstallResolved(resolver, *resolved, opts, tally)) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("Summary: {} installed, {} already up-to-date in {}\n", tally.installed, tally.upToDate,
                   FormatDuration(ElapsedMs(installStart)));
    }
    return 0;
}

int Cli::RunUninstall(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view packageSpec;
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
        if (!arg.starts_with('-') && packageSpec.empty()) {
            packageSpec = arg;
            continue;
        }
        PrintUnknownOption(arg, "uninstall");
        return 1;
    }
    if (global && !packageSpec.empty()) {
        std::print(stderr, "error: '--global' cannot be combined with a package name\n");
        return 1;
    }

    /// Remove every installed version of one package, or just the ones a
    /// requirement selects, and report what went.
    const auto removeVersions = [&opts](const IdentitySegment &ns, const IdentitySegment &name,
                                        const std::optional<VersionRange> &range, int &removed) -> bool {
        for (const auto &installed : InstalledVersions(ns, name)) {
            if (range && !range->Matches(installed.version)) {
                continue;
            }
            std::error_code ec;
            std::filesystem::remove_all(installed.root, ec);
            if (ec) {
                std::print(stderr, "error: failed to remove '{}': {}\n", installed.root.generic_string(), ec.message());
                return false;
            }
            if (!opts.quiet) {
                std::print("Uninstalled {} {}\n", QualifiedIdentity(ns, name), installed.version.Text());
            }
            ++removed;
        }
        return true;
    };

    if (global) {
        const auto cacheDir = RegistryPackagesDir();
        int removed = 0;
        for (const auto &[ns, name] : CachedPackages()) {
            if (!removeVersions(ns, name, std::nullopt, removed)) {
                return 1;
            }
        }
        if (removed == 0) {
            if (!opts.quiet) {
                std::print("  Global cache is empty ({})\n", cacheDir.string());
            }
            return 0;
        }
        if (!opts.quiet) {
            std::print("Summary: {} uninstalled\n", removed);
        }
        return 0;
    }

    if (!packageSpec.empty()) {
        auto spec = RequirementFromSpec(packageSpec, "uninstall");
        if (!spec) {
            return 1;
        }
        // A bare `Namespace/Name` removes every installed version; adding
        // `@requirement` narrows it to the versions that requirement selects.
        const PackageRequirement &requirement = spec->requirement;
        int removed = 0;
        if (!removeVersions(requirement.ns, requirement.package,
                            spec->explicitRange ? std::optional(requirement.range) : std::nullopt, removed)) {
            return 1;
        }
        if (removed == 0) {
            std::print(stderr, "error: no installed version of '{}' matches\n",
                       Qualified(requirement.ns, requirement.package));
            return 1;
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
    const std::vector<PackageRequirement> declared = CollectPackageRequirements(*manifest);
    if (declared.empty()) {
        if (!opts.quiet) {
            std::print("  No registry dependencies to uninstall.\n");
        }
        return 0;
    }
    int removed = 0;
    int notFound = 0;
    for (const auto &requirement : declared) {
        int before = removed;
        if (!removeVersions(requirement.ns, requirement.package, std::nullopt, removed)) {
            return 1;
        }
        if (removed == before) {
            if (!opts.quiet) {
                std::print("Not installed {}\n", Qualified(requirement.ns, requirement.package));
            }
            ++notFound;
        }
    }
    if (!opts.quiet) {
        std::print("Summary: {} uninstalled, {} not installed\n", removed, notFound);
    }
    return 0;
}

int Cli::RunAdd(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view spec;
    std::string_view pathArg;
    std::string_view registryArg;
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
        if (arg == "--registry") {
            if (!ReadRegistryOption(args, i, registryArg)) {
                return 1;
            }
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
    const auto parsedSpec = ParsePackageSpec(spec);
    if (!parsedSpec) {
        std::print(stderr, "error: {}\n", parsedSpec.error());
        return 1;
    }
    const std::string pkgName = parsedSpec->name.Text();

    if (!pathArg.empty()) {
        if (parsedSpec->ns) {
            std::print(stderr, "error: a path dependency cannot name a registry namespace\n");
            return 1;
        }
        const bool changed = manifest->AddPathDependency(parsedSpec->name, std::string(pathArg));
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
    // A registry dependency records the namespace it resolves under, so the
    // qualified spelling is the only one that can be written.
    if (!parsedSpec->ns) {
        std::print(stderr, "error: a registry dependency needs a namespace; write 'rux add Namespace/{}'\n", pkgName);
        return 1;
    }

    // Confirm the package exists before writing a requirement that could never
    // resolve. The index is the cheapest route that answers that question.
    const std::string base = ResolveRegistryBase(registryArg);
    if (!opts.quiet) {
        std::print("Resolving from {}\n", base);
    }
    if (auto entry = FetchPackageIndex(base, *parsedSpec->ns, parsedSpec->name); !entry) {
        std::print(stderr, "error: {}\n", Describe(entry.error(), base, Qualified(*parsedSpec->ns, parsedSpec->name)));
        return 1;
    }

    // An omitted requirement accepts any stable release.
    VersionRange requirement = parsedSpec->version.value_or(*VersionRange::Parse("*"));
    const std::string ver = requirement.Text();
    const bool changed = manifest->AddRegistryDependency(parsedSpec->name, *parsedSpec->ns, std::move(requirement));
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        if (changed) {
            std::print("Added {}/{} @ {}\n", parsedSpec->ns->Text(), pkgName, ver);
        }
        else {
            std::print("Up-to-date {}/{} @ {}\n", parsedSpec->ns->Text(), pkgName, ver);
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
    const std::string pkgName(name);
    const auto importName = IdentitySegment::Parse(pkgName);
    if (!importName) {
        std::print(stderr, "error: '{}' is not a valid import name: {}\n", pkgName, Describe(importName.error()));
        return 1;
    }
    if (!manifest->RemoveDependency(*importName)) {
        std::print(stderr, "error: package '{}' is not a dependency\n", pkgName);
        return 1;
    }
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        std::print("Removed {}\n", pkgName);
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
        std::vector<std::string> lines;
        for (const auto &[ns, name] : CachedPackages()) {
            for (const auto &installed : InstalledVersions(ns, name)) {
                // Cache directories carry the spelling the package was published
                // with, so the directory name is the display spelling and there
                // is no manifest to re-read for it.
                lines.push_back(std::format("{} {}", QualifiedIdentity(ns, name), installed.version.Text()));
            }
        }
        if (lines.empty()) {
            if (!opts.quiet) {
                std::print("  Global cache is empty ({})\n", cacheDir.string());
            }
            return 0;
        }
        std::print("Global cache ({} version{} at {}):\n", lines.size(), lines.size() == 1 ? "" : "s",
                   cacheDir.string());
        for (const auto &line : lines) {
            std::print("  {}\n", line);
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
        if (dep.IsPath()) {
            std::print("  {} (path: {}){}\n", dep.importName.Text(), dep.Path(), TargetSuffix(dep));
            continue;
        }
        const auto *registry = dep.Registry();
        // Naming the version a build would actually use turns the requirement
        // into something checkable without running the build.
        const auto installed = FindInstalledPackage(registry->ns, dep.package, registry->version);
        if (installed) {
            std::print("  {}/{} @ {}{} (installed {})\n", registry->ns.Text(), dep.package.Text(),
                       registry->version.Text(), TargetSuffix(dep), installed->version.Text());
        }
        else {
            std::print("  {}/{} @ {}{} (not installed)\n", registry->ns.Text(), dep.package.Text(),
                       registry->version.Text(), TargetSuffix(dep));
        }
    }
    return 0;
}

int Cli::RunUpdate(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool global = false;
    std::string_view registryArg;
    std::string_view targetArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--global") {
            global = true;
            continue;
        }
        if (arg == "--registry") {
            if (!ReadRegistryOption(args, i, registryArg)) {
                return 1;
            }
            continue;
        }
        if (arg == "--target") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--target' requires an argument\n");
                return 1;
            }
            targetArg = args[++i];
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("update");
            return 0;
        }
        PrintUnknownOption(arg, "update");
        return 1;
    }

    const auto target = ResolvePackageTarget(targetArg);
    if (!target) {
        return 1;
    }

    std::vector<PackageRequirement> seeds;
    if (global) {
        // Nothing declares a requirement on a cached package here, so each one
        // is refreshed to the newest release the registry still offers.
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

    // Must precede resolution, not merely run early; see the function.
    RemoveLegacyCacheEntries(opts);
    if (!opts.quiet) {
        std::print("Resolving from {}\n", ResolveRegistryBase(registryArg));
    }
    PackageResolver resolver(ResolveRegistryBase(registryArg));
    const auto resolved = resolver.Resolve(seeds, *target);
    if (!resolved) {
        ReportResolutionFailure(resolved.error());
        return 1;
    }

    InstallTally tally;
    if (!InstallResolved(resolver, *resolved, opts, tally)) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("Summary: {} newly installed, {} already current\n", tally.installed, tally.upToDate);
    }
    return 0;
}

// TODO: Extend Package manifest metadata support
int Cli::RunInfo(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view packageSpec;
    std::string_view registryArg;
    const bool jsonOutput = std::ranges::find(args, "--json") != args.end();
    auto JsonFailure = [&](const std::string_view message) {
        if (jsonOutput) {
            std::println("{{\"success\":false,\"error\":\"{}\"}}", EscapeJson(message));
        }
        return 1;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("info");
            return 0;
        }
        if (arg == "--json") {
            continue;
        }
        if (arg == "--registry") {
            if (!ReadRegistryOption(args, i, registryArg)) {
                return JsonFailure("--registry requires an argument");
            }
            continue;
        }
        if (!arg.starts_with('-') && packageSpec.empty()) {
            packageSpec = arg;
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
            return JsonFailure("the specified manifest was not found");
        }
    }
    else if (packageSpec.empty()) {
        auto localManifestOpt = Manifest::Find(std::filesystem::current_path());
        if (!localManifestOpt) {
            std::print(stderr, "error: missing package name, and no Rux.toml found in current directory\n");
            return JsonFailure("missing package name and no Rux.toml was found");
        }
        manifestPath = *localManifestOpt;
    }
    else {
        auto spec = RequirementFromSpec(packageSpec, "info");
        if (!spec) {
            return JsonFailure("invalid package identity or version requirement");
        }
        const PackageRequirement &requirement = spec->requirement;
        const auto installed = FindInstalledPackage(requirement.ns, requirement.package, requirement.range);
        if (!installed) {
            // Not having it locally is worth distinguishing from it not
            // existing, so the registry is asked which versions there are.
            const std::string base = ResolveRegistryBase(registryArg);
            const std::string identity = Qualified(requirement.ns, requirement.package);
            auto entry = FetchPackageIndex(base, requirement.ns, requirement.package);
            if (!entry) {
                std::print(stderr, "error: {}\n", Describe(entry.error(), base, identity));
                return JsonFailure("the registry lookup failed");
            }
            std::print(stderr, "error: no installed version of {} matches '{}'; run 'rux install {}'\n", identity,
                       requirement.range.Text(), packageSpec);
            std::print(stderr, "{}{} publishes {}\n", errorContinuation, base, DescribeAvailableVersions(*entry));
            return JsonFailure("no installed package version matches the requirement");
        }
        manifestPath = installed->root / "Rux.toml";
    }
    auto infoResult = Manifest::Load(manifestPath);
    if (!infoResult.Ok()) {
        ReportManifestDiagnostics(infoResult);
        return JsonFailure("the package manifest is invalid");
    }
    const auto manifest = std::move(infoResult.manifest);
    // not using nlohmann/json.hpp to keep compiler as small and fast as
    // possible
    if (jsonOutput) {
        std::print("{}\n", "{");
        std::print("  \"success\": true,\n");
        if (manifest->package.ns) {
            std::print("  \"namespace\": \"{}\",\n", EscapeJson(manifest->package.ns->Text()));
        }
        std::print("  \"name\": \"{}\",\n", EscapeJson(manifest->package.name.Text()));
        std::print("  \"version\": \"{}\",\n", EscapeJson(manifest->package.version.Text()));
        std::print("  \"type\": \"{}\",\n", EscapeJson(ToString(manifest->package.type)));
        if (!manifest->package.keywords.empty()) {
            std::print("  \"keywords\": [");
            for (std::size_t i = 0; i < manifest->package.keywords.size(); ++i) {
                std::print("{}\"{}\"", i == 0 ? "" : ", ", EscapeJson(manifest->package.keywords[i].Text()));
            }
            std::print("],\n");
        }
        if (!manifest->package.description.empty()) {
            std::print("  \"description\": \"{}\",\n", EscapeJson(manifest->package.description));
        }
        if (!manifest->package.authors.empty()) {
            std::print("  \"authors\": [");
            for (std::size_t i = 0; i < manifest->package.authors.size(); ++i) {
                std::print("{}\"{}\"", i == 0 ? "" : ", ", EscapeJson(manifest->package.authors[i]));
            }
            std::print("],\n");
        }
        if (!manifest->package.license.empty()) {
            std::print("  \"license\": \"{}\",\n", EscapeJson(manifest->package.license));
        }
        if (!manifest->package.licenseFile.empty()) {
            std::print("  \"licenseFile\": \"{}\",\n", EscapeJson(manifest->package.licenseFile));
        }
        if (!manifest->package.repository.empty()) {
            std::print("  \"repository\": \"{}\",\n", EscapeJson(manifest->package.repository));
        }
        if (!manifest->package.homepage.empty()) {
            std::print("  \"homepage\": \"{}\",\n", EscapeJson(manifest->package.homepage));
        }
        std::print("  \"dependencies\": [\n");
        for (size_t i = 0; i < manifest->dependencies.size(); ++i) {
            const auto &dep = manifest->dependencies[i];
            std::print("    {}", "{");
            std::print("\"name\": \"{}\"", EscapeJson(dep.importName.Text()));

            if (dep.IsPath()) {
                std::print(", \"path\": \"{}\"", EscapeJson(dep.Path()));
            }
            else {
                const auto *registry = dep.Registry();
                std::print(", \"namespace\": \"{}\", \"package\": \"{}\", \"version\": \"{}\"",
                           EscapeJson(registry->ns.Text()), EscapeJson(dep.package.Text()),
                           EscapeJson(registry->version.Text()));
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
        if (manifest->package.ns) {
            std::print("Namespace:   {}\n", manifest->package.ns->Text());
        }
        std::print("Name:        {}\n"
                   "Version:     {}\n"
                   "Type:        {}\n",
                   manifest->package.name.Text(), manifest->package.version.Text(), ToString(manifest->package.type));
        if (!manifest->package.keywords.empty()) {
            std::print("Keywords:    ");
            for (std::size_t i = 0; i < manifest->package.keywords.size(); ++i) {
                std::print("{}{}", i == 0 ? "" : ", ", manifest->package.keywords[i].Text());
            }
            std::print("\n");
        }
        if (!manifest->package.description.empty()) {
            std::print("Description: {}\n", manifest->package.description);
        }
        if (!manifest->package.authors.empty()) {
            std::print("Authors:     ");
            for (std::size_t i = 0; i < manifest->package.authors.size(); ++i) {
                std::print("{}{}", i == 0 ? "" : ", ", manifest->package.authors[i]);
            }
            std::print("\n");
        }
        if (!manifest->package.license.empty()) {
            std::print("License:     {}\n", manifest->package.license);
        }
        if (!manifest->package.licenseFile.empty()) {
            std::print("LicenseFile: {}\n", manifest->package.licenseFile);
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
                if (dep.IsPath()) {
                    std::print("  - {} (path: {}){}\n", dep.importName.Text(), dep.Path(), TargetSuffix(dep));
                }
                else {
                    const auto *registry = dep.Registry();
                    std::print("  - {}/{} @ {}{}\n", registry->ns.Text(), dep.package.Text(), registry->version.Text(),
                               TargetSuffix(dep));
                }
            }
        }
    }
    return 0;
}
