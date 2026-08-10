#pragma once

// Read side of the package registry contract: the resolver index, an exact
// version's checksum, and the artifact bytes themselves.
//
// The write side lives in Cli/CmdPublish and Driver/Credentials. Everything here
// is public and unauthenticated, so no credential is involved: installing a
// package never needs a token.
//
// Routes are relative to the base URL Driver::ResolveRegistryBase produces, so
// --registry and RUX_REGISTRY_URL retarget resolution and download exactly as
// they retarget publication.

#include "Package/Identity.h"
#include "Package/Version.h"
#include "Target/Target.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Driver {
/// One dependency edge as the index reports it.
struct RegistryDependencyEdge {
    IdentitySegment alias;
    IdentitySegment ns;
    IdentitySegment package;
    VersionRange range;
    std::vector<Target::OS> targetOS;

    [[nodiscard]] bool MatchesTarget(Target::OS os) const noexcept;
};

/// One published version of a package.
struct RegistryVersion {
    SemanticVersion version;

    /// Oldest compiler release that can build it, when the registry knows one.
    std::optional<SemanticVersion> minRux;

    /// Yanked versions stay listed so an existing install can still be
    /// identified; a new resolution must not select one.
    bool yanked = false;

    std::vector<RegistryDependencyEdge> dependencies;
};

/// A package's resolver index, versions in ascending registry order.
struct RegistryIndexEntry {
    IdentitySegment ns;
    IdentitySegment package;
    std::vector<RegistryVersion> versions;
};

/// Why a registry request did not produce what was asked for.
enum class RegistryErrorKind : std::uint8_t {
    /// No response arrived: DNS, connection or TLS failure.
    Unreachable,

    /// The package, or the exact version, does not exist.
    NotFound,

    /// A response arrived but could not be understood.
    Malformed,

    /// The registry answered with a problem document.
    Rejected,
};

/**
 * @brief A failed registry request, carrying whatever the registry explained.
 *
 * `code` is the RFC 9457 problem code when there was one, which is stable and
 * machine-readable; `detail` is the registry's own prose. The CLI turns these
 * into user-facing text.
 */
struct RegistryError {
    RegistryErrorKind kind = RegistryErrorKind::Unreachable;
    unsigned status = 0;
    std::string code;
    std::string detail;
};

/// Render a failure as one CLI error line, naming the registry and package.
[[nodiscard]] std::string Describe(const RegistryError &error, std::string_view base, std::string_view identity);

/// `Namespace/Name` in display spelling, for diagnostics.
[[nodiscard]] std::string QualifiedIdentity(const IdentitySegment &ns, const IdentitySegment &package);

/// Decode a `GET /v1/index/{ns}/{package}` body. Exposed so the decoding is
/// testable without a network.
[[nodiscard]] std::expected<RegistryIndexEntry, RegistryError> DecodePackageIndex(std::string_view body);

/// Decode the artifact digest out of an exact-version metadata body.
[[nodiscard]] std::expected<std::string, RegistryError> DecodeArtifactChecksum(std::string_view body);

/// Read a problem document into a failure of `kind`.
[[nodiscard]] RegistryError DecodeProblem(std::string_view body, unsigned status);

/// `GET {base}/v1/index/{ns}/{package}`.
[[nodiscard]] std::expected<RegistryIndexEntry, RegistryError>
FetchPackageIndex(std::string_view base, const IdentitySegment &ns, const IdentitySegment &package);

/// `GET {base}/v1/packages/{ns}/{package}/{version}`, reduced to the artifact's
/// lowercase-hex SHA-256.
[[nodiscard]] std::expected<std::string, RegistryError> FetchArtifactChecksum(std::string_view base,
                                                                              const IdentitySegment &ns,
                                                                              const IdentitySegment &package,
                                                                              const SemanticVersion &version);

/// `GET {base}/v1/packages/{ns}/{package}/{version}/download`, following the
/// redirect the registry answers with, to the complete `.ruxpkg` bytes.
[[nodiscard]] std::expected<std::string, RegistryError> DownloadArtifact(std::string_view base,
                                                                         const IdentitySegment &ns,
                                                                         const IdentitySegment &package,
                                                                         const SemanticVersion &version);

/// Why one published version was, or was not, a candidate.
enum class VersionRejection : std::uint8_t {
    /// Nothing rules it out.
    Eligible,

    /// It satisfies the requirements but has been withdrawn.
    Yanked,

    /// It satisfies the requirements but declares a newer compiler minimum.
    CompilerTooOld,

    /// No requirement problem beyond the version range itself.
    RequirementUnmet,
};

/**
 * @brief Decide whether one version could be selected, and why not when it could not.
 *
 * The order matters for diagnostics: a version is only reported as yanked or as
 * needing a newer compiler when the requirements would otherwise have accepted
 * it, so an error never blames a rule the user was nowhere near.
 */
[[nodiscard]] VersionRejection ClassifyVersion(const RegistryVersion &candidate, std::span<const VersionRange> ranges,
                                               const SemanticVersion &compiler);

/**
 * @brief Choose the version of `entry` that satisfies every requirement in `ranges`.
 *
 * Yanked versions are skipped, as is any version whose declared minimum
 * compiler release is newer than `compiler`. The highest remaining match wins,
 * ordered by SemVer precedence with build metadata as the tie-break, which is
 * the registry's own total order.
 *
 * @return The chosen version, or nullptr when nothing qualifies
 */
[[nodiscard]] const RegistryVersion *
SelectVersion(const RegistryIndexEntry &entry, std::span<const VersionRange> ranges, const SemanticVersion &compiler);

/// Convenience overload for a single requirement.
[[nodiscard]] const RegistryVersion *SelectVersion(const RegistryIndexEntry &entry, const VersionRange &range,
                                                   const SemanticVersion &compiler);

/// A failed resolution, split into the `error:` line and what follows it.
struct ResolutionFailure {
    /// The headline, already specific enough to stand alone where it can be.
    std::string message;

    /// Extra lines, indented under the headline. Empty when the headline said
    /// everything there was to say.
    std::vector<std::string> details;
};

/**
 * @brief Explain why no version of `entry` could be selected.
 *
 * Naming only the requirement would be misleading whenever a version did match
 * it and lost to the yank or compiler-minimum rule instead: the reader would be
 * told that nothing satisfies a requirement directly above the version that
 * does. So when one excluded version accounts for the whole failure, the reason
 * *is* the message and there is nothing below it; the requirement only leads
 * when several versions failed for different reasons, or when none matched at
 * all and the published list is the useful thing to show.
 */
[[nodiscard]] ResolutionFailure DescribeResolutionFailure(const RegistryIndexEntry &entry,
                                                          std::span<const VersionRange> ranges,
                                                          const SemanticVersion &compiler, std::string_view base);

/// Render accumulated requirements as `'^1.0.0'` or `'>=1.0.0', '<2.0.0'`.
[[nodiscard]] std::string DescribeRanges(std::span<const VersionRange> ranges);

/// Every version text in `entry`, ascending, for an error that has to say what
/// the registry does offer.
[[nodiscard]] std::string DescribeAvailableVersions(const RegistryIndexEntry &entry);

/// The compiler's own release, used as the `MinRux` ceiling during resolution.
[[nodiscard]] SemanticVersion CompilerVersion();
} // namespace Rux::Driver
