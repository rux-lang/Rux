#pragma once

#include "Package/Manifest.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace Rux {
/**
 * @brief Describes the package that ScaffoldPackage creates.
 *
 * The package kind is the manifest's own `ManifestPackageType`, so scaffolding
 * and manifest parsing agree on the four Version 1 kinds.
 */
struct ScaffoldOptions {
    std::filesystem::path root;                                 ///< Directory that receives the package.
    std::string name;                                           ///< Package name, validated as an identity segment.
    ManifestPackageType type = ManifestPackageType::Executable; ///< Executable or one of the library kinds.
    std::optional<IdentitySegment> ns = std::nullopt;           ///< Optional registry namespace.
    bool initMode = false;                                      ///< Do not fail when the directory already exists.
};

enum class ScaffoldErrorKind {
    ExistingDestination,
    InvalidName,
    CreateDirectory,
    WriteFile,
};

struct ScaffoldChanges {
    std::size_t directoryTreesCreated = 0;
    std::size_t filesWritten = 0;
    std::size_t filesPreserved = 0;
};

struct ScaffoldError {
    ScaffoldErrorKind kind;
    std::filesystem::path path;
    std::string detail;
    ScaffoldChanges partialChanges;
};

using ScaffoldResult = std::expected<ScaffoldChanges, ScaffoldError>;

/**
 * @brief Scaffolds a new Rux package structure.
 *
 * Creates standard directories, a Version 1 Rux.toml manifest, and a starter
 * source file matching the package kind.
 *
 * @param options Package location, identity and kind
 *
 * @return Counts of filesystem changes, or a structured failure. This package
 * layer never writes diagnostics; the caller owns presentation.
 */
[[nodiscard]] ScaffoldResult ScaffoldPackage(const ScaffoldOptions &options);
} // namespace Rux
