// `rux fmt` and `rux doc`.

#include "Cli/Cli.h"
#include "Cli/DefineOption.h"
#include "Documentation/Generator.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "Formatter/Formatter.h"
#include "System/Os.h"

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
using namespace Driver;

int Cli::RunFmt(std::span<const std::string_view> args, const GlobalOptions &opts) {
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
    auto root = manifestPath->parent_path();
    auto FormatManifest = [&]() -> bool {
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            return false;
        }
        const std::string formattedContent = manifest->Serialize();
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
                return false;
            }
            if (!opts.quiet) {
                std::print("  Manifest is already formatted: {}\n", manifestPath->string());
            }
            return true;
        }
        if (originalContent != formattedContent) {
            if (!opts.quiet) {
                std::print("  Formatting {}\n", manifestPath->string());
            }
            if (!manifest->Save(*manifestPath)) {
                std::print(stderr, "error: failed to write manifest file '{}'\n", manifestPath->string());
                return false;
            }
        }
        else {
            if (!opts.quiet) {
                std::print("  Manifest is already formatted: {}\n", manifestPath->string());
            }
        }
        return true;
    };
    if (!sourceOnly && !FormatManifest()) {
        return 1;
    }
    if (manifestOnly) {
        return 0;
    }
    auto sourceDir = root / "Src";
    if (!std::filesystem::exists(sourceDir)) {
        if (!opts.quiet) {
            std::print("  No source directory found.\n");
        }
        return 0;
    }
    int fileCount = 0;
    bool formattingFailed = false;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(sourceDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".rux") {
            continue;
        }
        ++fileCount;
        std::ifstream input(entry.path(), std::ios::binary);
        std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (!input && !input.eof()) {
            std::print(stderr, "error: failed to read '{}'\n", entry.path().string());
            formattingFailed = true;
            continue;
        }
        auto result = Formatting::Format(source);
        if (!result.changed) {
            continue;
        }
        if (check) {
            if (!opts.quiet) {
                std::print(stderr, "error: '{}' is not formatted\n", entry.path().string());
            }
            formattingFailed = true;
            continue;
        }
        if (!opts.quiet) {
            std::print("  Formatting {}\n", entry.path().string());
        }
        std::ofstream output(entry.path(), std::ios::binary | std::ios::trunc);
        output << result.text;
        if (!output) {
            std::print(stderr, "error: failed to write '{}'\n", entry.path().string());
            formattingFailed = true;
        }
    }
    if (fileCount == 0 && !opts.quiet) {
        std::print("  No .rux files found.\n");
    }
    return formattingFailed ? 1 : 0;
}

int Cli::RunDoc(std::span<const std::string_view> args, const GlobalOptions &opts) {
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
                std::println(stderr, "error: {}", error);
                return 2;
            }
            continue;
        }
        PrintUnknownOption(arg, "doc");
        return 2;
    }
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto rootManifest = LoadManifest(*manifestPath);
    if (!rootManifest) {
        return 1;
    }
    std::string targetName = target.empty() ? HostTargetTriple() : CanonicalTargetTriple(target);
    if (!IsSupportedTargetTriple(targetName)) {
        std::println(stderr, "error: unsupported target '{}'; supported targets are {}", targetName,
                     SupportedTargetTriples());
        return 1;
    }

    const auto root = manifestPath->parent_path();
    std::filesystem::path output = requestedOutput;
    if (output.empty()) {
        output = rootManifest->build.output.empty() ? root / "Bin" / "Docs"
                                                    : std::filesystem::path(rootManifest->build.output) / "Docs";
        if (output.is_relative())
            output = root / output;
    }

    std::map<std::string, std::filesystem::path> localPackages;
    if (rootManifest->IsWorkspace()) {
        for (const auto &member : rootManifest->workspace.packages) {
            const auto memberPath = root / member / "Rux.toml";
            auto memberManifest = LoadManifest(memberPath);
            if (!memberManifest || memberManifest->IsWorkspace())
                return 1;
            localPackages.emplace(memberManifest->package.name.Text(), memberPath.parent_path());
        }
    }

    auto CompileAndGenerate = [&](const std::filesystem::path &packageManifestPath, Manifest packageManifest,
                                  const std::filesystem::path &packageOutput) {
        if (!opts.quiet) {
            std::println(stderr, "Generating documentation for {} v{}", packageManifest.package.name.Text(),
                         packageManifest.package.version.Text());
        }
        Manifest generatorManifest = packageManifest;
        CompileOptions compileOptions;
        compileOptions.manifestPath = packageManifestPath;
        compileOptions.manifest = std::move(packageManifest);
        compileOptions.targetName = targetName;
        compileOptions.profileName = "Debug";
        compileOptions.defines = defines;
        compileOptions.localPackageRoots = localPackages;
        compileOptions.localDependenciesOnly = rootManifest->IsWorkspace();
        compileOptions.quiet = true;
        compileOptions.verbose = opts.verbose;
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
            std::println(stderr, "error: {}", error);
            return false;
        }
        return true;
    };

    if (rootManifest->IsWorkspace()) {
        std::error_code ec;
        if (std::filesystem::exists(output, ec) && !std::filesystem::is_empty(output, ec) &&
            !std::filesystem::exists(output / ".rux-docs", ec)) {
            std::println(stderr, "error: refusing to replace non-empty unmarked directory '{}'", output.string());
            return 1;
        }
        const auto temporary = output.parent_path() / ".rux-docs-workspace-tmp";
        std::filesystem::remove_all(temporary, ec);
        std::filesystem::create_directories(temporary, ec);
        if (ec) {
            std::println(stderr, "error: failed to create temporary documentation directory: {}", ec.message());
            return 1;
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
                return 1;
            const std::string name = memberManifest->package.name.Text();
            if (!CompileAndGenerate(memberPath, std::move(*memberManifest), temporary / name))
                return 1;
            landing << "<li><a href=\"" << name << "/index.html\">" << name << "</a></li>";
        }
        landing << "</ul>";
        landing.close();
        std::ofstream marker(temporary / ".rux-docs", std::ios::binary | std::ios::trunc);
        marker << "rux-docs-v1\n";
        marker.close();
        if (!landing || !marker) {
            std::println(stderr, "error: failed to write workspace documentation landing page");
            return 1;
        }
        if (std::filesystem::exists(output, ec))
            std::filesystem::remove_all(output, ec);
        std::filesystem::rename(temporary, output, ec);
        if (ec) {
            std::println(stderr, "error: failed to install workspace documentation: {}", ec.message());
            return 1;
        }
    }
    else if (!CompileAndGenerate(*manifestPath, std::move(*rootManifest), output)) {
        return 1;
    }

    const auto index = output / "index.html";
    std::println("{}", index.string());
    if (openAfter && !System::OpenInDefaultApplication(index)) {
        std::println(stderr, "error: documentation was generated, but no browser opener is available");
        return 1;
    }
    return 0;
}
