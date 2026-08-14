// Package inspection and cache commands: uninstall, list, and info.
//
// Registry lookups use the versioned API in Driver/Registry. Installed packages
// are cached per exact version, so one host can hold several versions of the
// same package and a build picks the one its manifest asks for without
// contacting the registry.

#include "Cli/Cli.h"
#include "Cli/TerminalStyle.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Driver/Credentials.h"
#include "Driver/PackageResolution.h"
#include "Driver/Registry.h"
#include "Package/Manifest.h"

#include <algorithm>
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

} // namespace

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
