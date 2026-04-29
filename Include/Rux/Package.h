/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include <filesystem>
#include <string>

namespace Rux {
    enum class PackageType { Bin, Lib };

    // Creates the full directory/file scaffold for a new package.
    // root      - directory to create/populate
    // name      - package name
    // type      - Bin or Lib
    // initMode  - true = rux init (don't create the root dir itself,
    //             skip files that already exist)
    // Returns true on success.
    bool ScaffoldPackage(const std::filesystem::path &root,
                         const std::string &name,
                         PackageType type,
                         bool initMode = false);
}
