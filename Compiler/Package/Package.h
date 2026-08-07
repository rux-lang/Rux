#pragma once

#include "Package/Manifest.h"

#include <filesystem>
#include <optional>
#include <string>

namespace Rux {
/**
 * @brief Describes the package that ScaffoldPackage creates.
 *
 * The package kind is the manifest's own `ManifestPackageType`, so scaffolding
 * and manifest parsing agree on the three Version 1 kinds.
 */
struct ScaffoldOptions {
    std::filesystem::path root;                              ///< Directory that receives the package.
    std::string name;                                        ///< Package name, validated as an identity segment.
    ManifestPackageType type = ManifestPackageType::Program; ///< Program, Library or Source.
    std::optional<IdentitySegment> ns = std::nullopt;        ///< Optional registry namespace.
    bool initMode = false;                                   ///< Do not fail when the directory already exists.
};

/**
 * @brief Scaffolds a new Rux package structure.
 *
 * Creates standard directories, a Version 1 Rux.toml manifest, and a starter
 * source file matching the package kind.
 *
 * @param options Package location, identity and kind
 *
 * @return true on success, false on failure
 */
bool ScaffoldPackage(const ScaffoldOptions &options);
} // namespace Rux
