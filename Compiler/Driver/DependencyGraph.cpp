#include "Driver/DependencyGraph.h"

#include "Package/Cache.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Rux::Driver {
namespace {
std::filesystem::path CanonicalManifest(const std::filesystem::path &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::absolute(path).lexically_normal() : canonical;
}

std::string Identity(const Manifest &manifest, const std::filesystem::path &path) {
    if (manifest.package.ns)
        return manifest.package.ns->Normalized() + "/" + manifest.package.name.Normalized();
    return "local:" + path.generic_string();
}

std::string Describe(const SourcePackage &package) {
    return std::format("{}@{} from '{}'", package.id, package.manifest.package.version.Text(),
                       package.manifestPath.string());
}
} // namespace

DependencyGraph::DependencyGraph(Manifest rootManifest, std::filesystem::path manifestPath,
                                 const Target::TargetTriple inputTarget,
                                 const std::span<const std::filesystem::path> localRoots, const bool inputLocalOnly)
    : target(inputTarget)
    , localOnly(inputLocalOnly) {
    manifestPath = CanonicalManifest(manifestPath);
    rootId = Identity(rootManifest, manifestPath);
    manifests.emplace(manifestPath, rootManifest);
    packages.emplace(rootId, SourcePackage{rootId, manifestPath, std::move(rootManifest)});
    bindings[rootId];
    for (const auto &local : localRoots)
        AddLocal(local / "Rux.toml");
    for (const auto &dependency : Root().manifest.dependencies) {
        if (dependency.IsPath() && dependency.MatchesTarget(target.Os()))
            AddLocal(Root().Root() / dependency.Path() / "Rux.toml");
    }
}

const SourcePackage &DependencyGraph::Root() const {
    return packages.at(rootId);
}

void DependencyGraph::Fail(const SourcePackage &owner, std::string message, std::vector<std::string> notes,
                           std::optional<std::string> help) {
    auto diagnostic = ErrorDiagnostic(std::move(message), std::move(notes), std::move(help));
    diagnostic.sourceName = owner.manifestPath.string();
    diagnostics.push_back(std::move(diagnostic));
}

const Manifest *DependencyGraph::ReadManifest(const std::filesystem::path &path, const bool reportErrors) {
    if (const auto found = manifests.find(path); found != manifests.end())
        return &found->second;
    auto loaded = Manifest::Load(path);
    if (!loaded.Ok()) {
        if (reportErrors) {
            for (const auto &diagnostic : loaded.diagnostics) {
                diagnostics.push_back({Diagnostic::Severity::Error,
                                       diagnostic.path.string(),
                                       {.line = diagnostic.line, .column = diagnostic.column, .offset = 0},
                                       diagnostic.message,
                                       diagnostic.notes,
                                       diagnostic.help,
                                       diagnostic.documentationUrl});
            }
        }
        return nullptr;
    }
    return &manifests.emplace(path, std::move(*loaded.manifest)).first->second;
}

void DependencyGraph::AddLocal(const std::filesystem::path &path) {
    const auto canonical = CanonicalManifest(path);
    // An unused or conditionally excluded dependency must not fail a build merely because its path is absent.
    const auto *manifest = ReadManifest(canonical, false);
    if (!manifest || manifest->package.name.Empty())
        return;
    auto &sources = localSources[Identity(*manifest, canonical)];
    if (!std::ranges::contains(sources, canonical))
        sources.push_back(canonical);
}

const SourcePackage *DependencyGraph::Resolve(const SourcePackage &owner, const std::string_view importName) {
    auto &ownerBindings = bindings[owner.id];
    if (const auto found = ownerBindings.find(std::string(importName)); found != ownerBindings.end())
        return &packages.at(found->second);
    const auto dependency = std::ranges::find(owner.manifest.dependencies, importName,
                                              [](const ManifestDependency &entry) { return entry.importName.Text(); });
    if (dependency == owner.manifest.dependencies.end()) {
        if (importName == owner.manifest.package.name.Text()) {
            ownerBindings.emplace(importName, owner.id);
            return &owner;
        }
        Fail(owner,
             std::format("package '{}' is not listed in [Dependencies] of '{}'", importName,
                         owner.manifestPath.string()),
             {"the import requires a package dependency with the same import name"},
             "add the package under [Dependencies] or correct the import path");
        return nullptr;
    }
    if (!dependency->MatchesTarget(target.Os())) {
        Fail(owner, std::format("dependency '{}' is not available for target '{}'", importName, target.CanonicalName()),
             {std::format("TargetOS in '{}' excludes {}", owner.manifestPath.string(), Target::ToString(target.Os()))},
             "select an available target or include the target OS in the dependency's TargetOS list");
        return nullptr;
    }
    std::filesystem::path path;
    const auto *registry = dependency->Registry();
    if (dependency->IsPath()) {
        path = CanonicalManifest(owner.Root() / dependency->Path() / "Rux.toml");
    }
    else {
        const std::string id = registry->ns.Normalized() + "/" + dependency->package.Normalized();
        if (const auto local = localSources.find(id); local != localSources.end()) {
            if (local->second.size() != 1) {
                std::vector<std::string> notes;
                for (const auto &source : local->second)
                    notes.push_back(std::format("{} from '{}'", id, source.string()));
                Fail(owner, std::format("dependency '{}' has conflicting local sources for '{}'", importName, id),
                     std::move(notes), "select one local source for this package identity");
                return nullptr;
            }
            path = local->second.front();
        }
        else if (localOnly) {
            Fail(owner,
                 std::format("package '{}' is not a local workspace member; registry dependencies are disabled",
                             registry->ns.Text() + "/" + dependency->package.Text()),
                 {"workspace checks resolve registry declarations only from matching local members"},
                 "add the package to [Workspace].Packages or use a local Path dependency");
            return nullptr;
        }
        else {
            const auto installed = Packages::FindInstalledPackage(registry->ns, dependency->package, registry->version);
            if (!installed) {
                std::string present;
                for (const auto &candidate : Packages::InstalledVersions(registry->ns, dependency->package))
                    present += (present.empty() ? "" : ", ") + candidate.version.Text();
                Fail(owner,
                     std::format("no installed version of '{}/{}' satisfies '{}'", registry->ns.Text(),
                                 dependency->package.Text(), registry->version.Text()),
                     {present.empty() ? "no versions are installed" : "installed versions: " + present},
                     "run 'rux install' to resolve and cache the dependency");
                return nullptr;
            }
            path = CanonicalManifest(installed->root / "Rux.toml");
        }
    }
    const Manifest *manifest = ReadManifest(path, true);
    if (!manifest) {
        Fail(owner,
             std::format("cannot load dependency package '{}' from '{}'", importName, path.parent_path().string()),
             {"the dependency manifest is missing or invalid"}, "check the dependency path and its Rux.toml manifest");
        return nullptr;
    }
    const std::string id = Identity(*manifest, path);
    if (registry && (id != registry->ns.Normalized() + "/" + dependency->package.Normalized() ||
                     !registry->version.Matches(manifest->package.version))) {
        Fail(owner,
             std::format("dependency '{}' requires {}/{}@{} but selected {}@{}", importName, registry->ns.Text(),
                         dependency->package.Text(), registry->version.Text(), id, manifest->package.version.Text()),
             {"selected source: '" + path.string() + "'", "declaring manifest: '" + owner.manifestPath.string() + "'"});
        return nullptr;
    }
    const auto existing = packages.find(id);
    if (existing != packages.end() && existing->second.manifestPath != path) {
        Fail(owner, std::format("dependency '{}' resolves '{}' from incompatible sources", importName, id),
             {"already selected: " + Describe(existing->second),
              "requested: " + Describe(SourcePackage{id, path, *manifest})});
        return nullptr;
    }
    const auto selected = packages.try_emplace(id, SourcePackage{id, path, *manifest}).first;
    ownerBindings.emplace(importName, id);
    bindings[id];
    return &selected->second;
}
} // namespace Rux::Driver
