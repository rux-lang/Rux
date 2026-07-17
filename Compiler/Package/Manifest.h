#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Rux {
/**
 * @brief A dependency entry in a Rux.toml manifest.
 *
 * A dependency can either be:
 *  - version-based (version is set, path is empty)
 *  - path-based (path is set, version is ignored)
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
    std::string name;

    /// Semantic version (default: 0.1.0)
    std::string version = "0.1.0";

    /// Package type: "bin", "sharedlib", or "dll" (Windows PE32+ shared
    /// library)
    std::string type = "bin";

    std::string description;
    std::string authors;
    std::string license;
    std::string repository;
    std::string homepage;
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
