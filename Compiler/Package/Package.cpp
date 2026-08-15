#include "Package/Package.h"

#include "Package/Manifest.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux {
namespace fs = std::filesystem;

/**
 * @brief Writes a file with the given content.
 *
 * If @p skipIfExists is true and the file already exists,
 * the function succeeds without modifying it.
 *
 * @param path Output file path
 * @param content File content
 * @param skipIfExists Whether to skip writing if file already exists
 * @return A structured error on failure, otherwise no value
 */
static std::optional<ScaffoldError> WriteFile(const fs::path &path, const std::string_view content,
                                              const bool skipIfExists, ScaffoldChanges &changes) {
    std::error_code existsError;
    const bool exists = fs::exists(path, existsError);
    if (existsError) {
        return ScaffoldError{ScaffoldErrorKind::WriteFile, path, existsError.message(), changes};
    }
    if (skipIfExists && exists) {
        std::error_code typeError;
        const bool regularFile = fs::is_regular_file(path, typeError);
        if (typeError) {
            return ScaffoldError{ScaffoldErrorKind::WriteFile, path, typeError.message(), changes};
        }
        if (!regularFile) {
            return ScaffoldError{ScaffoldErrorKind::WriteFile, path, "the path exists but is not a regular file",
                                 changes};
        }
        ++changes.filesPreserved;
        return std::nullopt;
    }

    if (std::ofstream f{path, std::ios::binary}; f.write(content.data(), content.size())) {
        ++changes.filesWritten;
        return std::nullopt;
    }
    return ScaffoldError{ScaffoldErrorKind::WriteFile, path, "the file could not be opened or written", changes};
}

/**
 * @brief Creates a directory tree.
 *
 * @param path Directory path to create
 * @return A structured error on failure, otherwise no value
 */
static std::optional<ScaffoldError> MakeDir(const fs::path &path, ScaffoldChanges &changes) {
    std::error_code error;
    const bool created = fs::create_directories(path, error);
    if (error) {
        return ScaffoldError{ScaffoldErrorKind::CreateDirectory, path, error.message(), changes};
    }
    changes.directoryTreesCreated += static_cast<std::size_t>(created);
    return std::nullopt;
}

/**
 * @brief Names the starter source file for a package kind.
 *
 * Only an Executable carries an entry point, so it alone gets Main.rux. Library
 * packages expose a module instead.
 */
static std::string_view StarterFileName(const ManifestPackageType type) {
    return type == ManifestPackageType::Executable ? "Main.rux" : "Lib.rux";
}

/**
 * @brief Starter source matching the package kind.
 */
static std::string StarterSource(const ManifestPackageType type, const std::string &name) {
    if (type == ManifestPackageType::Executable) {
        return "func Main() -> int {\n    return 0;\n}\n";
    }
    return std::format("module {} {{\n}}\n", name);
}

ScaffoldResult ScaffoldPackage(const ScaffoldOptions &options) {
    const fs::path &root = options.root;

    // Validate the identity before touching the filesystem, so a rejected name
    // leaves no half-created package behind.
    const auto packageName = IdentitySegment::Parse(options.name);
    if (!packageName) {
        return std::unexpected(ScaffoldError{ScaffoldErrorKind::InvalidName, {}, Describe(packageName.error()), {}});
    }

    std::error_code existsError;
    const bool rootExists = fs::exists(root, existsError);
    if (existsError) {
        return std::unexpected(ScaffoldError{ScaffoldErrorKind::CreateDirectory, root, existsError.message(), {}});
    }
    if (!options.initMode && rootExists) {
        return std::unexpected(ScaffoldError{ScaffoldErrorKind::ExistingDestination, root, {}, {}});
    }

    ScaffoldChanges changes;

    // A SourceLibrary package produces no artifact of its own, so it needs no output
    // directories; it is compiled into whichever package depends on it.
    std::vector dirs = {root / "Src", root / "Temp"};
    if (options.type != ManifestPackageType::SourceLibrary) {
        dirs.insert(dirs.begin(), root / "Bin");
    }
    for (const auto &directory : dirs) {
        if (auto error = MakeDir(directory, changes)) {
            return std::unexpected(std::move(*error));
        }
    }

    const auto tomlPath = root / "Rux.toml";
    std::error_code manifestExistsError;
    const bool manifestExists = fs::exists(tomlPath, manifestExistsError);
    if (manifestExistsError) {
        return std::unexpected(
            ScaffoldError{ScaffoldErrorKind::WriteFile, tomlPath, manifestExistsError.message(), changes});
    }
    if (options.initMode && manifestExists) {
        std::error_code manifestTypeError;
        const bool regularManifest = fs::is_regular_file(tomlPath, manifestTypeError);
        if (manifestTypeError) {
            return std::unexpected(
                ScaffoldError{ScaffoldErrorKind::WriteFile, tomlPath, manifestTypeError.message(), changes});
        }
        if (!regularManifest) {
            return std::unexpected(ScaffoldError{ScaffoldErrorKind::WriteFile, tomlPath,
                                                 "the path exists but is not a regular file", changes});
        }
    }
    if (!options.initMode || !manifestExists) {
        Manifest m;
        m.package.ns = options.ns;
        m.package.name = *packageName;
        m.package.version = *SemanticVersion::Parse("0.1.0");
        m.package.type = options.type;
        if (!m.Save(tomlPath)) {
            return std::unexpected(ScaffoldError{ScaffoldErrorKind::WriteFile, tomlPath,
                                                 "the manifest could not be opened or written", changes});
        }
        ++changes.filesWritten;
    }
    else {
        ++changes.filesPreserved;
    }

    const std::string srcContent = StarterSource(options.type, packageName->Text());

    struct FileTask {
        fs::path path;
        std::string_view content;
    };

    const FileTask tasks[] = {{root / "Src" / StarterFileName(options.type), srcContent},
                              {root / ".gitignore", "# Rux build outputs\nBin/\nTemp/\n"}};

    for (const auto &task : tasks) {
        if (auto error = WriteFile(task.path, task.content, options.initMode, changes)) {
            return std::unexpected(std::move(*error));
        }
    }
    return changes;
}
} // namespace Rux
