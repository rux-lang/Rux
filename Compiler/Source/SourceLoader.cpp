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
    std::error_code filesystemError;
    const bool exists = std::filesystem::exists(srcDir, filesystemError);
    if (filesystemError) {
        result.diagnostics.push_back(
            ErrorDiagnostic(std::format("cannot inspect source directory '{}'", srcDir.string()),
                            {std::format("system error {}: {}", filesystemError.value(), filesystemError.message())},
                            "check that the package path is accessible"));
        return result;
    }
    if (!exists) {
        result.diagnostics.push_back(
            ErrorDiagnostic(std::format("source directory '{}' does not exist", srcDir.string()), {},
                            "create a 'Src' directory containing at least one '.rux' file"));
        return result;
    }
    const bool isDirectory = std::filesystem::is_directory(srcDir, filesystemError);
    if (filesystemError) {
        result.diagnostics.push_back(
            ErrorDiagnostic(std::format("cannot inspect source path '{}'", srcDir.string()),
                            {std::format("system error {}: {}", filesystemError.value(), filesystemError.message())},
                            "check that the source path is accessible"));
        return result;
    }
    if (!isDirectory) {
        result.diagnostics.push_back(
            ErrorDiagnostic(std::format("source path '{}' is not a directory", srcDir.string()), {},
                            "replace it with a 'Src' directory"));
        return result;
    }
    const auto paths = CollectSourcePaths(srcDir, result.diagnostics);
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

    std::error_code filesystemError;
    auto absolutePath = std::filesystem::absolute(path, filesystemError);
    if (filesystemError) {
        return std::nullopt;
    }
    return SourceFile{
        .path = std::move(absolutePath),
        .source = buf.str(),
    };
}

std::vector<std::filesystem::path> SourceLoader::CollectSourcePaths(const std::filesystem::path &srcDir,
                                                                    std::vector<Diagnostic> &diagnostics) {
    std::vector<std::filesystem::path> paths;
    std::error_code filesystemError;
    std::filesystem::recursive_directory_iterator entry(srcDir, filesystemError);
    const std::filesystem::recursive_directory_iterator end;
    if (filesystemError) {
        diagnostics.push_back(
            ErrorDiagnostic(std::format("cannot enumerate source directory '{}'", srcDir.string()),
                            {std::format("system error {}: {}", filesystemError.value(), filesystemError.message())},
                            "check that the source directory is readable"));
        return paths;
    }
    while (entry != end) {
        filesystemError.clear();
        const bool isRegularFile = entry->is_regular_file(filesystemError);
        if (filesystemError) {
            diagnostics.push_back(ErrorDiagnostic(
                std::format("cannot inspect source path '{}'", entry->path().string()),
                {std::format("system error {}: {}", filesystemError.value(), filesystemError.message())},
                "check that the source tree is readable"));
        }
        else if (isRegularFile && entry->path().extension() == ".rux") {
            paths.push_back(entry->path());
        }

        entry.increment(filesystemError);
        if (filesystemError) {
            diagnostics.push_back(ErrorDiagnostic(
                std::format("cannot continue enumerating source directory '{}'", srcDir.string()),
                {std::format("system error {}: {}", filesystemError.value(), filesystemError.message())},
                "check that the source tree is readable"));
            break;
        }
    }
    // Sort for deterministic ordering across platforms
    std::ranges::sort(paths);
    return paths;
}
} // namespace Rux
