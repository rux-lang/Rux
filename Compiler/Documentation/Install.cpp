#include "Documentation/Install.h"

#include <cerrno>
#include <chrono>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace Rux::Documentation {
namespace {
constexpr std::string_view MarkerName = ".rux-docs";

/// Build an operational diagnostic that carries the system error, so a permission or disk-full failure says what
/// actually went wrong rather than only that generation failed.
Diagnostic FilesystemFailure(std::string message, const std::error_code error, std::optional<std::string> help = {}) {
    return ErrorDiagnostic(std::move(message), {std::format("filesystem error {}: {}", error.value(), error.message())},
                           std::move(help));
}

bool WriteFile(const std::filesystem::path &path, const std::string_view value, Diagnostic &error) {
    errno = 0;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    if (!output) {
        std::error_code writeError(errno, std::generic_category());
        if (!writeError) {
            writeError = std::make_error_code(std::errc::io_error);
        }
        error = FilesystemFailure(std::format("could not write generated documentation file '{}'", path.string()),
                                  writeError, "check that the output path is writable and has enough free space");
        return false;
    }
    return true;
}
} // namespace

GenerateResult InstallManagedDirectory(const std::filesystem::path &outputDirectory,
                                       const std::span<const OutputFile> files) {
    GenerateResult result;
    auto Fail = [&](Diagnostic diagnostic) {
        result.diagnostics.push_back(std::move(diagnostic));
        return result;
    };
    std::error_code ec;
    const auto output = std::filesystem::absolute(outputDirectory, ec).lexically_normal();
    if (ec) {
        return Fail(FilesystemFailure(
            std::format("could not resolve documentation output directory '{}'", outputDirectory.string()), ec,
            "choose a valid path with '--output <dir>'"));
    }
    const bool outputExists = std::filesystem::exists(output, ec);
    if (ec) {
        return Fail(FilesystemFailure(
            std::format("could not inspect documentation output directory '{}'", output.string()), ec));
    }
    if (outputExists) {
        const bool outputEmpty = std::filesystem::is_empty(output, ec);
        if (ec) {
            return Fail(FilesystemFailure(
                std::format("could not inspect documentation output directory '{}'", output.string()), ec));
        }
        const bool managed = outputEmpty || std::filesystem::exists(output / MarkerName, ec);
        if (ec) {
            return Fail(FilesystemFailure(
                std::format("could not inspect documentation marker '{}'", (output / MarkerName).string()), ec));
        }
        if (!managed) {
            return Fail(
                ErrorDiagnostic(std::format("refusing to replace non-empty unmarked directory '{}'", output.string()),
                                {}, "choose an empty output directory or remove its contents"));
        }
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = output.parent_path() / std::format(".rux-docs-tmp-{}", nonce);
    std::filesystem::create_directories(temporary, ec);
    if (ec) {
        return Fail(FilesystemFailure(
            std::format("could not create temporary documentation directory '{}'", temporary.string()), ec,
            "check that the output directory's parent is writable"));
    }

    Diagnostic error;
    bool written = WriteFile(temporary / MarkerName, "rux-docs-v1\n", error);
    for (const auto &file : files) {
        written = written && WriteFile(temporary / file.name, file.content, error);
    }
    if (!written) {
        std::filesystem::remove_all(temporary, ec);
        return Fail(std::move(error));
    }
    if (std::filesystem::exists(output, ec)) {
        std::filesystem::remove_all(output, ec);
    }
    if (ec) {
        return Fail(
            FilesystemFailure(std::format("could not replace managed documentation directory '{}'", output.string()),
                              ec, "close programs using the generated documentation and try again"));
    }
    std::filesystem::rename(temporary, output, ec);
    if (ec) {
        return Fail(FilesystemFailure(std::format("could not install generated documentation at '{}'", output.string()),
                                      ec, "check that the output directory's parent is writable"));
    }
    result.ok = true;
    return result;
}
} // namespace Rux::Documentation
