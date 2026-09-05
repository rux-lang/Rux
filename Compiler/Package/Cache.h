#pragma once
#include "Package/Identity.h"
#include "Package/Version.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace Rux::Packages {
/// Per-user directory where installed registry packages are cached.
[[nodiscard]] std::filesystem::path RegistryPackagesDir();

/// Cache directory of one package: <cache>/<namespace>/<name>. An existing directory is found by comparing normalized
/// names, so `Rux/My_Pkg` and `rux/my-pkg` resolve to one directory the way they share one registry entry. When nothing
/// is installed yet, the path carries the display spelling, which is the name an install creates.
///
/// This reads the filesystem rather than only composing a path, because the spelling on disk is the publisher's and the
/// spelling asked for is the consuming manifest's, and the two need not match.
[[nodiscard]] std::filesystem::path RegistryPackageParentDir(const IdentitySegment &ns, const IdentitySegment &name);

/// Cache directory of one exact version: <cache>/<namespace>/<name>/<version>, resolved as RegistryPackageParentDir
/// does, with the version keeping its exact text including build metadata.
[[nodiscard]] std::filesystem::path RegistryPackageDir(const IdentitySegment &ns, const IdentitySegment &name,
                                                       const SemanticVersion &version);

/// An installed version of a package, paired with the directory holding it.
struct InstalledPackage {
    SemanticVersion version;
    std::filesystem::path root;
};

/// Every installed version of one package, ascending. Directory names that are not semantic versions are ignored, so a
/// cache entry left by an older layout is inert rather than a failure.
[[nodiscard]] std::vector<InstalledPackage> InstalledVersions(const IdentitySegment &ns, const IdentitySegment &name);

/// The installed version a requirement resolves to: the highest one it matches, or nullopt when none is installed.
/// Build and check use this instead of contacting the registry, so a build never needs the network.
[[nodiscard]] std::optional<InstalledPackage>
FindInstalledPackage(const IdentitySegment &ns, const IdentitySegment &name, const VersionRange &range);
} // namespace Rux::Packages
