#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Package/Manifest.h"
#include "Target/TargetTriple.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Driver {
/// A package's normalized identity and the independently selected manifest supplying its sources.
struct SourcePackage {
    std::string id;
    std::filesystem::path manifestPath;
    Manifest manifest;

    [[nodiscard]] std::filesystem::path Root() const {
        return manifestPath.parent_path();
    }
};

/// Resolves only requested imports, without source parsing, network access, or terminal output. Node identities
/// deduplicate packages; each owner's bindings retain the aliases declared by that owner's manifest. A registry
/// dependency resolves from a local root of the same identity, else from the package cache, unless its normalized
/// namespace is one of `localNamespaces`: a workspace owns the namespaces its members declare, so a package in one of
/// them that is not a member is an error rather than a cache lookup.
class DependencyGraph {
public:
    DependencyGraph(Manifest rootManifest, std::filesystem::path manifestPath, Target::TargetTriple target,
                    std::span<const std::filesystem::path> localRoots = {}, std::set<std::string> localNamespaces = {});

    [[nodiscard]] const SourcePackage &Root() const;
    /// The resolved package with identity `id`, or null when nothing resolved to it.
    [[nodiscard]] const SourcePackage *Find(const std::string &id) const;
    [[nodiscard]] const SourcePackage *Resolve(const SourcePackage &owner, std::string_view importName);

    [[nodiscard]] const auto &Bindings() const {
        return bindings;
    }

    [[nodiscard]] const auto &Diagnostics() const {
        return diagnostics;
    }

private:
    [[nodiscard]] const Manifest *ReadManifest(const std::filesystem::path &path, bool reportErrors);
    void AddLocal(const std::filesystem::path &path);
    void Fail(const SourcePackage &owner, std::string message, std::vector<std::string> notes = {},
              std::optional<std::string> help = {});

    Target::TargetTriple target;
    std::set<std::string> localNamespaces;
    std::string rootId;
    std::map<std::string, SourcePackage> packages;
    std::map<std::filesystem::path, Manifest> manifests;
    std::map<std::string, std::vector<std::filesystem::path>> localSources;
    std::map<std::string, std::map<std::string, std::string>> bindings;
    std::vector<Diagnostic> diagnostics;
};
} // namespace Rux::Driver
