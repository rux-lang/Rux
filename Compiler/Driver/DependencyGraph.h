#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Package/Manifest.h"
#include "Target/TargetTriple.h"

#include <filesystem>
#include <map>
#include <optional>
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
/// deduplicate packages; each owner's bindings retain the aliases declared by that owner's manifest.
class DependencyGraph {
public:
    DependencyGraph(Manifest rootManifest, std::filesystem::path manifestPath, Target::TargetTriple target,
                    std::span<const std::filesystem::path> localRoots = {}, bool localOnly = false);

    [[nodiscard]] const SourcePackage &Root() const;
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
    bool localOnly;
    std::string rootId;
    std::map<std::string, SourcePackage> packages;
    std::map<std::filesystem::path, Manifest> manifests;
    std::map<std::string, std::vector<std::filesystem::path>> localSources;
    std::map<std::string, std::map<std::string, std::string>> bindings;
    std::vector<Diagnostic> diagnostics;
};
} // namespace Rux::Driver
