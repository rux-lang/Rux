// `rux fmt` and `rux doc`.

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

int Cli::RunDoc(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter result(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    bool openAfter = false;
    bool includePrivate = false;
    std::filesystem::path requestedOutput;
    std::string_view target;
    std::map<std::string, std::string> defines;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--open") {
            openAfter = true;
            continue;
        }
        if (arg == "--document-private-items") {
            includePrivate = true;
            continue;
        }
        if ((arg == "-o" || arg == "--output") && i + 1 < args.size()) {
            requestedOutput = args[++i];
            continue;
        }
        if (arg == "--target" && i + 1 < args.size()) {
            target = args[++i];
            continue;
        }
        if (arg == "--define" && i + 1 < args.size()) {
            std::string error;
            if (!CliSupport::AddCompileTimeDefine(args[++i], defines, error)) {
                diagnostics.Error(error);
                return 2;
            }
            continue;
        }
        PrintUnknownOption(arg, "doc");
        return 2;
    }
    const auto targetTriple =
        target.empty() ? std::optional{Target::TargetTriple::Host()} : Target::TargetTriple::Parse(target);
    if (!targetTriple) {
        diagnostics.Error(std::format("target '{}' is not supported", target));
        diagnostics.Note(std::format("supported targets are {}", SupportedTargetTriples()));
        diagnostics.Help("choose a supported target with '--target <triple>'");
        return 1;
    }
    const std::string targetName(targetTriple->CanonicalName());
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto rootManifest = LoadManifest(*manifestPath);
    if (!rootManifest) {
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto DocumentationFailed = [&]() {
        diagnostics.Failure(
            "Failed", std::format("documentation generation in {}", Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    };
    const auto root = manifestPath->parent_path();
    std::filesystem::path output = requestedOutput;
    if (output.empty()) {
        output = ResolveRawOutputRoot(root, *rootManifest) / "Docs";
    }
    const std::size_t packageCount = rootManifest->IsWorkspace() ? rootManifest->workspace.packages.size() : 1;
    const std::string subject = rootManifest->IsWorkspace()
                                  ? std::format("workspace ({})", targetName)
                                  : std::format("{} v{} ({})", rootManifest->package.name.Text(),
                                                rootManifest->package.version.Text(), targetName);
    diagnostics.Progress("Generating", std::format("documentation for {}", subject));
    diagnostics.Verbose(std::format("Output directory: {}", output.string()));
    if (includePrivate) {
        diagnostics.Verbose("Including private items");
    }

    std::map<std::string, std::filesystem::path> localPackages;
    if (rootManifest->IsWorkspace()) {
        for (const auto &member : rootManifest->workspace.packages) {
            const auto memberPath = root / member / "Rux.toml";
            auto memberManifest = LoadManifest(memberPath);
            if (!memberManifest || memberManifest->IsWorkspace())
                return DocumentationFailed();
            localPackages.emplace(memberManifest->package.name.Normalized(), memberPath.parent_path());
        }
    }

    auto CompileAndGenerate = [&](const std::filesystem::path &packageManifestPath, Manifest packageManifest,
                                  const std::filesystem::path &packageOutput) {
        diagnostics.Verbose(std::format("Package: {} v{}", packageManifest.package.name.Text(),
                                        packageManifest.package.version.Text()));
        Manifest generatorManifest = packageManifest;
        CompileOptions compileOptions;
        compileOptions.manifestPath = packageManifestPath;
        compileOptions.manifest = std::move(packageManifest);
        compileOptions.target = *targetTriple;
        compileOptions.defines = defines;
        compileOptions.localPackageRoots = localPackages;
        compileOptions.localDependenciesOnly = rootManifest->IsWorkspace();
        CliSupport::ConfigureCompileDiagnostics(compileOptions, diagnostics);
        if (opts.verbose) {
            compileOptions.emitProgress = [&](const CompileProgress &event) {
                CliSupport::ReportCompileProgress(diagnostics, event);
            };
        }
        compileOptions.checkOnly = true;
        compileOptions.captureFrontend = true;
        CompilerDriver driver(std::move(compileOptions));
        auto result = driver.Compile();
        if (!result.ok)
            return false;

        std::string error;
        const Documentation::GenerateOptions generateOptions{.packageRoot = packageManifestPath.parent_path(),
                                                             .outputDirectory = packageOutput,
                                                             .includePrivate = includePrivate};
        if (!Documentation::Generate(generatorManifest, result.modules, generateOptions, error)) {
            diagnostics.Error(error);
            return false;
        }
        return true;
    };

    if (rootManifest->IsWorkspace()) {
        std::error_code ec;
        if (std::filesystem::exists(output, ec) && !std::filesystem::is_empty(output, ec) &&
            !std::filesystem::exists(output / ".rux-docs", ec)) {
            diagnostics.Error(std::format("refusing to replace non-empty unmarked directory '{}'", output.string()));
            diagnostics.Help("choose an empty output directory or remove its contents");
            return DocumentationFailed();
        }
        const auto temporary = output.parent_path() / ".rux-docs-workspace-tmp";
        std::filesystem::remove_all(temporary, ec);
        std::filesystem::create_directories(temporary, ec);
        if (ec) {
            diagnostics.Error(std::format("could not create temporary documentation directory '{}': {}",
                                          temporary.string(), ec.message()));
            return DocumentationFailed();
        }
        std::ofstream landing(temporary / "index.html", std::ios::binary | std::ios::trunc);
        landing << "<!doctype html><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
                   "<title>Workspace API "
                   "documentation</title><style>body{max-width:52rem;margin:auto;padding:2rem;font:16px/1.6 "
                   "system-ui;color-scheme:light dark}</style>"
                   "<h1>Workspace API documentation</h1><ul>";
        for (const auto &member : rootManifest->workspace.packages) {
            const auto memberPath = root / member / "Rux.toml";
            auto memberManifest = LoadManifest(memberPath);
            if (!memberManifest)
                return DocumentationFailed();
            const std::string name = memberManifest->package.name.Text();
            if (!CompileAndGenerate(memberPath, std::move(*memberManifest), temporary / name))
                return DocumentationFailed();
            landing << "<li><a href=\"" << name << "/index.html\">" << name << "</a></li>";
        }
        landing << "</ul>";
        landing.close();
        std::ofstream marker(temporary / ".rux-docs", std::ios::binary | std::ios::trunc);
        marker << "rux-docs-v1\n";
        marker.close();
        if (!landing || !marker) {
            diagnostics.Error(std::format("could not write workspace documentation under '{}'", temporary.string()));
            return DocumentationFailed();
        }
        if (std::filesystem::exists(output, ec))
            std::filesystem::remove_all(output, ec);
        std::filesystem::rename(temporary, output, ec);
        if (ec) {
            diagnostics.Error(
                std::format("could not install workspace documentation at '{}': {}", output.string(), ec.message()));
            return DocumentationFailed();
        }
    }
    else if (!CompileAndGenerate(*manifestPath, std::move(*rootManifest), output)) {
        return DocumentationFailed();
    }

    const auto index = output / "index.html";
    if (openAfter && !System::OpenInDefaultApplication(index)) {
        diagnostics.Warning("documentation was generated but Rux could not open a browser");
        diagnostics.Detail(std::format("Output: {}", index.string()), CliSupport::MessageVisibility::Always);
        diagnostics.Help(std::format("open '{}' in a browser", index.string()));
        return 1;
    }
    result.Success("Generated",
                   std::format("documentation for {} in {}", Reporting::FormatCount(packageCount, "package"),
                               Reporting::FormatDuration(ElapsedMs(started))));
    result.Detail(std::format("Output: {}", index.string()));
    return 0;
}
