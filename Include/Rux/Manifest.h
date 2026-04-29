/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <optional>

namespace Rux {

    struct Dependency {
        std::string name;
        std::string version; // empty = "latest"
    };

    struct Package {
        std::string name;
        std::string version = "0.1.0";
        std::string type    = "bin"; // "bin" | "lib"
    };

    struct Manifest {
        Package                  package;
        std::vector<Dependency>  dependencies;

        // Load from Rux.toml. Returns null on parse error.
        static std::optional<Manifest> Load(const std::filesystem::path& path);

        // Save to Rux.toml. Returns false on write error.
        [[nodiscard]] bool Save(const std::filesystem::path& path) const;

        // Add or update a dependency. Returns false if already present with same version.
        bool AddDependency(const std::string& name, const std::string& version);

        // Remove a dependency by name. Returns false if not found.
        bool RemoveDependency(const std::string& name);

        // Find the manifest file by walking up from the current directory.
        static std::optional<std::filesystem::path> Find(
            const std::filesystem::path& start = std::filesystem::current_path());
    };

    // Parse a package spec like "Json" or "[email protected]"
    // Returns {name, version} where version may be empty.
    std::pair<std::string, std::string> ParsePackageSpec(std::string_view spec);
}
