#include "Package/Package.h"

#include "Package/Manifest.h"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <string_view>
#include <vector>

namespace Rux {
namespace fs = std::filesystem;
using ScaffoldResult = std::expected<void, std::string>;

/**
 * @brief Writes a file with the given content.
 *
 * If @p skipIfExists is true and the file already exists,
 * the function succeeds without modifying it.
 *
 * @param path Output file path
 * @param content File content
 * @param skipIfExists Whether to skip writing if file already exists
 * @return std::expected<void, std::string> error message on failure
 */
static ScaffoldResult WriteFile(const fs::path &path, const std::string_view content, const bool skipIfExists) {
    if (skipIfExists && fs::exists(path)) {
        return {};
    }

    if (std::ofstream f{path, std::ios::binary}; f.write(content.data(), content.size())) {
        return {};
    }
    return std::unexpected(std::format("failed to write file: {}", path.string()));
}

/**
 * @brief Creates a directory tree.
 *
 * @param path Directory path to create
 * @return std::expected<void, std::string> error message on failure
 */
static ScaffoldResult MakeDir(const fs::path &path) {
    if (std::error_code ec; fs::create_directories(path, ec) || !ec) {
        return {};
    }
    return std::unexpected(std::format("failed to create directory: {}", path.string()));
}

/**
 * @brief Names the starter source file for a package kind.
 *
 * Only a Program carries an entry point, so it alone gets Main.rux. Library and
 * Source packages expose a module instead.
 */
static std::string_view StarterFileName(const ManifestPackageType type) {
    return type == ManifestPackageType::Program ? "Main.rux" : "Lib.rux";
}

/**
 * @brief Starter source matching the package kind.
 */
static std::string StarterSource(const ManifestPackageType type, const std::string &name) {
    if (type == ManifestPackageType::Program) {
        return "func Main() -> int {\n    return 0;\n}\n";
    }
    return std::format("module {} {{\n}}\n", name);
}

bool ScaffoldPackage(const ScaffoldOptions &options) {
    const fs::path &root = options.root;
    if (!options.initMode && fs::exists(root)) {
        std::println(stderr, "error: directory '{}' already exists", root.string());
        return false;
    }

    // Validate the identity before touching the filesystem, so a rejected name
    // leaves no half-created package behind.
    const auto packageName = IdentitySegment::Parse(options.name);
    if (!packageName) {
        std::println(stderr, "error: '{}' is not a valid package name: {}", options.name,
                     Describe(packageName.error()));
        return false;
    }

    auto run_task = [](auto &&task_result) -> bool {
        if (!task_result) {
            std::println(stderr, "error: {}", task_result.error());
            return false;
        }
        return true;
    };

    // A Source package produces no artifact of its own, so it needs no output
    // directories; it is compiled into whichever package depends on it.
    std::vector dirs = {root / "Src", root / "Temp"};
    if (options.type != ManifestPackageType::Source) {
        dirs.insert(dirs.begin(), {root / "Bin/Debug", root / "Bin/Release"});
    }
    if (!std::ranges::all_of(dirs, [&](const auto &p) { return run_task(MakeDir(p)); })) {
        return false;
    }

    if (const auto tomlPath = root / "Rux.toml"; !options.initMode || !fs::exists(tomlPath)) {
        Manifest m;
        m.package.ns = options.ns;
        m.package.name = *packageName;
        m.package.version = *SemanticVersion::Parse("0.1.0");
        m.package.type = options.type;
        if (!m.Save(tomlPath)) {
            std::println(stderr, "error: cannot write Rux.toml");
            return false;
        }
    }

    const std::string srcContent = StarterSource(options.type, packageName->Text());

    struct FileTask {
        fs::path path;
        std::string_view content;
    };

    const FileTask tasks[] = {{root / "Src" / StarterFileName(options.type), srcContent},
                              {root / ".gitignore", "# Rux build outputs\nBin/\nTemp/\n"}};

    return std::ranges::all_of(tasks,
                               [&](const auto &t) { return run_task(WriteFile(t.path, t.content, options.initMode)); });
}
} // namespace Rux
