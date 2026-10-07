#pragma once

#include "Documentation/Generator.h"

#include <filesystem>
#include <span>
#include <string>

namespace Rux::Documentation {
/// One generated file, named relative to the output directory.
struct OutputFile {
    std::string name;
    std::string content;
};

/// Replace `outputDirectory` with exactly `files` and the `.rux-docs` marker that claims the directory.
///
/// A directory is replaced only while it is missing, empty, or already marked, so a mistyped `--output` cannot erase
/// unrelated files. Everything is written into a temporary sibling first and renamed into place, so a failed write
/// leaves the previous output intact.
[[nodiscard]] GenerateResult InstallManagedDirectory(const std::filesystem::path &outputDirectory,
                                                     std::span<const OutputFile> files);
} // namespace Rux::Documentation
