// Package inspection and cache lifecycle commands: list, info, and uninstall.
//
// Registry lookups use the versioned API in Package/Registry. Installed packages
// are cached per exact version, so one host can hold several versions of the
// same package and a build picks the one its manifest asks for without
// contacting the registry.

#include "Cli/BuildReport.h"
#include "Cli/Cli.h"
#include "Cli/ManifestInput.h"
#include "Cli/Reporter.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Package/Cache.h"
#include "Package/Credentials.h"
#include "Package/Manifest.h"
#include "Package/PackageResolution.h"
#include "Package/Registry.h"
#include "Reporting/Reporting.h"

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

using namespace Rux::Packages;

using namespace Rux;
using namespace CliSupport;
using namespace Driver;

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
std::optional<SpecRequirement> RequirementFromSpec(const std::string_view spec, const std::string_view command,
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
        diagnostics.Help(std::format("use 'rux {} Namespace/{}'", command, parsed->name.Text()));
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
bool ReadRegistryOption(const std::span<const std::string_view> args, std::size_t &index, std::string_view &registryArg,
                        const CliSupport::Reporter &diagnostics) {
    if (index + 1 >= args.size()) {
        diagnostics.Error("option '--registry' requires a registry URL");
        diagnostics.Help("try 'rux info Namespace/Package --registry https://registry.example'");
        return false;
    }
    registryArg = args[++index];
    return true;
}

std::string InstallCommand(const IdentitySegment &ns, const IdentitySegment &package, const VersionRange &range) {
    const std::string identity = Qualified(ns, package);
    return range.Text() == "*" ? std::format("rux install {}", identity)
                               : std::format("rux install {}@{}", identity, range.Text());
}

void PrintDependency(const ManifestDependency &dependency, const CliSupport::Reporter &output) {
    if (dependency.IsPath()) {
        output.Write(std::format("  Path {} at '{}'{}\n", dependency.importName.Text(), dependency.Path(),
                                 TargetSuffix(dependency)));
        return;
    }
    const auto *registry = dependency.Registry();
    const std::string identity = Qualified(registry->ns, dependency.package);
    const auto installed = FindInstalledPackage(registry->ns, dependency.package, registry->version);
    if (installed) {
        output.Write(std::format("  Resolved {} @ {}{} to {}\n", identity, registry->version.Text(),
                                 TargetSuffix(dependency), installed->version.Text()));
        return;
    }
    output.Write(std::format("  Missing {} @ {}{}\n", identity, registry->version.Text(), TargetSuffix(dependency)));
    if (output.Visible(CliSupport::MessageVisibility::Normal)) {
        output.Help(
            std::format("install it with '{}'", InstallCommand(registry->ns, dependency.package, registry->version)));
    }
}

} // namespace

int Cli::RunUninstall(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
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
        diagnostics.Error("option '--global' cannot be combined with a package name");
        diagnostics.Help(std::format("use 'rux uninstall {}' or 'rux uninstall --global'", packageSpec));
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto cacheDir = RegistryPackagesDir();

    /// Remove every installed version of one package, or just the ones a
    /// requirement selects, and report what went.
    const auto removeVersions = [&](const IdentitySegment &ns, const IdentitySegment &name,
                                    const std::optional<VersionRange> &range, int &removed) -> bool {
        for (const auto &installed : InstalledVersions(ns, name)) {
            if (range && !range->Matches(installed.version)) {
                continue;
            }
            std::error_code ec;
            std::filesystem::remove_all(installed.root, ec);
            if (ec) {
                diagnostics.Error(std::format("could not remove cached package at '{}'", installed.root.string()));
                diagnostics.Note(ec.message());
                diagnostics.Help("check the cache directory's permissions, then retry");
                return false;
            }
            output.Success("Removed", std::format("{} {}", QualifiedIdentity(ns, name), installed.version.Text()));
            ++removed;
        }
        return true;
    };

    if (global) {
        int removed = 0;
        for (const auto &[ns, name] : CachedPackages()) {
            if (!removeVersions(ns, name, std::nullopt, removed)) {
                return 1;
            }
        }
        if (removed == 0) {
            output.Success("Empty", "global package cache");
            output.Detail(std::format("Cache: '{}'", cacheDir.string()));
            return 0;
        }
        output.Success("Removed", std::format("{} from the global package cache in {}",
                                              Reporting::FormatCount(removed, "package version"),
                                              Reporting::FormatDuration(ElapsedMs(started))));
        output.Detail(std::format("Cache: '{}'", cacheDir.string()));
        return 0;
    }

    if (!packageSpec.empty()) {
        auto spec = RequirementFromSpec(packageSpec, "uninstall", diagnostics);
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
            const std::string identity = Qualified(requirement.ns, requirement.package);
            diagnostics.Error(
                std::format("no installed version of '{}' matches '{}'", identity, requirement.range.Text()));
            diagnostics.Note(std::format("global package cache: '{}'", cacheDir.string()));
            diagnostics.Help(std::format("run '{}' to install a matching version",
                                         InstallCommand(requirement.ns, requirement.package, requirement.range)));
            return 1;
        }
        output.Success("Removed", std::format("{} in {}", Reporting::FormatCount(removed, "package version"),
                                              Reporting::FormatDuration(ElapsedMs(started))));
        output.Detail(std::format("Cache: '{}'", cacheDir.string()));
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
        output.Success("Unchanged", "global package cache; the project declares no registry dependencies");
        output.Detail(std::format("Manifest: '{}'", manifestPath->string()));
        output.Detail(std::format("Cache: '{}'", cacheDir.string()));
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
            output.Write(std::format("Missing {}\n", Qualified(requirement.ns, requirement.package)));
            if (output.Visible(CliSupport::MessageVisibility::Normal)) {
                output.Help(std::format("install it with '{}'",
                                        InstallCommand(requirement.ns, requirement.package, requirement.range)));
            }
            ++notFound;
        }
    }
    output.Success("Finished", std::format("project uninstall in {} ({} removed, {} missing)",
                                           Reporting::FormatDuration(ElapsedMs(started)), removed, notFound));
    output.Detail(std::format("Manifest: '{}'", manifestPath->string()));
    output.Detail(std::format("Cache: '{}'", cacheDir.string()));
    return 0;
}

int Cli::RunList(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
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
            output.Success("Empty", "global package cache");
            output.Detail(std::format("Cache: '{}'", cacheDir.string()));
            return 0;
        }
        output.Write(std::format("Global package cache ({}):\n", Reporting::FormatCount(lines.size(), "version")));
        output.Detail(std::format("Cache: '{}'", cacheDir.string()));
        for (const auto &line : lines) {
            output.Write(std::format("  Installed {}\n", line));
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
        output.Success("Empty", "project dependency list");
        output.Detail(std::format("Manifest: '{}'", manifestPath->string()));
        return 0;
    }
    output.Write(std::format("Project dependencies ({}):\n",
                             Reporting::FormatCount(manifest->dependencies.size(), "dependency", "dependencies")));
    output.Detail(std::format("Manifest: '{}'", manifestPath->string()));
    for (const auto &dep : manifest->dependencies) {
        PrintDependency(dep, output);
    }
    return 0;
}

// TODO: Extend Package manifest metadata support
int Cli::RunInfo(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
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
            if (!ReadRegistryOption(args, i, registryArg, diagnostics)) {
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
    std::optional<SpecRequirement> selectedRequirement;
    std::optional<InstalledPackage> selectedInstallation;
    if (!opts.manifest.empty()) {
        manifestPath = opts.manifest;
        if (!std::filesystem::exists(manifestPath)) {
            diagnostics.Error(std::format("specified manifest '{}' was not found", manifestPath.string()));
            diagnostics.Help("check the path passed to '--manifest'");
            return JsonFailure("the specified manifest was not found");
        }
    }
    else if (packageSpec.empty()) {
        auto localManifestOpt = Manifest::Find(std::filesystem::current_path());
        if (!localManifestOpt) {
            diagnostics.Error("package name is required because no 'Rux.toml' was found in the current directory");
            diagnostics.Help("pass 'Namespace/Package' or run the command from a Rux project");
            return JsonFailure("missing package name and no Rux.toml was found");
        }
        manifestPath = *localManifestOpt;
    }
    else {
        auto spec = RequirementFromSpec(packageSpec, "info", diagnostics);
        if (!spec) {
            return JsonFailure("invalid package identity or version requirement");
        }
        selectedRequirement = *spec;
        const PackageRequirement &requirement = spec->requirement;
        const auto installed = FindInstalledPackage(requirement.ns, requirement.package, requirement.range);
        if (!installed) {
            // Not having it locally is worth distinguishing from it not
            // existing, so the registry is asked which versions there are.
            const std::string base = ResolveRegistryBase(registryArg);
            const std::string identity = Qualified(requirement.ns, requirement.package);
            auto entry = FetchPackageIndex(base, requirement.ns, requirement.package);
            if (!entry) {
                const auto problem = Describe(entry.error(), base, identity);
                diagnostics.Error(problem.message);
                for (const auto &note : problem.notes) {
                    diagnostics.Note(note);
                }
                diagnostics.Help(problem.help.value_or(
                    std::format("check the registry URL, then retry 'rux info {}'", packageSpec)));
                return JsonFailure("the registry lookup failed");
            }
            diagnostics.Error(
                std::format("package '{}' has no installed version matching '{}'", identity, requirement.range.Text()));
            diagnostics.Note(std::format("registry '{}': {}", base, DescribeAvailableVersions(*entry)));
            diagnostics.Note(std::format("global package cache: '{}'", RegistryPackagesDir().string()));
            diagnostics.Help(std::format("install it with '{}'",
                                         InstallCommand(requirement.ns, requirement.package, requirement.range)));
            return JsonFailure("no installed package version matches the requirement");
        }
        selectedInstallation = installed;
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
        if (selectedRequirement && selectedInstallation) {
            const auto &requirement = selectedRequirement->requirement;
            output.Write(std::format("Installed package '{}' selected by requirement '{}':\n",
                                     Qualified(requirement.ns, requirement.package), requirement.range.Text()));
            output.Detail(std::format("Cache: '{}'", selectedInstallation->root.string()));
        }
        else {
            output.Write(std::format("Project package from '{}':\n", manifestPath.string()));
        }
        if (manifest->package.ns) {
            output.Write(std::format("  Namespace:   {}\n", manifest->package.ns->Text()));
        }
        output.Write(std::format("  Name:        {}\n"
                                 "  Version:     {}\n"
                                 "  Type:        {}\n",
                                 manifest->package.name.Text(), manifest->package.version.Text(),
                                 ToString(manifest->package.type)));
        if (!manifest->package.keywords.empty()) {
            std::string keywords;
            for (std::size_t i = 0; i < manifest->package.keywords.size(); ++i) {
                keywords += i == 0 ? "" : ", ";
                keywords += manifest->package.keywords[i].Text();
            }
            output.Write(std::format("  Keywords:    {}\n", keywords));
        }
        if (!manifest->package.description.empty()) {
            output.Write(std::format("  Description: {}\n", manifest->package.description));
        }
        if (!manifest->package.authors.empty()) {
            std::string authors;
            for (std::size_t i = 0; i < manifest->package.authors.size(); ++i) {
                authors += i == 0 ? "" : ", ";
                authors += manifest->package.authors[i];
            }
            output.Write(std::format("  Authors:     {}\n", authors));
        }
        if (!manifest->package.license.empty()) {
            output.Write(std::format("  License:     {}\n", manifest->package.license));
        }
        if (!manifest->package.licenseFile.empty()) {
            output.Write(std::format("  LicenseFile: {}\n", manifest->package.licenseFile));
        }
        if (!manifest->package.repository.empty()) {
            output.Write(std::format("  Repository:  {}\n", manifest->package.repository));
        }
        if (!manifest->package.homepage.empty()) {
            output.Write(std::format("  Homepage:    {}\n", manifest->package.homepage));
        }
        output.Write(std::format("\nPackage dependencies ({}):\n",
                                 Reporting::FormatCount(manifest->dependencies.size(), "dependency", "dependencies")));
        if (manifest->dependencies.empty()) {
            output.Detail("None");
        }
        for (const auto &dep : manifest->dependencies) {
            PrintDependency(dep, output);
        }
    }
    return 0;
}
