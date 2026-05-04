/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <optional>

namespace Rux
{
    // Represents a single loaded source file.
    struct SourceFile
    {
        std::filesystem::path path; // Absolute path to the file
        std::string source; // Full file contents
    };

    // Result of a load operation.
    struct SourceLoadResult
    {
        std::vector<SourceFile> files;
        std::vector<std::string> errors; // Non-fatal per-file errors, if any
    };

    class SourceLoader
    {
    public:
        // Load all *.rux files from the Src/ directory of a package.
        // manifestDir  - the directory that contains Rux.toml
        // Returns nullopt if the Src/ directory does not exist or cannot be opened.
        [[nodiscard]] static std::optional<SourceLoadResult>
        Load(const std::filesystem::path& manifestDir);

        // Load a single *.rux file by explicit path.
        // Returns nullopt if the file cannot be opened.
        [[nodiscard]] static std::optional<SourceFile>
        LoadFile(const std::filesystem::path& path);

    private:
        // Collect all *.rux paths under a directory tree (recursive).
        static std::vector<std::filesystem::path>
        CollectSourcePaths(const std::filesystem::path& srcDir);
    };
}
