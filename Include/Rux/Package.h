/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include <filesystem>
#include <string>

namespace Rux {
    /**
     * @brief Defines the architectural nature of a Rux package.
     */
    enum class PackageType {
        Executable, ///< Standard binary output (e.g., CLI tools, applications).
        SharedLibrary, ///< Dynamically linked library (.so/.dll/.dylib).
        StaticLibrary, ///< Statically linked library archive (.a/.lib).
        Source ///< Source-only package.
    };

    /**
     * @brief Initializes a new Rux project structure.
     *
     * Creates the standard directory hierarchy (Bin, Src, Temp) and
     * generates default project files (Rux.toml, .gitignore).
     *
     * @param root      Path to the project folder.
     * @param name      Name of the package.
     * @param type      The project's build target (executable, library, etc.).
     * @param initMode  If true, attempts to scaffold in an existing directory
     * without overwriting existing files.
     *
     * @return true on success, false if any file or directory creation fails.
     */
    bool ScaffoldPackage(const std::filesystem::path& root,
                         const std::string& name,
                         PackageType type,
                         bool initMode = false);
} // namespace Rux
