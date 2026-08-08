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
 * Collects the manifest, every regular file below `Src/`, and the files named
 * by `ReadmeFile` and `LicenseFile`. Entries are sorted by path and carry a
 * fixed timestamp, so building the same tree twice produces identical bytes.
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

/// What an archive turned out to contain once written to disk.
struct ExtractedArtifact {
    std::size_t fileCount = 0;
    std::size_t sourceFileCount = 0;
    std::size_t expandedBytes = 0;
};

/**
 * @brief Write the contents of a `.ruxpkg` archive into `dest`.
 *
 * Holds a downloaded archive to the same Artifact v1 contract that
 * BuildPackageArtifact enforces when packing: Stored entries only, a root
 * `Rux.toml`, at least one Rux source below `Src/`, portable entry paths, and
 * the declared size and count limits. Every entry's CRC-32 is verified, so a
 * truncated or altered archive is rejected rather than installed.
 *
 * Nothing is written until the whole archive has been validated. `dest` is
 * expected to be a fresh staging directory the caller commits afterwards.
 *
 * @param archive Complete archive bytes
 * @param dest Directory to write the entries into
 *
 * @return What was extracted, or the reason the archive was rejected
 */
[[nodiscard]] std::expected<ExtractedArtifact, std::string> ExtractPackageArtifact(std::string_view archive,
                                                                                   const std::filesystem::path &dest);
} // namespace Rux
