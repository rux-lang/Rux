#include "Rux/SourceLoader.h"

#include <algorithm>
#include <limits>
#include <fstream>
#include <iterator>
#include <print>
#include <system_error>

namespace Rux {
    std::optional<SourceLoadResult> SourceLoader::Load(const std::filesystem::path& manifestDir) {
        const auto srcDir = manifestDir / "Src";
        if (!std::filesystem::exists(srcDir)) {
            std::print(stderr, "error: source directory '{}' does not exist\n", srcDir.string());
            return std::nullopt;
        }
        if (!std::filesystem::is_directory(srcDir)) {
            std::print(stderr, "error: '{}' is not a directory\n", srcDir.string());
            return std::nullopt;
        }
        const auto paths = CollectSourcePaths(srcDir);
        if (paths.empty()) {
            std::print(stderr, "warning: no *.rux files found under '{}'\n", srcDir.string());
        }
        SourceLoadResult result;
        result.files.reserve(paths.size());
        for (const auto& path : paths) {
            auto file = LoadFile(path);
            if (!file) {
                result.errors.push_back(std::format("error: cannot read source file '{}'\n", path.string()));
                continue;
            }
            result.files.push_back(std::move(*file));
        }
        return result;
    }

    std::optional<SourceFile> SourceLoader::LoadFile(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return std::nullopt;

        std::string source;
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        constexpr auto maxSize = (std::min)(static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)()),
                                            static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)()));
        if (!ec && size <= maxSize) {
            source.resize(static_cast<std::size_t>(size));
            if (!source.empty()) stream.read(source.data(), static_cast<std::streamsize>(source.size()));
        }
        else {
            source.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }

        if (!stream && !stream.eof()) return std::nullopt;

        return SourceFile{
            .path = std::filesystem::absolute(path),
            .source = std::move(source),
        };
    }

    std::vector<std::filesystem::path> SourceLoader::CollectSourcePaths(const std::filesystem::path& srcDir) {
        std::vector<std::filesystem::path> paths;
        std::error_code ec;
        const std::filesystem::recursive_directory_iterator end;
        std::filesystem::recursive_directory_iterator it(
            srcDir, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != end; it.increment(ec)) {
            const auto& entry = *it;
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc) continue;
            if (entry.path().extension() != ".rux") continue;
            paths.push_back(entry.path());
        }
        // Sort for deterministic ordering across platforms
        std::ranges::sort(paths);
        return paths;
    }
} // namespace Rux
