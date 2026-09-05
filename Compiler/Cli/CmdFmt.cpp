// `rux fmt`.

#include "Cli/Cli.h"
#include "Cli/CompilerProgress.h"
#include "Cli/DefineOption.h"
#include "Cli/ManifestInput.h"
#include "Cli/Reporter.h"
#include "Documentation/Generator.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "Formatter/Formatter.h"
#include "Reporting/Reporting.h"
#include "System/Os.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace CliSupport;
using namespace Driver;

int Cli::RunFmt(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    bool check = false;
    bool manifestOnly = false;
    bool sourceOnly = false;
    for (auto &arg : args) {
        if (arg == "--check") {
            check = true;
            continue;
        }
        if (arg == "--manifest-only") {
            manifestOnly = true;
            continue;
        }
        if (arg == "--source-only") {
            sourceOnly = true;
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
    const auto started = std::chrono::steady_clock::now();
    const auto root = manifestPath->parent_path();
    output.Progress(check ? "Checking" : "Formatting", std::format("files under '{}'", root.string()));

    std::size_t examined = 0;
    std::size_t changed = 0;
    std::size_t unchanged = 0;
    std::size_t failed = 0;
    auto FormatManifest = [&]() -> bool {
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            ++failed;
            return false;
        }
        const std::string formattedContent = manifest->Serialize();
        std::ifstream input(*manifestPath, std::ios::binary);
        const std::string originalContent{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        ++examined;
        if (!input && !input.eof()) {
            diagnostics.Error(std::format("could not read manifest '{}'", manifestPath->string()));
            ++failed;
            return false;
        }
        if (originalContent == formattedContent) {
            ++unchanged;
            output.Verbose(std::format("Unchanged: {}", manifestPath->string()));
            return true;
        }
        ++changed;
        if (check) {
            diagnostics.Error(std::format("manifest '{}' is not formatted", manifestPath->string()));
            return true;
        }
        if (!manifest->Save(*manifestPath)) {
            diagnostics.Error(std::format("could not write manifest '{}'", manifestPath->string()));
            ++failed;
            return false;
        }
        output.Verbose(std::format("Changed: {}", manifestPath->string()));
        return true;
    };
    if (!sourceOnly) {
        static_cast<void>(FormatManifest());
    }
    if (!manifestOnly) {
        const auto sourceDir = root / "Src";
        std::error_code iterationError;
        const bool sourceDirectoryExists = std::filesystem::exists(sourceDir, iterationError);
        if (iterationError) {
            diagnostics.Error(std::format("could not examine source directory '{}': {}", sourceDir.string(),
                                          iterationError.message()));
            ++failed;
        }
        else if (!sourceDirectoryExists) {
            if (!opts.quiet) {
                output.Note(std::format("source directory '{}' does not exist; no source files were examined",
                                        sourceDir.string()));
            }
        }
        else {
            std::size_t sourceFiles = 0;
            std::filesystem::recursive_directory_iterator iterator(sourceDir, iterationError);
            const std::filesystem::recursive_directory_iterator end;
            while (!iterationError && iterator != end) {
                const auto entry = *iterator;
                iterator.increment(iterationError);
                std::error_code typeError;
                if (!entry.is_regular_file(typeError) || entry.path().extension() != ".rux") {
                    continue;
                }
                ++sourceFiles;
                ++examined;
                std::ifstream input(entry.path(), std::ios::binary);
                const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                if (!input && !input.eof()) {
                    diagnostics.Error(std::format("could not read source file '{}'", entry.path().string()));
                    ++failed;
                    continue;
                }
                auto result = Formatting::Format(source);
                if (!result.changed) {
                    ++unchanged;
                    output.Verbose(std::format("Unchanged: {}", entry.path().string()));
                    continue;
                }
                ++changed;
                if (check) {
                    diagnostics.Error(std::format("source file '{}' is not formatted", entry.path().string()));
                    continue;
                }
                std::ofstream formatted(entry.path(), std::ios::binary | std::ios::trunc);
                formatted << result.text;
                if (!formatted) {
                    diagnostics.Error(std::format("could not write source file '{}'", entry.path().string()));
                    ++failed;
                    continue;
                }
                output.Verbose(std::format("Changed: {}", entry.path().string()));
            }
            if (iterationError) {
                diagnostics.Error(std::format("could not examine source directory '{}': {}", sourceDir.string(),
                                              iterationError.message()));
                ++failed;
            }
            if (sourceFiles == 0 && !iterationError) {
                if (!opts.quiet) {
                    output.Note(std::format("no '.rux' files were found under '{}'", sourceDir.string()));
                }
            }
        }
    }

    const auto duration = Reporting::FormatDuration(ElapsedMs(started));
    if (failed > 0) {
        output.Failure("Failed", std::format("to format {} in {} ({} errors)", Reporting::FormatCount(examined, "file"),
                                             duration, failed));
        return 1;
    }
    if (check && changed > 0) {
        output.Failure("Failed", std::format("formatting check for {} in {} ({} formatted, {} need formatting)",
                                             Reporting::FormatCount(examined, "file"), duration, unchanged, changed));
        return 1;
    }
    if (check) {
        output.Success("Checked", std::format("{} in {} ({} formatted, 0 need formatting)",
                                              Reporting::FormatCount(examined, "file"), duration, unchanged));
    }
    else {
        output.Success("Formatted",
                       std::format("{} in {} ({} changed, {} unchanged)", Reporting::FormatCount(examined, "file"),
                                   duration, changed, unchanged));
    }
    return 0;
}
