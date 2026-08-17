#pragma once

#include "Diagnostics/Diagnostics.h"
#include "SourceModel/SourceFile.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace Rux {
/// Result of a load operation. A file that could not be read is reported in `diagnostics` and left out of `files`, so a
/// single unreadable file does not abandon the rest of the package.
struct SourceLoadResult {
    std::vector<SourceFile> files;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool HasErrors() const noexcept;
};

class SourceLoader {
public:
    /**
     * @brief Load every `*.rux` file under a package's `Src` directory.
     *
     * The walk is recursive and its results are sorted, so module order is the same on every platform and a build is
     * reproducible. A missing or non-directory `Src` is an error; an existing but empty one is only a warning, since
     * the manifest may still be worth reporting on.
     *
     * @param manifestDir The directory that contains `Rux.toml`
     */
    [[nodiscard]] static SourceLoadResult Load(const std::filesystem::path &manifestDir);

    /**
     * @brief Load a single `*.rux` file by explicit path.
     *
     * The stored path is absolute, so diagnostics name the same file whatever directory the compiler was invoked from.
     *
     * @return The file, or nullopt when it cannot be opened, read, or resolved
     */
    [[nodiscard]] static std::optional<SourceFile> LoadFile(const std::filesystem::path &path);

private:
    /// Collect all `*.rux` paths under a directory tree, recursively. Errors accumulate into `diagnostics`; whatever
    /// was reachable is still returned.
    static std::vector<std::filesystem::path> CollectSourcePaths(const std::filesystem::path &srcDir,
                                                                 std::vector<Diagnostic> &diagnostics);
};
} // namespace Rux
