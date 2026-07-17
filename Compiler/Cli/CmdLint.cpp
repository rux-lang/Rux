// `rux lint`: run source-level lint checks for the current package.

#include "Cli/Cli.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Linter/Linter.h"
#include "Source/SourceLoader.h"

#include <cstdio>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;
using namespace Driver;

int Cli::RunLint(std::span<const std::string_view> args, const GlobalOptions &opts) {
    for (const auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("lint");
            return 0;
        }
        PrintUnknownOption(arg, "lint");
        return 1;
    }

    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }

    auto LintPackage = [&](const std::filesystem::path &packageManifestPath, const Manifest &packageManifest,
                           const bool showPackage) {
        if (packageManifest.IsWorkspace() || packageManifest.package.name.empty()) {
            std::print(stderr, "error: workspace member '{}' is not a package\n",
                       packageManifestPath.parent_path().string());
            return true;
        }
        if (showPackage && !opts.quiet) {
            std::println("Linting {} v{} [{}]", packageManifest.package.name, packageManifest.package.version,
                         packageManifestPath.parent_path().string());
        }
        const auto sources = SourceLoader::Load(packageManifestPath.parent_path());
        if (!sources) {
            return true;
        }

        bool packageFailed = false;
        for (const auto &error : sources->errors) {
            std::print(stderr, "{}", error);
            packageFailed = true;
        }
        for (const auto &file : sources->files) {
            if (opts.verbose) {
                std::println("  Linting {}", file.path.string());
            }
            auto result = Linting::Lint(file.source, file.path.string());
            for (const auto &diagnostic : result.diagnostics) {
                PrintDiagnostic(diagnostic);
            }
            packageFailed |= result.HasErrors();
        }

        if (!packageFailed && !opts.quiet) {
            std::println("  Lint passed: {} file(s)", sources->files.size());
        }
        return packageFailed;
    };

    bool failed = false;
    if (manifest->IsWorkspace()) {
        if (!opts.quiet) {
            std::println("Linting workspace");
        }
        const auto workspaceRoot = manifestPath->parent_path();
        const auto targetName = HostTargetTriple();
        for (const auto &member : manifest->workspace.packages) {
            const auto memberManifestPath = (workspaceRoot / member / "Rux.toml").lexically_normal();
            std::error_code ec;
            if (!std::filesystem::exists(memberManifestPath, ec)) {
                std::print(stderr, "error: workspace member '{}' has no Rux.toml\n", member);
                failed = true;
                continue;
            }
            auto memberManifest = LoadManifest(memberManifestPath);
            if (!memberManifest) {
                failed = true;
                continue;
            }
            if (IsPlatformPackageName(memberManifest->package.name) &&
                !PlatformPackageMatchesTarget(memberManifest->package.name, targetName)) {
                continue;
            }
            failed |= LintPackage(memberManifestPath, *memberManifest, true);
        }
    }
    else {
        failed = LintPackage(*manifestPath, *manifest, false);
    }
    return failed ? 1 : 0;
}
