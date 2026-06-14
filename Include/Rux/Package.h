// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

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
        Source,        ///< Source-only package.
    };

    /**
     * @brief Scaffolds a new Rux package structure.
     *
     * Creates standard directories, a default Rux.toml manifest,
     * and starter source files depending on package type.
     *
     * @param root Project root directory
     * @param name Package name
     * @param type Package type (executable or library)
     * @param initMode If true, does not fail when directory already exists
     *
     * @return true on success, false on failure
     */
    bool ScaffoldPackage(const std::filesystem::path& root,
                         const std::string& name,
                         PackageType type,
                         bool initMode = false);
} // namespace Rux
