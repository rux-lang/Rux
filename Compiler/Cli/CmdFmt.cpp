// `rux fmt`.

#include "Cli/BuildReport.h"
#include "Cli/Cli.h"
#include "Cli/ManifestInput.h"
#include "Cli/Reporter.h"
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
    const auto FormatManifest = [&](const std::filesystem::path &path) {
        auto manifest = LoadManifest(path);
        if (!manifest) {
            ++failed;
            return;
        }
        const std::string formattedContent = manifest->Serialize();
        std::ifstream input(path, std::ios::binary);
        const std::string originalContent{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        ++examined;
        if (!input && !input.eof()) {
            diagnostics.Error(std::format("could not read manifest '{}'", path.string()));
            ++failed;
            return;
        }
        if (originalContent == formattedContent) {
            ++unchanged;
            output.Verbose(std::format("Unchanged: {}", path.string()));
            return;
        }
        ++changed;
        if (check) {
            diagnostics.Error(std::format("manifest '{}' is not formatted", path.string()));
            return;
        }
        if (!manifest->Save(path)) {
            diagnostics.Error(std::format("could not write manifest '{}'", path.string()));
            ++failed;
            return;
        }
        output.Verbose(std::format("Changed: {}", path.string()));
    };
    const auto FormatSources = [&](const std::filesystem::path &packageRoot) {
        const auto sourceDir = packageRoot / "Src";
        std::error_code iterationError;
        const bool sourceDirectoryExists = std::filesystem::exists(sourceDir, iterationError);
        if (iterationError) {
            diagnostics.Error(std::format("could not examine source directory '{}': {}", sourceDir.string(),
                                          iterationError.message()));
            ++failed;
            return;
        }
        if (!sourceDirectoryExists) {
            if (!opts.quiet) {
                output.Note(std::format("source directory '{}' does not exist; no source files were examined",
                                        sourceDir.string()));
            }
            return;
        }
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
    };

    if (!sourceOnly) {
        FormatManifest(*manifestPath);
    }
    // A workspace root has no sources of its own: its members' manifests and sources are formatted instead.
    const auto rootManifest = Manifest::Load(*manifestPath);
    if (rootManifest.Ok() && rootManifest.manifest->IsWorkspace()) {
        for (const auto &member : LoadWorkspaceMembers(*manifestPath, *rootManifest.manifest)) {
            if (!member.manifest) {
                if (!member.problem.empty())
                    diagnostics.Error(member.problem);
                ++failed;
                continue;
            }
            if (!sourceOnly) {
                FormatManifest(member.manifestPath);
            }
            if (!manifestOnly) {
                FormatSources(member.manifestPath.parent_path());
            }
        }
    }
    else if (!manifestOnly) {
        FormatSources(root);
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
