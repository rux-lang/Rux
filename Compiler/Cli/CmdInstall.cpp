// Package installation commands: install and update.
//
// Both commands resolve a complete registry dependency graph before fetching
// artifacts, then verify and stage every selected package through the same
// cache installation path.

#include "Cli/Cli.h"
#include "Cli/ManifestInput.h"
#include "Cli/Reporter.h"
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
#include <format>
#include <optional>
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
                                                  const InstallCommand command,
                                                  const CliSupport::Reporter &diagnostics) {
    InstallOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--global" && command == InstallCommand::Update) {
            options.global = true;
            continue;
        }
        if (arg == "--registry" || arg == "--target") {
            if (i + 1 >= args.size()) {
                diagnostics.Error(std::format("option '{}' requires {}", arg,
                                              arg == "--registry" ? "a registry URL" : "a target triple"));
                diagnostics.Help(std::format("run 'rux help {}' for usage information",
                                             command == InstallCommand::Install ? "install" : "update"));
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
        diagnostics.Error(std::format("unknown option '{}' for command '{}'", arg, commandName));
        diagnostics.Help(std::format("run 'rux help {}' for usage information", commandName));
        return std::nullopt;
    }
    return options;
}

std::optional<Target::TargetTriple> ResolvePackageTarget(const std::string_view requested,
                                                         const CliSupport::Reporter &diagnostics) {
    const auto target =
        requested.empty() ? std::optional{Target::TargetTriple::Host()} : Target::TargetTriple::Parse(requested);
    if (!target) {
        diagnostics.Error(std::format("target '{}' is not supported", requested));
        diagnostics.Note(std::format("supported targets: {}", SupportedTargetTriples()));
        diagnostics.Help("choose one of the supported target triples");
        return std::nullopt;
    }
    return target;
}

/// Turn a command-line package spec into one registry requirement.
std::optional<PackageRequirement> RequirementFromSpec(const std::string_view spec,
                                                      const CliSupport::Reporter &diagnostics) {
    const auto parsed = ParsePackageSpec(spec);
    if (!parsed) {
        diagnostics.Error(parsed.error().message);
        for (const auto &note : parsed.error().notes) {
            diagnostics.Note(note);
        }
        diagnostics.Help(parsed.error().help.value_or("use '[namespace]/[package]@[requirement]'"));
        return std::nullopt;
    }
    if (!parsed->ns) {
        diagnostics.Error(std::format("registry package '{}' must include a namespace", spec));
        diagnostics.Help(std::format("use 'rux install Namespace/{}'", parsed->name.Text()));
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
                                                               const Target::TargetTriple target,
                                                               const CliSupport::Reporter &output,
                                                               const CliSupport::Reporter &diagnostics) {
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
        output.Progress("Reading", "dependencies from the manifestless workspace");
        for (const auto &memberPath : workspaceManifests) {
            auto memberManifest = LoadManifest(memberPath);
            if (!memberManifest) {
                return std::nullopt;
            }
            if (memberManifest->IsWorkspace() || memberManifest->package.name.Empty()) {
                diagnostics.Error(
                    std::format("workspace member '{}' is not a package", memberPath.parent_path().string()));
                diagnostics.Help("replace the member with a package manifest, then retry");
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

    output.Progress("Reading", std::format("workspace dependencies from '{}'", manifestPath->string()));
    const std::filesystem::path workspaceRoot = manifestPath->parent_path();
    for (const auto &member : manifest->workspace.packages) {
        const auto memberPath = (workspaceRoot / member / "Rux.toml").lexically_normal();
        std::error_code ec;
        if (!std::filesystem::exists(memberPath, ec)) {
            diagnostics.Error(std::format("workspace member '{}' has no Rux.toml", member));
            diagnostics.Help("correct the workspace member path or add its manifest, then retry");
            return std::nullopt;
        }
        auto memberManifest = LoadManifest(memberPath);
        if (!memberManifest) {
            return std::nullopt;
        }
        if (memberManifest->IsWorkspace() || memberManifest->package.name.Empty()) {
            diagnostics.Error(std::format("workspace member '{}' is not a package", member));
            diagnostics.Help("replace the member with a package manifest, then retry");
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
std::optional<int> RemoveLegacyCacheEntries(const CliSupport::Reporter &output,
                                            const CliSupport::Reporter &diagnostics) {
    const std::filesystem::path cacheDir = RegistryPackagesDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(cacheDir, ec)) {
        return 0;
    }
    int removed = 0;
    for (const auto &entry : std::filesystem::directory_iterator(cacheDir, ec)) {
        if (!entry.is_directory(ec) || !std::filesystem::exists(entry.path() / "Rux.toml", ec)) {
            continue;
        }
        std::error_code removeError;
        std::filesystem::remove_all(entry.path(), removeError);
        if (removeError) {
            diagnostics.Error(std::format("could not remove legacy cache entry '{}'", entry.path().string()));
            diagnostics.Note(removeError.message());
            diagnostics.Help("check the cache directory permissions, then retry");
            return std::nullopt;
        }
        output.Success("Removed", std::format("legacy cache entry '{}'", entry.path().filename().string()));
        ++removed;
    }
    return removed;
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
bool InstallResolved(const PackageResolver &resolver, const std::vector<ResolvedPackage> &resolved, InstallTally &tally,
                     const CliSupport::Reporter &output, const CliSupport::Reporter &diagnostics) {
    for (const auto &resolution : resolved) {
        const std::string identity = QualifiedIdentity(resolution.ns, resolution.package);
        const std::filesystem::path packageDir =
            RegistryPackageDir(resolution.ns, resolution.package, resolution.version);

        if (CacheEntryMatches(packageDir, resolution)) {
            output.Success("Up-to-date", std::format("{} {}", identity, resolution.version.Text()));
            ++tally.upToDate;
            continue;
        }

        output.Progress("Downloading", std::format("{} {}", identity, resolution.version.Text()));
        output.Verbose(std::format("checksum metadata from '{}/v1/packages/{}/{}'", resolver.Base(), identity,
                                   resolution.version.Text()));
        auto digest = FetchArtifactChecksum(resolver.Base(), resolution.ns, resolution.package, resolution.version);
        if (!digest) {
            const auto problem = Describe(digest.error(), resolver.Base(), identity);
            diagnostics.Error(
                std::format("could not fetch checksum metadata for {} {}", identity, resolution.version.Text()));
            diagnostics.Note(problem.message);
            for (const auto &note : problem.notes) {
                diagnostics.Note(note);
            }
            diagnostics.Help(problem.help.value_or("check the registry URL and network, then retry the installation"));
            return false;
        }
        auto archive = DownloadArtifact(resolver.Base(), resolution.ns, resolution.package, resolution.version);
        if (!archive) {
            const auto problem = Describe(archive.error(), resolver.Base(), identity);
            diagnostics.Error(std::format("could not download {} {}", identity, resolution.version.Text()));
            diagnostics.Note(problem.message);
            for (const auto &note : problem.notes) {
                diagnostics.Note(note);
            }
            diagnostics.Help(problem.help.value_or("check the registry URL and network, then retry the installation"));
            return false;
        }
        if (const std::string actual = Sha256Hex(*archive); !DigestsEqual(actual, *digest)) {
            diagnostics.Error(std::format("downloaded archive for {} {} failed checksum verification", identity,
                                          resolution.version.Text()));
            diagnostics.Note(std::format("expected sha256: {}", *digest));
            diagnostics.Note(std::format("downloaded sha256: {}", actual));
            diagnostics.Help("retry the download; contact the registry operator if the mismatch persists");
            return false;
        }

        std::filesystem::path staging = packageDir;
        staging += ".download";
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        std::filesystem::create_directories(packageDir.parent_path(), ec);
        if (ec) {
            diagnostics.Error(
                std::format("could not create package cache directory '{}'", packageDir.parent_path().string()));
            diagnostics.Note(ec.message());
            diagnostics.Help("check the cache directory permissions, then retry");
            return false;
        }

        auto extracted = ExtractPackageArtifact(*archive, staging);
        if (!extracted) {
            std::filesystem::remove_all(staging, ec);
            diagnostics.Error(
                std::format("downloaded archive for {} {} was rejected", identity, resolution.version.Text()));
            diagnostics.Note(extracted.error().message);
            for (const auto &note : extracted.error().notes) {
                diagnostics.Note(note);
            }
            diagnostics.Help("contact the registry operator; the published archive is unsafe or invalid");
            return false;
        }

        const auto staged = Manifest::Load(staging / "Rux.toml");
        if (!staged.Ok()) {
            std::filesystem::remove_all(staging, ec);
            diagnostics.Error(std::format("archive published as {} {} contains an invalid manifest", identity,
                                          resolution.version.Text()));
            if (!staged.diagnostics.empty()) {
                diagnostics.Note(staged.diagnostics.front().Format());
            }
            diagnostics.Help("contact the registry operator; the artifact metadata is inconsistent");
            return false;
        }
        if (staged.manifest->IsWorkspace() || !staged.manifest->package.ns ||
            *staged.manifest->package.ns != resolution.ns || staged.manifest->package.name != resolution.package ||
            staged.manifest->package.version.Text() != resolution.version.Text()) {
            std::filesystem::remove_all(staging, ec);
            diagnostics.Error(std::format("archive published as {} {} contains a different package", identity,
                                          resolution.version.Text()));
            if (staged.manifest->IsWorkspace()) {
                diagnostics.Note("archive manifest declares a workspace instead of a package");
            }
            else {
                const std::string actualIdentity = staged.manifest->package.ns
                                                     ? std::format("{}/{}", staged.manifest->package.ns->Text(),
                                                                   staged.manifest->package.name.Text())
                                                     : staged.manifest->package.name.Text();
                diagnostics.Note(std::format("archive manifest declares {} {}", actualIdentity,
                                             staged.manifest->package.version.Text()));
            }
            diagnostics.Help("contact the registry operator; the artifact metadata is inconsistent");
            return false;
        }

        if (!CommitDownloadedPackage(staging, packageDir)) {
            std::filesystem::remove_all(staging, ec);
            diagnostics.Error(std::format("could not install {} {} into '{}'", identity, resolution.version.Text(),
                                          packageDir.string()));
            diagnostics.Help("check the cache directory permissions, then retry");
            return false;
        }
        output.Success("Installed", std::format("{} {} ({})", identity, resolution.version.Text(),
                                                Reporting::FormatCount(extracted->fileCount, "file")));
        ++tally.installed;
    }
    return true;
}

void ReportResolutionFailure(const ResolutionFailure &failure, const CliSupport::Reporter &diagnostics) {
    diagnostics.Error(failure.message);
    for (const auto &note : failure.notes) {
        diagnostics.Note(note);
    }
    if (failure.help) {
        diagnostics.Help(*failure.help);
    }
}

int ResolveAndInstall(const InstallCommand command, const std::vector<PackageRequirement> &seeds,
                      const Target::TargetTriple target, const std::string_view registryArg, const GlobalOptions &opts,
                      const std::chrono::steady_clock::time_point started = {}) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const auto legacyRemoved = RemoveLegacyCacheEntries(output, diagnostics);
    if (!legacyRemoved) {
        return 1;
    }
    const std::string registry = ResolveRegistryBase(registryArg);
    output.Progress("Resolving", std::format("dependencies from '{}'", registry));
    PackageResolver resolver(registry);
    const auto resolved = resolver.Resolve(seeds, target);
    if (!resolved) {
        ReportResolutionFailure(resolved.error(), diagnostics);
        return 1;
    }
    output.Verbose(std::format("selected {}", Reporting::FormatCount(resolved->size(), "package")));

    InstallTally tally;
    if (!InstallResolved(resolver, *resolved, tally, output, diagnostics)) {
        return 1;
    }
    output.Success("Finished", std::format("{} in {} ({} installed, {} up-to-date, {} legacy removed)",
                                           command == InstallCommand::Install ? "installation" : "update",
                                           Reporting::FormatDuration(ElapsedMs(started)), tally.installed,
                                           tally.upToDate, *legacyRemoved));
    return 0;
}
} // namespace

int Cli::RunInstall(const std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const auto options = ParseInstallOptions(args, InstallCommand::Install, diagnostics);
    if (!options) {
        return 1;
    }

    const auto installStart = std::chrono::steady_clock::now();
    const auto target = ResolvePackageTarget(options->target, diagnostics);
    if (!target) {
        return 1;
    }

    std::vector<PackageRequirement> seeds;
    if (!options->packageSpec.empty()) {
        auto requirement = RequirementFromSpec(options->packageSpec, diagnostics);
        if (!requirement) {
            return 1;
        }
        seeds.push_back(std::move(*requirement));
    }
    else {
        auto seeded = SeedFromProject(opts, *target, output, diagnostics);
        if (!seeded) {
            return 1;
        }
        seeds = std::move(*seeded);
        if (seeds.empty()) {
            output.Success("Up-to-date", std::format("project dependencies in {} (0 packages)",
                                                     Reporting::FormatDuration(ElapsedMs(installStart))));
            return 0;
        }
    }

    return ResolveAndInstall(InstallCommand::Install, seeds, *target, options->registry, opts, installStart);
}

int Cli::RunUpdate(const std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const auto options = ParseInstallOptions(args, InstallCommand::Update, diagnostics);
    if (!options) {
        return 1;
    }

    const auto updateStart = std::chrono::steady_clock::now();
    const auto target = ResolvePackageTarget(options->target, diagnostics);
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
            output.Success("Up-to-date", std::format("global package cache in {} (0 packages)",
                                                     Reporting::FormatDuration(ElapsedMs(updateStart))));
            return 0;
        }
    }
    else {
        auto seeded = SeedFromProject(opts, *target, output, diagnostics);
        if (!seeded) {
            return 1;
        }
        seeds = std::move(*seeded);
        if (seeds.empty()) {
            output.Success("Up-to-date", std::format("project dependencies in {} (0 packages)",
                                                     Reporting::FormatDuration(ElapsedMs(updateStart))));
            return 0;
        }
    }

    return ResolveAndInstall(InstallCommand::Update, seeds, *target, options->registry, opts, updateStart);
}
