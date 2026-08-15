#include "Source/SourceLoader.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

namespace Rux {
bool SourceLoadResult::HasErrors() const noexcept {
    return std::ranges::any_of(diagnostics, &Diagnostic::IsError);
}

SourceLoadResult SourceLoader::Load(const std::filesystem::path &manifestDir) {
    SourceLoadResult result;
    const auto srcDir = manifestDir / "Src";
    if (!std::filesystem::exists(srcDir)) {
        result.diagnostics.push_back(
            ErrorDiagnostic(std::format("source directory '{}' does not exist", srcDir.string()), {},
                            "create a 'Src' directory containing at least one '.rux' file"));
        return result;
    }
    if (!std::filesystem::is_directory(srcDir)) {
        result.diagnostics.push_back(
            ErrorDiagnostic(std::format("source path '{}' is not a directory", srcDir.string()), {},
                            "replace it with a 'Src' directory"));
        return result;
    }
    const auto paths = CollectSourcePaths(srcDir);
    if (paths.empty()) {
        result.diagnostics.push_back({Diagnostic::Severity::Warning,
                                      {},
                                      {.line = 0, .column = 0, .offset = 0},
                                      std::format("no *.rux files found under '{}'", srcDir.string()),
                                      {},
                                      {},
                                      {}});
    }
    for (const auto &path : paths) {
        auto file = LoadFile(path);
        if (!file) {
            result.diagnostics.push_back(ErrorDiagnostic(std::format("cannot read source file '{}'", path.string()), {},
                                                         "check that the file is readable"));
            continue;
        }
        result.files.push_back(std::move(*file));
    }
    return result;
}

std::optional<SourceFile> SourceLoader::LoadFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    std::ostringstream buf;
    buf << stream.rdbuf();
    if (!stream && !stream.eof()) {
        return std::nullopt;
    }

    return SourceFile{
        .path = std::filesystem::absolute(path),
        .source = buf.str(),
    };
}

std::vector<std::filesystem::path> SourceLoader::CollectSourcePaths(const std::filesystem::path &srcDir) {
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(srcDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".rux") {
            continue;
        }
        paths.push_back(entry.path());
    }
    // Sort for deterministic ordering across platforms
    std::ranges::sort(paths);
    return paths;
}
} // namespace Rux
