// `rux fmt` and `rux doc`.

#include "Cli/Cli.h"
#include "Driver/BuildTarget.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Misc;

int Cli::RunFmt(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool check = false;
    bool manifestOnly = false;
    for (auto &arg : args) {
        if (arg == "--check") {
            check = true;
            continue;
        }
        if (arg == "--manifest-only") {
            manifestOnly = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("fmt");
            return 0;
        }
        PrintUnknownOption(arg, "fmt");
        return 1;
    }
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto root = manifestPath->parent_path();
    if (manifestOnly) {
        auto manifest = Manifest::Load(*manifestPath);
        if (!manifest) {
            std::print(stderr, "error: failed to parse manifest file '{}'\n", manifestPath->string());
            return 1;
        }
        // Get formatted content by saving to a temp file and reading it
        auto tempPath = manifestPath->parent_path() / "Rux.toml.fmt.tmp";
        if (!manifest->Save(tempPath)) {
            std::print(stderr, "error: failed to serialize formatted manifest\n");
            return 1;
        }
        std::string formattedContent;
        {
            std::ifstream tempFile(tempPath, std::ios::binary);
            if (tempFile) {
                formattedContent.assign(std::istreambuf_iterator<char>(tempFile), std::istreambuf_iterator<char>());
            }
        }
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
        std::string originalContent;
        {
            std::ifstream inFile(*manifestPath, std::ios::binary);
            if (inFile) {
                originalContent.assign(std::istreambuf_iterator<char>(inFile), std::istreambuf_iterator<char>());
            }
        }
        if (check) {
            if (originalContent != formattedContent) {
                if (!opts.quiet) {
                    std::print(stderr, "error: manifest '{}' is not formatted\n", manifestPath->string());
                }
                return 1;
            }
            if (!opts.quiet) {
                std::print("  Manifest is already formatted: {}\n", manifestPath->string());
            }
            return 0;
        }
        if (originalContent != formattedContent) {
            if (!opts.quiet) {
                std::print("  Formatting {}\n", manifestPath->string());
            }
            if (!manifest->Save(*manifestPath)) {
                std::print(stderr, "error: failed to write manifest file '{}'\n", manifestPath->string());
                return 1;
            }
        }
        else {
            if (!opts.quiet) {
                std::print("  Manifest is already formatted: {}\n", manifestPath->string());
            }
        }
        return 0;
    }
    auto sourceDir = root / "Source";
    if (!std::filesystem::exists(sourceDir)) {
        if (!opts.quiet) {
            std::print("  No source directory found.\n");
        }
        return 0;
    }
    int fileCount = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(sourceDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".rux") {
            continue;
        }
        ++fileCount;
        if (!opts.quiet) {
            if (check) {
                std::print("  Checking   {}\n", entry.path().string());
            }
            else {
                std::print("  Formatting {}\n", entry.path().string());
            }
        }
        // TODO: source formatter
    }
    if (fileCount == 0 && !opts.quiet) {
        std::print("  No .rux files found.\n");
    }
    return 0;
}

int Cli::RunDoc(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool openAfter = false;
    for (auto &arg : args) {
        if (arg == "--open") {
            openAfter = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("doc");
            return 0;
        }
        PrintUnknownOption(arg, "doc");
        return 1;
    }
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("  Generating documentation for {} v{}\n", manifest->package.name, manifest->package.version);
    }

    // TODO: documentation generator

    if (openAfter && !opts.quiet) {
        std::print("     Opening documentation...\n");
    }

    return 0;
}
