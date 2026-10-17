#include "Source/SourceLoader.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <string>

namespace Rux {
bool SourceLoadResult::HasErrors() const noexcept {
    return std::ranges::any_of(diagnostics, &Diagnostic::IsError);
}

SourceLoadResult SourceLoader::Load(const std::filesystem::path &manifestDir, Parallel::WorkerPool *pool) {
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
    // Files are read side by side and reported in path order.
    std::vector<std::optional<SourceFile>> files(paths.size());
    const auto read = [&](const std::size_t index) { files[index] = LoadFile(paths[index]); };
    if (pool != nullptr)
        pool->ForEach(paths.size(), read);
    else
        for (std::size_t index = 0; index < paths.size(); ++index)
            read(index);
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (!files[index]) {
            result.diagnostics.push_back(
                ErrorDiagnostic(std::format("cannot read source file '{}'", paths[index].string()), {},
                                "check that the file is readable"));
            continue;
        }
        result.files.push_back(std::move(*files[index]));
    }
    return result;
}

std::optional<SourceFile> SourceLoader::LoadFile(const std::filesystem::path &path) {
    // Stamped before the read: an edit that lands after it leaves a stamp older than the file, so the next build
    // looks at the file again rather than trusting what this build read. One query of the file answers both parts.
    std::error_code stampError;
    const std::filesystem::directory_entry entry(path, stampError);
    const auto size = stampError ? 0 : entry.file_size(stampError);
    const auto modified = stampError ? std::filesystem::file_time_type{} : entry.last_write_time(stampError);
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return std::nullopt;
    }
    const auto end = stream.tellg();
    if (end < 0) {
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(end), '\0');
    stream.seekg(0);
    if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return std::nullopt;
    }

    std::error_code filesystemError;
    auto absolutePath = std::filesystem::absolute(path, filesystemError);
    if (filesystemError) {
        return std::nullopt;
    }
    return SourceFile{
        .path = std::move(absolutePath),
        .source = std::move(text),
        .size = stampError ? ~std::uintmax_t{0} : size,
        .modified = stampError ? std::filesystem::file_time_type{} : modified,
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
