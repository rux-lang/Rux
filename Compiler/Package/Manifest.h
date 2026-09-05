#pragma once

#include "Package/Identity.h"
#include "Package/Problem.h"
#include "Package/Version.h"
#include "Target/Target.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Rux {
/// The only manifest schema version this compiler accepts.
inline constexpr int manifestSchemaVersion = 1;

/// Manifest source larger than this is rejected before parsing.
inline constexpr std::size_t manifestMaxBytes = 65536;

/// Longest accepted metadata URL.
inline constexpr std::size_t manifestMaxUrlBytes = 2048;

inline constexpr std::size_t manifestMaxDependencies = 256;
inline constexpr std::size_t manifestMaxWorkspacePackages = 256;
inline constexpr std::size_t manifestMaxDefines = 128;
inline constexpr std::size_t manifestMaxAuthors = 32;
inline constexpr std::size_t manifestMaxKeywords = 32;

/// Stable reference for the Version 1 manifest contract.
inline constexpr std::string_view manifestDocumentationUrl = "https://rux-lang.dev/docs/manifest";

/**
 * @brief What a package builds.
 */
enum class ManifestPackageType {
    /// A runnable executable with a Main entry point.
    Executable,

    /// A shared library linked by dependents and loaded at run time.
    SharedLibrary,

    /// A target-native static library archive.
    StaticLibrary,

    /// Rux sources compiled directly into dependent packages.
    SourceLibrary,
};

[[nodiscard]] std::string_view ToString(ManifestPackageType type) noexcept;

[[nodiscard]] std::optional<ManifestPackageType> ParseManifestPackageType(std::string_view value) noexcept;

/// Canonical manifest spelling of an operating system accepted by `TargetOS`.
[[nodiscard]] std::string_view ManifestTargetOSName(Target::OS os) noexcept;

/// Parse one exact operating-system name accepted by `TargetOS`.
[[nodiscard]] std::optional<Target::OS> ParseManifestTargetOS(std::string_view value) noexcept;

/**
 * @brief A manifest problem located in the file it came from.
 *
 * Package code does not depend on the compiler's diagnostic component, so the
 * CLI and driver translate these into their own diagnostic type for display.
 */
struct ManifestDiagnostic {
    std::filesystem::path path;

    /// 1-based line and column of the rejected token.
    std::uint32_t line = 1;
    std::uint32_t column = 1;

    std::string message;

    /// Corrective context and the stable schema reference, when applicable.
    std::vector<std::string> notes;
    std::optional<std::string> help;
    std::optional<std::string> documentationUrl;

    /// The source line containing the rejected token, retained for human rendering.
    std::optional<std::string> sourceLine;

    /// Render the legacy location-and-message form used by programmatic callers.
    [[nodiscard]] std::string Format() const;

    /// Render the complete human diagnostic, including a source frame and corrective context.
    [[nodiscard]] std::string Render() const;
};

/**
 * @brief The `[Manifest]` schema header.
 *
 * `schemaVersion` is the manifest schema version, distinct from the package's
 * own `[Package].Version`. `minRux` is the oldest compiler release that can
 * build the package; it is optional locally and required to publish.
 */
struct ManifestHeader {
    int schemaVersion = manifestSchemaVersion;
    std::optional<SemanticVersion> minRux;
};

/**
 * @brief A dependency resolved from the package registry.
 */
struct RegistryDependencySource {
    IdentitySegment ns;
    VersionRange version;
};

/**
 * @brief A dependency resolved from a local directory.
 */
struct PathDependencySource {
    std::string path;
};

/**
 * @brief A typed `[Dependencies]` entry.
 *
 * `importName` is the table key and the name the dependency is imported under.
 * `package` is the dependency's own package name, which defaults to the import
 * name and differs only when the entry sets `Package`.
 */
struct ManifestDependency {
    IdentitySegment importName;
    IdentitySegment package;
    std::variant<RegistryDependencySource, PathDependencySource> source;
    std::vector<Target::OS> targetOS;

    [[nodiscard]] bool IsPath() const noexcept {
        return std::holds_alternative<PathDependencySource>(source);
    }

    /// The local directory of a path dependency, or an empty string.
    [[nodiscard]] const std::string &Path() const noexcept;

    /// The registry source, or nullptr for a path dependency.
    [[nodiscard]] const RegistryDependencySource *Registry() const noexcept {
        return std::get_if<RegistryDependencySource>(&source);
    }

    /// Whether this dependency is available to `os`; an empty allow-list means every target.
    [[nodiscard]] bool MatchesTarget(Target::OS os) const noexcept;
};

/**
 * @brief Package metadata section of the manifest.
 */
struct Package {
    /// Registry namespace; unset for a local-only package.
    std::optional<IdentitySegment> ns;

    IdentitySegment name;
    SemanticVersion version;
    ManifestPackageType type = ManifestPackageType::Executable;

    std::string description;
    std::vector<std::string> authors;
    std::vector<IdentitySegment> keywords;

    /// SPDX expression naming the terms.
    ///
    /// Independent of `licenseFile`: a manifest may declare the expression,
    /// the file, both, or neither.
    std::string license;

    /// Package-relative path to the license text carried in the package.
    std::string licenseFile;

    std::string repository;
    std::string homepage;

    /// Package-relative path to a readme file.
    std::string readmeFile;
};

/**
 * @brief Workspace section of the manifest.
 *
 * A workspace manifest groups several member packages under one root
 * `Rux.toml`. It carries no `[Package]` of its own; instead it lists the
 * relative paths of the member packages it owns.
 */
struct Workspace {
    /// Relative paths (from the manifest directory) of member packages.
    std::vector<std::string> packages;
};

/**
 * @brief One `[Build.Defines]` value.
 *
 * The declared TOML type is retained so canonical serialization reproduces the
 * spelling it read. Compile-time configuration consumes the text form.
 */
struct DefineValue {
    enum class Kind {
        String,
        Boolean,
        Integer,
    };

    Kind kind = Kind::String;
    std::string text;
};

/**
 * @brief Build configuration section.
 */
struct Build {
    /// Output directory, relative to the manifest unless absolute.
    std::string output = "Bin";

    /// User-defined compile-time values exposed through the Rux package's `config` value.
    std::map<std::string, DefineValue> defines;

    /// The defines as plain text, in the form compile-time configuration expects.
    [[nodiscard]] std::map<std::string, std::string> ConfigValues() const;
};

struct ManifestResult;

/**
 * @brief A parsed Version 1 `Rux.toml`.
 */
struct Manifest {
    ManifestHeader header;
    Package package;
    Build build;
    std::vector<ManifestDependency> dependencies;
    Workspace workspace;

    /**
     * @brief Whether this manifest describes a workspace rather than a package.
     *
     * `[Package]` and `[Workspace]` are mutually exclusive, and a workspace
     * lists at least one member.
     */
    [[nodiscard]] bool IsWorkspace() const noexcept {
        return !workspace.packages.empty();
    }

    /**
     * @brief Load and validate a manifest from disk.
     * @param path Path to Rux.toml
     */
    [[nodiscard]] static ManifestResult Load(const std::filesystem::path &path);

    /**
     * @brief Validate manifest text that is already in memory.
     * @param text Manifest source
     * @param path Path used to locate diagnostics
     */
    [[nodiscard]] static ManifestResult Parse(std::string_view text, const std::filesystem::path &path);

    /**
     * @brief Render the manifest in canonical form.
     */
    [[nodiscard]] std::string Serialize() const;

    /**
     * @brief Write the canonical form to disk.
     * @return false when the file could not be written
     */
    [[nodiscard]] bool Save(const std::filesystem::path &path) const;

    /// Find a dependency by normalized import name.
    [[nodiscard]] const ManifestDependency *FindDependency(const IdentitySegment &importName) const;

    /**
     * @brief Add or replace a registry dependency.
     * @return false when an identical entry already exists
     */
    bool AddRegistryDependency(IdentitySegment importName, IdentitySegment ns, VersionRange version);

    /**
     * @brief Add or replace a path dependency.
     * @return false when an identical entry already exists
     */
    bool AddPathDependency(IdentitySegment importName, std::string path);

    /**
     * @brief Remove a dependency by normalized import name.
     * @return false when no such dependency exists
     */
    bool RemoveDependency(const IdentitySegment &importName);

    /**
     * @brief Find a Rux.toml by walking up directories.
     * @param start Starting directory
     * @return Path if found, otherwise std::nullopt
     */
    static std::optional<std::filesystem::path>
    Find(const std::filesystem::path &start = std::filesystem::current_path());
};

/**
 * @brief The outcome of loading or parsing a manifest.
 *
 * A failed load carries at least one diagnostic and no manifest. A successful
 * load carries a manifest and no diagnostics. Syntax failures stop parsing;
 * schema validation accumulates independent failures in source order.
 */
struct ManifestResult {
    std::optional<Manifest> manifest;
    std::vector<ManifestDiagnostic> diagnostics;

    [[nodiscard]] bool Ok() const noexcept {
        return manifest.has_value();
    }
};

/**
 * @brief Discover package manifests owned by a workspace without a root manifest.
 *
 * A manifest-less workspace may keep test packages below Tests/ and package
 * members in immediate child directories. Member test packages are discovered
 * below each member's Tests/ directory as well. Group directories below a
 * Tests/ root are searched to the same bounded depth used by `rux test`.
 */
[[nodiscard]] std::vector<std::filesystem::path>
DiscoverManifestlessWorkspaceManifests(const std::filesystem::path &root = std::filesystem::current_path());

/**
 * @brief A package reference written on the command line.
 *
 * `Namespace/Name@requirement` names a registry package; the namespace and the
 * requirement are both optional in the spelling and absent here when omitted.
 */
struct PackageSpec {
    std::optional<IdentitySegment> ns;
    IdentitySegment name;
    std::optional<VersionRange> version;
};

/**
 * @brief Parse `[Namespace/]Name[@requirement]`.
 * @return The parsed reference, or an explanation of what was rejected
 */
[[nodiscard]] std::expected<PackageSpec, PackageProblem> ParsePackageSpec(std::string_view spec);
} // namespace Rux
