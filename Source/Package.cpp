/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Package.h"
#include "Rux/Manifest.h"

#include <fstream>
#include <print>
#include <system_error>

namespace Rux {
    // Write a file only if it does not already exist (init mode) or always (new mode).
    static bool WriteFile(const std::filesystem::path &path,
                          const std::string_view content,
                          const bool skipIfExists) {
        if (skipIfExists && std::filesystem::exists(path)) return true;
        std::ofstream f(path);
        if (!f) return false;
        f << content;
        return f.good();
    }

    static bool MakeDir(const std::filesystem::path &path) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return !ec;
    }

    bool ScaffoldPackage(const std::filesystem::path &root,
                         const std::string &name,
                         const PackageType type,
                         const bool initMode) {
        // In new mode create the root directory; in init mode it already exists.
        if (!initMode) {
            if (std::filesystem::exists(root)) {
                std::print(stderr, "error: directory '{}' already exists\n", root.string());
                return false;
            }
            if (!MakeDir(root)) {
                std::print(stderr, "error: cannot create directory '{}'\n", root.string());
                return false;
            }
        }

        // Subdirectories
        for (const auto &subdir: {
                 root / "Bin" / "Debug",
                 root / "Bin" / "Release",
                 root / "Src",
                 root / "Temp"
             }) {
            if (!MakeDir(subdir)) {
                std::print(stderr, "error: cannot create directory '{}'\n", subdir.string());
                return false;
            }
        }

        // Rux.toml
        {
            auto tomlPath = root / "Rux.toml";
            if (!initMode || !std::filesystem::exists(tomlPath)) {
                Manifest m;
                m.package.name = name;
                m.package.version = "0.1.0";
                m.package.type = (type == PackageType::Bin) ? "bin" : "lib";
                if (!m.Save(tomlPath)) {
                    std::print(stderr, "error: cannot write Rux.toml\n");
                    return false;
                }
            }
        }

        // Src/Main.rux  (bin)  or  Src/Lib.rux  (lib)
        {
            const bool isBin = (type == PackageType::Bin);
            std::string srcName = isBin ? "Main.rux" : "Lib.rux";
            const std::string srcContent = isBin
                                               ? "func Main() {\n    Print(\"Hello, World!\")\n}\n"
                                               : "// " + name + " library\n";

            if (!WriteFile(root / "Src" / srcName, srcContent, initMode)) {
                std::print(stderr,
                           "error: cannot write Src/{}\n", srcName);
                return false;
            }
        }

        // .gitignore
        {
            constexpr std::string_view gitignore =
                    "# Rux build outputs\n"
                    "Bin/\n"
                    "Temp/\n";
            if (!WriteFile(root / ".gitignore", gitignore, initMode)) {
                std::print(stderr, "error: cannot write .gitignore\n");
                return false;
            }
        }

        return true;
    }
}
