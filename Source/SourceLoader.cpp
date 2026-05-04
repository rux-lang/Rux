/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/SourceLoader.h"

#include <fstream>
#include <print>
#include <sstream>

namespace Rux
{
    std::optional<SourceLoadResult>
    SourceLoader::Load(const std::filesystem::path& manifestDir)
    {
        const auto srcDir = manifestDir / "Src";
        if (!std::filesystem::exists(srcDir))
        {
            std::print(stderr,
                       "error: source directory '{}' does not exist\n",
                       srcDir.string());
            return std::nullopt;
        }
        if (!std::filesystem::is_directory(srcDir))
        {
            std::print(stderr,
                       "error: '{}' is not a directory\n",
                       srcDir.string());
            return std::nullopt;
        }
        const auto paths = CollectSourcePaths(srcDir);
        if (paths.empty())
        {
            std::print(stderr,
                       "warning: no *.rux files found under '{}'\n",
                       srcDir.string());
        }
        SourceLoadResult result;
        for (const auto& path : paths)
        {
            auto file = LoadFile(path);
            if (!file)
            {
                result.errors.push_back(
                    std::format("error: cannot read source file '{}'\n", path.string()));
                continue;
            }
            result.files.push_back(std::move(*file));
        }
        return result;
    }


    std::optional<SourceFile>
    SourceLoader::LoadFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        if (!stream) return std::nullopt;

        std::ostringstream buf;
        buf << stream.rdbuf();
        if (!stream && !stream.eof()) return std::nullopt;

        return SourceFile{
            .path = std::filesystem::absolute(path),
            .source = buf.str(),
        };
    }


    std::vector<std::filesystem::path>
    SourceLoader::CollectSourcePaths(const std::filesystem::path& srcDir)
    {
        std::vector<std::filesystem::path> paths;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(srcDir))
        {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".rux") continue;
            paths.push_back(entry.path());
        }
        // Sort for deterministic ordering across platforms
        std::ranges::sort(paths);
        return paths;
    }
}
