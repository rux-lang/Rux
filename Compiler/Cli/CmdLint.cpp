// `rux lint`: run source-level lint checks for the current package.

#include "Cli/Cli.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Linter/Linter.h"
#include "Source/SourceLoader.h"

#include <cstdio>
#include <print>
#include <span>
#include <string_view>

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
    const auto sources = SourceLoader::Load(manifestPath->parent_path());
    if (!sources) {
        return 1;
    }

    bool failed = false;
    for (const auto &error : sources->errors) {
        std::print(stderr, "{}", error);
        failed = true;
    }
    for (const auto &file : sources->files) {
        if (opts.verbose) {
            std::println("  Linting {}", file.path.string());
        }
        auto result = Linting::Lint(file.source, file.path.string());
        for (const auto &diagnostic : result.diagnostics) {
            PrintDiagnostic(diagnostic);
        }
        failed |= result.HasErrors();
    }

    if (!failed && !opts.quiet) {
        std::println("  Lint passed: {} file(s)", sources->files.size());
    }
    return failed ? 1 : 0;
}
