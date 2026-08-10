// Package-manager commands: install, uninstall, add, remove, list, update, info.
//
// Every command that reaches the network goes through the versioned registry
// API in Driver/Registry: the resolver index chooses versions, and the download
// route supplies the published .ruxpkg. Installed packages are cached per exact
// version, so one host can hold several versions of the same package and a
// build picks the one its manifest asks for without contacting the registry.

#include "Cli/Cli.h"
#include "Cli/TerminalStyle.h"
#include "Driver/BuildTarget.h"
#include "Driver/Credentials.h"
#include "Driver/Registry.h"
#include "Package/Artifact.h"
#include "Package/Checksum.h"
#include "Package/Manifest.h"
#include "System/Process.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Driver;
using namespace System;
using namespace CliSupport;

namespace {
/// One thing that must be resolved: a package and the requirement on it.
struct Requirement {
    IdentitySegment ns;
    IdentitySegment package;
    VersionRange range;
};

/// A package the resolver settled on.
struct Resolution {
    IdentitySegment ns;
    IdentitySegment package;
    SemanticVersion version;
};

/// The lookup key two spellings of one package share.
std::string IdentityKey(const IdentitySegment &ns, const IdentitySegment &package) {
    return ns.Normalized() + "/" + package.Normalized();
}

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

/**
 * @brief Fetches each package's index at most once per command.
 *
 * A dependency graph names the same package from several dependents, and the
 * index is the same document every time; one request per package keeps a large
 * install from hammering the registry.
 */
class IndexCache {
public:
    explicit IndexCache(std::string registryBase)
        : base(std::move(registryBase)) {
    }

    [[nodiscard]] const std::string &Base() const noexcept {
        return base;
    }

    /// The index for one package, or nullptr after reporting why it is missing.
    [[nodiscard]] const RegistryIndexEntry *Get(const IdentitySegment &ns, const IdentitySegment &package) {
        const std::string key = IdentityKey(ns, package);
        if (const auto found = entries.find(key); found != entries.end()) {
            return &found->second;
        }
        auto fetched = FetchPackageIndex(base, ns, package);
        if (!fetched) {
            std::print(stderr, "error: {}\n", Describe(fetched.error(), base, Qualified(ns, package)));
            return nullptr;
        }
        return &entries.emplace(key, std::move(*fetched)).first->second;
    }

private:
    std::string base;
    std::map<std::string, RegistryIndexEntry> entries;
};

/// The requirements accumulated for one package, and the version they settled on.
struct Selection {
    IdentitySegment ns;
    IdentitySegment package;
    std::vector<VersionRange> ranges;
    SemanticVersion version;
};

/**
 * @brief Resolve `seeds` and everything they depend on.
 *
 * Breadth-first over the resolver index: each selected version contributes its
 * own dependency edges, so the whole transitive graph is decided before
 * anything is downloaded. One version is selected per package; when a later
 * requirement rules the current selection out, the package is re-selected
 * against every requirement seen so far. There is no backtracking, so a graph
 * with genuinely incompatible requirements is reported rather than silently
 * resolved one way.
 *
 * @return The resolved graph, or nullopt after the reason has been printed
 */
std::optional<std::vector<Resolution>> ResolveGraph(IndexCache &index, const std::vector<Requirement> &seeds,
                                                    const Target::OS targetOS) {
    const SemanticVersion compiler = CompilerVersion();
    std::map<std::string, Selection> selections;
    std::vector<Requirement> queue = seeds;
    std::unordered_set<std::string> processed;
    // Insertion order is what the install phase reports, so seeds and their
    // dependencies appear in the order they were discovered.
    std::vector<std::string> order;

    for (std::size_t i = 0; i < queue.size(); ++i) {
        const Requirement requirement = queue[i];
        const std::string key = IdentityKey(requirement.ns, requirement.package);
        if (!processed.insert(key + "@" + requirement.range.Text()).second) {
            continue;
        }

        const RegistryIndexEntry *entry = index.Get(requirement.ns, requirement.package);
        if (entry == nullptr) {
            return std::nullopt;
        }

        auto [selection, inserted] = selections.try_emplace(
            key, Selection{.ns = entry->ns, .package = entry->package, .ranges = {}, .version = {}});
        if (inserted) {
            order.push_back(key);
        }
        selection->second.ranges.push_back(requirement.range);

        const RegistryVersion *chosen = SelectVersion(*entry, selection->second.ranges, compiler);
        if (chosen == nullptr) {
            const ResolutionFailure failure =
                DescribeResolutionFailure(*entry, selection->second.ranges, compiler, index.Base());
            std::print(stderr, "error: {}\n", failure.message);
            for (const auto &detail : failure.details) {
                std::print(stderr, "{}{}\n", errorContinuation, detail);
            }
            return std::nullopt;
        }
        if (!selection->second.version.Empty() && selection->second.version.Text() == chosen->version.Text()) {
            continue;
        }
        selection->second.version = chosen->version;
        for (const auto &edge : chosen->dependencies) {
            if (edge.MatchesTarget(targetOS)) {
                queue.push_back(Requirement{.ns = edge.ns, .package = edge.package, .range = edge.range});
            }
        }
    }

    std::vector<Resolution> resolved;
    resolved.reserve(order.size());
    for (const auto &key : order) {
        const Selection &selection = selections.at(key);
        resolved.push_back(Resolution{.ns = selection.ns, .package = selection.package, .version = selection.version});
    }
    return resolved;
}

/// Whether the manifest in a cache directory really is the package it claims.
bool CacheEntryMatches(const std::filesystem::path &root, const Resolution &resolution) {
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
bool InstallResolved(const IndexCache &index, const std::vector<Resolution> &resolved, const GlobalOptions &opts,
                     InstallTally &tally) {
    for (const auto &resolution : resolved) {
        const std::string identity = Qualified(resolution.ns, resolution.package);
        const std::filesystem::path packageDir =
            RegistryPackageDir(resolution.ns, resolution.package, resolution.version);

        if (CacheEntryMatches(packageDir, resolution)) {
            if (!opts.quiet) {
                std::print("{} {} {}\n", Status("Up-to-date"), identity, resolution.version.Text());
            }
            ++tally.upToDate;
            continue;
        }

        if (!opts.quiet) {
            std::print("{} {} {}\n", Status("Downloading"), identity, resolution.version.Text());
        }
        auto digest = FetchArtifactChecksum(index.Base(), resolution.ns, resolution.package, resolution.version);
        if (!digest) {
            std::print(stderr, "error: {}\n", Describe(digest.error(), index.Base(), identity));
            return false;
        }
        auto archive = DownloadArtifact(index.Base(), resolution.ns, resolution.package, resolution.version);
        if (!archive) {
            std::print(stderr, "error: {}\n", Describe(archive.error(), index.Base(), identity));
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
            std::print("{} {} {} ({} files)\n", Status("Installed"), identity, resolution.version.Text(),
                       extracted->fileCount);
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
            std::print("{} legacy cache entry {}\n", Status("Removed"), entry.path().filename().string());
        }
    }
}

/// Collect the registry dependencies a manifest declares.
void QueueRegistryDependencies(const Manifest &manifest, const std::optional<Target::OS> targetOS,
                               std::vector<Requirement> &out) {
    for (const auto &dep : manifest.dependencies) {
        if (const RegistryDependencySource *registry = dep.Registry();
            registry != nullptr && (!targetOS || dep.MatchesTarget(*targetOS))) {
            out.push_back(Requirement{.ns = registry->ns, .package = dep.package, .range = registry->version});
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
std::optional<std::vector<Requirement>> SeedFromProject(const GlobalOptions &opts, const Target::OS targetOS) {
    std::vector<Requirement> seeds;
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
            std::print("{} workspace\n", Status("Installing"));
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
            QueueRegistryDependencies(*memberManifest, targetOS, seeds);
        }
        return seeds;
    }

    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return std::nullopt;
    }
    if (!manifest->IsWorkspace()) {
        QueueRegistryDependencies(*manifest, targetOS, seeds);
        return seeds;
    }

    if (!opts.quiet) {
        std::print("{} workspace\n", Status("Installing"));
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
        QueueRegistryDependencies(*memberManifest, targetOS, seeds);
    }
    return seeds;
}

/// A requirement read off the command line, and whether it was written down.
struct SpecRequirement {
    Requirement requirement;

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

std::optional<Target::OS> ResolvePackageTarget(const std::string_view requested) {
    const std::string target = requested.empty() ? HostTargetTriple() : CanonicalTargetTriple(requested);
    if (!IsSupportedTargetTriple(target)) {
        std::print(stderr, "error: unsupported target '{}'; supported targets are {}\n", target,
                   SupportedTargetTriples());
        return std::nullopt;
    }
    return TargetTripleOs(target);
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

    const auto targetOS = ResolvePackageTarget(targetArg);
    if (!targetOS) {
        return 1;
    }

    std::vector<Requirement> seeds;
    if (!packageSpec.empty()) {
        auto spec = RequirementFromSpec(packageSpec, "install");
        if (!spec) {
            return 1;
        }
        seeds.push_back(std::move(spec->requirement));
    }
    else {
        auto seeded = SeedFromProject(opts, *targetOS);
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
        std::print("{} from {}\n", Status("Resolving"), ResolveRegistryBase(registryArg));
    }
    IndexCache index(ResolveRegistryBase(registryArg));
    const auto resolved = ResolveGraph(index, seeds, *targetOS);
    if (!resolved) {
        return 1;
    }

    InstallTally tally;
    if (!InstallResolved(index, *resolved, opts, tally)) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("{} {} installed, {} already up-to-date\n", Status("Summary:"), tally.installed, tally.upToDate);
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
                std::print("{} {} {}\n", Status("Uninstalled"), QualifiedIdentity(ns, name), installed.version.Text());
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
            std::print("{} {} uninstalled\n", Status("Summary:"), removed);
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
        const Requirement &requirement = spec->requirement;
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
    std::vector<Requirement> declared;
    QueueRegistryDependencies(*manifest, std::nullopt, declared);
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
                std::print("{} {}\n", Status("Not found"), Qualified(requirement.ns, requirement.package));
            }
            ++notFound;
        }
    }
    if (!opts.quiet) {
        std::print("{} {} uninstalled, {} not installed\n", Status("Summary:"), removed, notFound);
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
        std::print("{} from {}\n", Status("Resolving"), base);
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
        std::print("{} {}\n", Status("Removed"), pkgName);
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

    const auto targetOS = ResolvePackageTarget(targetArg);
    if (!targetOS) {
        return 1;
    }

    std::vector<Requirement> seeds;
    if (global) {
        // Nothing declares a requirement on a cached package here, so each one
        // is refreshed to the newest release the registry still offers.
        const VersionRange any = *VersionRange::Parse("*");
        for (const auto &[ns, name] : CachedPackages()) {
            seeds.push_back(Requirement{.ns = ns, .package = name, .range = any});
        }
        if (seeds.empty()) {
            if (!opts.quiet) {
                std::print("  No packages in global cache to update.\n");
            }
            return 0;
        }
    }
    else {
        auto seeded = SeedFromProject(opts, *targetOS);
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
        std::print("{} from {}\n", Status("Resolving"), ResolveRegistryBase(registryArg));
    }
    IndexCache index(ResolveRegistryBase(registryArg));
    const auto resolved = ResolveGraph(index, seeds, *targetOS);
    if (!resolved) {
        return 1;
    }

    InstallTally tally;
    if (!InstallResolved(index, *resolved, opts, tally)) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("{} {} newly installed, {} already current\n", Status("Summary:"), tally.installed, tally.upToDate);
    }
    return 0;
}

// TODO: Extend Package manifest metadata support
int Cli::RunInfo(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view packageSpec;
    std::string_view registryArg;
    bool jsonOutput = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("info");
            return 0;
        }
        if (arg == "--json") {
            jsonOutput = true;
            continue;
        }
        if (arg == "--registry") {
            if (!ReadRegistryOption(args, i, registryArg)) {
                return 1;
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
            return 1;
        }
    }
    else if (packageSpec.empty()) {
        auto localManifestOpt = Manifest::Find(std::filesystem::current_path());
        if (!localManifestOpt) {
            std::print(stderr, "error: missing package name, and no Rux.toml found in current directory\n");
            return 1;
        }
        manifestPath = *localManifestOpt;
    }
    else {
        auto spec = RequirementFromSpec(packageSpec, "info");
        if (!spec) {
            return 1;
        }
        const Requirement &requirement = spec->requirement;
        const auto installed = FindInstalledPackage(requirement.ns, requirement.package, requirement.range);
        if (!installed) {
            // Not having it locally is worth distinguishing from it not
            // existing, so the registry is asked which versions there are.
            const std::string base = ResolveRegistryBase(registryArg);
            const std::string identity = Qualified(requirement.ns, requirement.package);
            auto entry = FetchPackageIndex(base, requirement.ns, requirement.package);
            if (!entry) {
                std::print(stderr, "error: {}\n", Describe(entry.error(), base, identity));
                return 1;
            }
            std::print(stderr, "error: no installed version of {} matches '{}'; run 'rux install {}'\n", identity,
                       requirement.range.Text(), packageSpec);
            std::print(stderr, "{}{} publishes {}\n", errorContinuation, base, DescribeAvailableVersions(*entry));
            return 1;
        }
        manifestPath = installed->root / "Rux.toml";
    }
    auto infoResult = Manifest::Load(manifestPath);
    if (!infoResult.Ok()) {
        ReportManifestDiagnostics(infoResult);
        return 1;
    }
    const auto manifest = std::move(infoResult.manifest);
    // not using nlohmann/json.hpp to keep compiler as small and fast as
    // possible
    if (jsonOutput) {
        std::print("{}\n", "{");
        if (manifest->package.ns) {
            std::print("  \"namespace\": \"{}\",\n", manifest->package.ns->Text());
        }
        std::print("  \"name\": \"{}\",\n", manifest->package.name.Text());
        std::print("  \"version\": \"{}\",\n", manifest->package.version.Text());
        std::print("  \"type\": \"{}\",\n", ToString(manifest->package.type));
        if (!manifest->package.keywords.empty()) {
            std::print("  \"keywords\": [");
            for (std::size_t i = 0; i < manifest->package.keywords.size(); ++i) {
                std::print("{}\"{}\"", i == 0 ? "" : ", ", manifest->package.keywords[i].Text());
            }
            std::print("],\n");
        }
        if (!manifest->package.description.empty()) {
            std::print("  \"description\": \"{}\",\n", manifest->package.description);
        }
        if (!manifest->package.authors.empty()) {
            std::print("  \"authors\": [");
            for (std::size_t i = 0; i < manifest->package.authors.size(); ++i) {
                std::print("{}\"{}\"", i == 0 ? "" : ", ", manifest->package.authors[i]);
            }
            std::print("],\n");
        }
        if (!manifest->package.license.empty()) {
            std::print("  \"license\": \"{}\",\n", manifest->package.license);
        }
        if (!manifest->package.licenseFile.empty()) {
            std::print("  \"licenseFile\": \"{}\",\n", manifest->package.licenseFile);
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
            std::print("\"name\": \"{}\"", dep.importName.Text());

            if (dep.IsPath()) {
                std::print(", \"path\": \"{}\"", dep.Path());
            }
            else {
                const auto *registry = dep.Registry();
                std::print(", \"namespace\": \"{}\", \"package\": \"{}\", \"version\": \"{}\"", registry->ns.Text(),
                           dep.package.Text(), registry->version.Text());
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
