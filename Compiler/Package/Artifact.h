#pragma once

#include "Package/Manifest.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>

namespace Rux {
/// Extension of a published package archive.
inline constexpr std::string_view artifactExtension = ".ruxpkg";

/// Limits from the Artifact v1 contract, counted in bytes.
inline constexpr std::size_t artifactMaxBytes = 5 * 1024 * 1024;
inline constexpr std::size_t artifactMaxExpandedBytes = 10 * 1024 * 1024;
inline constexpr std::size_t artifactMaxEntries = 1024;
inline constexpr std::size_t artifactMaxFileBytes = 2 * 1024 * 1024;
inline constexpr std::size_t artifactMaxTextBytes = 1024 * 1024;
inline constexpr std::size_t artifactMaxEntryPathBytes = 2048;

/**
 * @brief A `.ruxpkg` archive and the manifest bytes it was built from.
 *
 * Publication uploads `manifestSource` beside `archive`, and the registry
 * rejects the pair unless the archive's root `Rux.toml` matches those bytes
 * exactly. Both therefore come from one read of the file on disk.
 */
struct PackageArtifact {
    /// Exact `Rux.toml` bytes, also stored at the archive root.
    std::string manifestSource;

    /// Complete archive bytes.
    std::string archive;

    /// Regular files in the archive, the manifest included.
    std::size_t fileCount = 0;

    /// Regular files whose path matches `Src/**/*.rux`.
    std::size_t sourceFileCount = 0;
};

/**
 * @brief Build the `.ruxpkg` archive for a package.
 *
 * Collects the manifest, every regular file below `Src/`, and the file named
 * by `Readme`. Entries are sorted by path and carry a fixed timestamp, so
 * building the same tree twice produces identical bytes.
 *
 * The caller is expected to have accepted the manifest under the publication
 * profile first; this function checks the archive contract rather than the
 * manifest one.
 *
 * @param manifestPath Path to the package's Rux.toml
 * @param manifest The manifest parsed from that path
 *
 * @return The artifact, or the reason the package cannot be packed
 */
[[nodiscard]] std::expected<PackageArtifact, std::string>
BuildPackageArtifact(const std::filesystem::path &manifestPath, const Manifest &manifest);

/**
 * @brief The default file name for a package's archive.
 *
 * Uses display spelling and the complete version text, so build metadata
 * survives into the file name: `Math-0.1.0.ruxpkg`.
 */
[[nodiscard]] std::string ArtifactFileName(const Manifest &manifest);
} // namespace Rux
