/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

#include "Rux/Package.h"

#include "Rux/Manifest.h"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <fstream>
#include <print>
#include <ranges>
#include <string_view>
#include <vector>

namespace Rux {
    namespace fs = std::filesystem;
    using ScaffoldResult = std::expected<void, std::string>;

    static ScaffoldResult WriteFile(const fs::path& path, const std::string_view content, const bool skipIfExists) {
        if (skipIfExists && fs::exists(path)) return {};

        if (std::ofstream f{path, std::ios::binary}; f.write(content.data(), content.size())) {
            return {};
        }
        return std::unexpected(std::format("failed to write file: {}", path.string()));
    }

    static ScaffoldResult MakeDir(const fs::path& path) {
        if (std::error_code ec; fs::create_directories(path, ec) || !ec) return {};
        return std::unexpected(std::format("failed to create directory: {}", path.string()));
    }

    bool ScaffoldPackage(const fs::path& root, const std::string& name, const PackageType type, const bool initMode) {
        if (!initMode && fs::exists(root)) {
            std::println(stderr, "error: directory '{}' already exists", root.string());
            return false;
        }

        auto run_task = [](auto&& task_result) -> bool {
            if (!task_result) {
                std::println(stderr, "error: {}", task_result.error());
                return false;
            }
            return true;
        };

        if (const std::vector dirs = {root / "Bin/Debug", root / "Bin/Release", root / "Src", root / "Temp"};
            !std::ranges::all_of(dirs, [&](const auto& p) { return run_task(MakeDir(p)); })) {
            return false;
        }

        if (const auto tomlPath = root / "Rux.toml"; !initMode || !fs::exists(tomlPath)) {
            Manifest m;
            m.package = {.name = name, .version = "0.1.0", .type = (type == PackageType::Executable ? "bin" : "lib")};
            if (!m.Save(tomlPath)) {
                std::println(stderr, "error: cannot write Rux.toml");
                return false;
            }
        }

        const bool isBin = (type == PackageType::Executable);
        const std::string_view srcContent = isBin ? "func Main() -> int {\n    return 0;\n}\n" : "// Library\n";

        struct FileTask {
            fs::path path;
            std::string_view content;
        };
        const FileTask tasks[] = {{root / "Src" / (isBin ? "Main.rux" : "Lib.rux"), srcContent},
                                  {root / ".gitignore", "# Rux build outputs\nBin/\nTemp/\n"}};

        return std::ranges::all_of(tasks,
                                   [&](const auto& t) { return run_task(WriteFile(t.path, t.content, initMode)); });
    }
} // namespace Rux
