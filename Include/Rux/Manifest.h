/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

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
        std::string path; // for path-based deps: { Path = "..." }, empty if version-based
    };

    /**
     * @brief Package metadata section of the manifest.
     */
    struct Package {
        std::string name;

        /// Semantic version (default: 0.1.0)
        std::string version = "0.1.0";

        /// Package type: "bin" or "lib"
        std::string type = "bin";
    };

    /**
     * @brief Build configuration section.
     */
    struct Build {
        /// Output directory or artifact name.
        std::string output = "Bin";
    };

    /**
     * @brief Represents a parsed Rux.toml manifest.
     */
    struct Manifest {
        Package package;
        Build build;
        std::vector<Dependency> dependencies;
        std::map<std::string, std::vector<Dependency>> targetDependencies;

        /**
         * @brief Load a manifest from disk.
         * @param path Path to Rux.toml
         * @return Parsed manifest or std::nullopt on failure
         */
        static std::optional<Manifest> Load(const std::filesystem::path& path);

        /**
         * @brief Save manifest to disk.
         * @param path Output file path
         * @return true on success, false on failure
         */
        [[nodiscard]] bool Save(const std::filesystem::path& path) const;

        /**
         * @brief Add or update a registry dependency.
         * @return false if already exists with same version
         */
        bool AddDependency(const std::string& name, const std::string& version);

        /**
         * @brief Add or update a path-based dependency.
         * @return false if already exists with same path
         */
        bool AddPathDependency(const std::string& name, const std::string& path);

        /**
         * @brief Remove a dependency by name.
         * @return false if not found
         */
        bool RemoveDependency(const std::string& name);

        /**
         * @brief Get combined global and target-specific dependencies.
         * @param target Target name
         * @return Merged dependency list
         */
        [[nodiscard]] std::vector<Dependency> EffectiveDependencies(const std::string& target) const;

        /**
         * @brief Find a Rux.toml by walking up directories.
         * @param start Starting directory
         * @return Path if found, otherwise std::nullopt
         */
        static std::optional<std::filesystem::path>
        Find(const std::filesystem::path& start = std::filesystem::current_path());
    };

    /**
     * @brief Parse a package specification string.
     *
     * Examples:
     * @code
     * "Std"       -> { "Std", "" }
     * "Std@1.2.0" -> { "Std", "1.2.0" }
     * @endcode
     *
     * @param spec Package spec string
     * @return Pair of {name, version}
     */
    std::pair<std::string, std::string> ParsePackageSpec(std::string_view spec);
} // namespace Rux
