#pragma once

#include "Package/Identity.h"
#include "Package/Version.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Rux {
/// The only manifest schema version this compiler accepts.
inline constexpr int manifestSchemaVersion = 1;

/**
 * @brief What a package builds.
 */
enum class ManifestPackageType {
    /// A runnable executable with a Main entry point.
    Program,

    /// A shared library linked by dependents and loaded at run time.
    Library,

    /// Rux sources compiled directly into dependent packages.
    Source,
};

[[nodiscard]] std::string_view ToString(ManifestPackageType type) noexcept;

[[nodiscard]] std::optional<ManifestPackageType> ParseManifestPackageType(std::string_view value) noexcept;

/**
 * @brief The `[Manifest]` schema header.
 *
 * `schemaVersion` is the manifest schema version, distinct from the package's
 * own `[Package].Version`. `minRux` is the oldest compiler release that can
 * build the package; it is optional locally and required to publish.
 */
struct ManifestHeader {
    int schemaVersion = 0;
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

    [[nodiscard]] bool IsPath() const noexcept {
        return std::holds_alternative<PathDependencySource>(source);
    }
};

/**
 * @brief A dependency entry as read by the pre-Version 1 line parser.
 *
 * A dependency can either be:
 *  - version-based (version is set, path is empty)
 *  - path-based (path is set, version is ignored)
 *
 * The strict parser replaces this with ManifestDependency, which carries
 * validated identities and a typed version requirement instead of raw strings.
 */
struct Dependency {
    std::string name;
    std::string package; // registry/package name; empty means same as name
    std::string version; // empty = "latest"
    std::string path;    // for path-based deps: { Path = "..." }, empty if
    // version-based
};

/**
 * @brief Package metadata section of the manifest.
 */
struct Package {
    /// Registry namespace; unset for a local-only package.
    std::optional<IdentitySegment> ns;

    std::string name;

    /// Semantic version (default: 0.1.0)
    std::string version = "0.1.0";

    /// Package type: "bin", "sharedlib", or "dll" (Windows PE32+ shared
    /// library)
    std::string type = "bin";

    std::string description;
    std::vector<std::string> authors;
    std::vector<std::string> keywords;
    std::string license;

    /// Package-relative path to a license file; mutually exclusive with license.
    std::string licenseFile;

    std::string repository;
    std::string homepage;

    /// Package-relative path to a readme file.
    std::string readme;
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
 * @brief Build configuration section.
 */
struct Build {
    /// Output directory or artifact name.
    std::string output = "Bin";

    /// User-defined compile-time values exposed through the Rux package's `config` value.
    std::map<std::string, std::string> defines;
};

/**
 * @brief Represents a parsed Rux.toml manifest.
 */
struct Manifest {
    ManifestHeader header;
    Package package;
    Build build;
    std::vector<Dependency> dependencies;
    Workspace workspace;

    /**
     * @brief Whether this manifest describes a workspace rather than a package.
     *
     * A workspace manifest declares `[Workspace]` with one or more member
     * packages and has no `[Package]` of its own.
     */
    [[nodiscard]] bool IsWorkspace() const noexcept {
        return package.name.empty() && !workspace.packages.empty();
    }

    /**
     * @brief Load a manifest from disk.
     * @param path Path to Rux.toml
     * @return Parsed manifest or std::nullopt on failure
     */
    static std::optional<Manifest> Load(const std::filesystem::path &path);

    /**
     * @brief Save manifest to disk.
     * @param path Output file path
     * @return true on success, false on failure
     */
    [[nodiscard]] bool Save(const std::filesystem::path &path) const;

    /**
     * @brief Add or update a registry dependency.
     * @return false if already exists with same version
     */
    bool AddDependency(const std::string &name, const std::string &version);

    /**
     * @brief Add or update a path-based dependency.
     * @return false if already exists with same path
     */
    bool AddPathDependency(const std::string &name, const std::string &path);

    /**
     * @brief Remove a dependency by name.
     * @return false if not found
     */
    bool RemoveDependency(const std::string &name);

    /**
     * @brief Find a Rux.toml by walking up directories.
     * @param start Starting directory
     * @return Path if found, otherwise std::nullopt
     */
    static std::optional<std::filesystem::path>
    Find(const std::filesystem::path &start = std::filesystem::current_path());
};

/**
 * @brief Discover package manifests owned by a workspace without a root manifest.
 *
 * A manifest-less workspace
 * may keep test packages below Tests/ and package
 * members in immediate child directories. Member test packages are
 * discovered
 * below each member's Tests/ directory as well. Group directories below a
 * Tests/ root are searched to
 * the same bounded depth used by `rux test`.
 */
[[nodiscard]] std::vector<std::filesystem::path>
DiscoverManifestlessWorkspaceManifests(const std::filesystem::path &root = std::filesystem::current_path());

/**
 * @brief Parse a package specification string.
 *
 * Examples:
 * @code
 * "Rux"       -> { "Rux", "" }
 * "Rux@1.2.0" -> { "Rux", "1.2.0" }
 * @endcode
 *
 * @param spec Package spec string
 * @return Pair of {name, version}
 */
std::pair<std::string, std::string> ParsePackageSpec(std::string_view spec);
} // namespace Rux
