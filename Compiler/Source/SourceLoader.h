#pragma once

#include "Diagnostics/Diagnostics.h"
#include "SourceModel/SourceFile.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace Rux {
// Result of a load operation.
struct SourceLoadResult {
    std::vector<SourceFile> files;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool HasErrors() const noexcept;
};

class SourceLoader {
public:
    // Load all *.rux files from the Src/ directory of a package.
    // manifestDir  - the directory that contains Rux.toml
    [[nodiscard]] static SourceLoadResult Load(const std::filesystem::path &manifestDir);

    // Load a single *.rux file by explicit path.
    // Returns nullopt if the file cannot be opened.
    [[nodiscard]] static std::optional<SourceFile> LoadFile(const std::filesystem::path &path);

private:
    // Collect all *.rux paths under a directory tree (recursive).
    static std::vector<std::filesystem::path> CollectSourcePaths(const std::filesystem::path &srcDir,
                                                                 std::vector<Diagnostic> &diagnostics);
};
} // namespace Rux
