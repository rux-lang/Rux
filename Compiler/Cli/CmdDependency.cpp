// Manifest dependency commands: add and remove.
//
// These handlers own command-line parsing, registry validation, persistence,
// and presentation. Manifest mutation and canonical serialization policy stay
// in Package/Manifest.

#include "Cli/BuildReport.h"
#include "Cli/Cli.h"
#include "Cli/ManifestInput.h"
#include "Cli/Reporter.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Package/Credentials.h"
#include "Package/Manifest.h"
#include "Package/Registry.h"
#include "Reporting/Reporting.h"

#include <chrono>
#include <cstdio>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux::Packages;

using namespace Rux;
using namespace CliSupport;
using namespace Driver;

int Cli::RunAdd(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    std::string_view spec;
    std::string_view pathArg;
    std::string_view registryArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("add");
            return 0;
        }
        if (arg == "--path") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--path' requires an argument\n");
                return 1;
            }
            pathArg = args[++i];
            continue;
        }
        if (arg == "--registry") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--registry' requires an argument\n");
                return 1;
            }
            registryArg = args[++i];
            continue;
        }
        if (!arg.starts_with('-') && spec.empty()) {
            spec = arg;
            continue;
        }
        PrintUnknownOption(arg, "add");
        return 1;
    }
    if (spec.empty()) {
        std::print(stderr, "error: missing package name\n\n");
        PrintHelpFor("add");
        return 1;
    }

    const auto parsedSpec = ParsePackageSpec(spec);
    if (!parsedSpec) {
        diagnostics.Error(parsedSpec.error().message);
        for (const auto &note : parsedSpec.error().notes) {
            diagnostics.Note(note);
        }
        diagnostics.Help(parsedSpec.error().help.value_or(
            pathArg.empty() ? "use '[namespace]/[package]@[requirement]' for a registry dependency"
                            : "use '[package] --path [path]' for a path dependency"));
        return 1;
    }
    const std::string packageName = parsedSpec->name.Text();

    if (!pathArg.empty() && !registryArg.empty()) {
        diagnostics.Error("options '--path' and '--registry' cannot be used together");
        diagnostics.Help("use '--path' for a local dependency or '--registry' for a registry dependency");
        return 1;
    }
    if (!pathArg.empty()) {
        if (parsedSpec->ns) {
            diagnostics.Error(std::format("path dependency '{}' cannot include a registry namespace", spec));
            diagnostics.Help(std::format("use 'rux add {} --path {}'", packageName, pathArg));
            return 1;
        }
        if (parsedSpec->version) {
            diagnostics.Error(std::format("path dependency '{}' cannot include a version requirement", spec));
            diagnostics.Help(std::format("use 'rux add {} --path {}'", packageName, pathArg));
            return 1;
        }
    }
    else if (!parsedSpec->ns) {
        diagnostics.Error(std::format("registry dependency '{}' must include a namespace", spec));
        diagnostics.Help(std::format("use 'rux add Namespace/{}'", packageName));
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        output.Failure(
            "Failed", std::format("to add dependency '{}' in {}", spec, Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        output.Failure(
            "Failed", std::format("to add dependency '{}' in {}", spec, Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    }

    if (!pathArg.empty()) {
        const bool changed = manifest->AddPathDependency(parsedSpec->name, std::string(pathArg));
        if (changed && !manifest->Save(*manifestPath)) {
            diagnostics.Error(std::format("could not write manifest '{}'", manifestPath->string()));
            diagnostics.Help("check the manifest's file permissions, then retry");
            output.Failure("Failed", std::format("to add path dependency '{}' in {}", packageName,
                                                 Reporting::FormatDuration(ElapsedMs(started))));
            return 1;
        }
        output.Success(changed ? "Added" : "Unchanged", std::format("path dependency '{}' in {}", packageName,
                                                                    Reporting::FormatDuration(ElapsedMs(started))));
        output.Detail(std::format("Path: {}", pathArg));
        output.Detail(std::format("Manifest: {}", manifestPath->string()));
        return 0;
    }

    // Confirm the package exists before writing a requirement that could never
    // resolve. The index is the cheapest route that answers that question.
    const std::string base = ResolveRegistryBase(registryArg);
    output.Progress("Resolving", std::format("registry dependency '{}' from {}", spec, base));
    if (auto entry = FetchPackageIndex(base, *parsedSpec->ns, parsedSpec->name); !entry) {
        const auto problem = Describe(entry.error(), base, QualifiedIdentity(*parsedSpec->ns, parsedSpec->name));
        diagnostics.Error(problem.message);
        for (const auto &note : problem.notes) {
            diagnostics.Note(note);
        }
        diagnostics.Help(problem.help.value_or(
            std::format("verify '{}' exists in the registry or choose another '--registry' URL", spec)));
        output.Failure("Failed", std::format("to add registry dependency '{}' in {}", spec,
                                             Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    }

    // An omitted requirement accepts any stable release.
    VersionRange requirement = parsedSpec->version.value_or(*VersionRange::Parse("*"));
    const std::string version = requirement.Text();
    const bool changed = manifest->AddRegistryDependency(parsedSpec->name, *parsedSpec->ns, std::move(requirement));
    if (changed && !manifest->Save(*manifestPath)) {
        diagnostics.Error(std::format("could not write manifest '{}'", manifestPath->string()));
        diagnostics.Help("check the manifest's file permissions, then retry");
        output.Failure("Failed", std::format("to add registry dependency '{}' in {}", spec,
                                             Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    }
    const auto identity = QualifiedIdentity(*parsedSpec->ns, parsedSpec->name);
    output.Success(changed ? "Added" : "Unchanged", std::format("registry dependency '{}@{}' in {}", identity, version,
                                                                Reporting::FormatDuration(ElapsedMs(started))));
    output.Detail(std::format("Registry: {}", base));
    output.Detail(std::format("Manifest: {}", manifestPath->string()));
    return 0;
}

int Cli::RunRemove(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    std::string_view name;
    for (const std::string_view arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("remove");
            return 0;
        }
        if (!arg.starts_with('-') && name.empty()) {
            name = arg;
            continue;
        }
        PrintUnknownOption(arg, "remove");
        return 1;
    }
    if (name.empty()) {
        std::print(stderr, "error: missing package name\n\n");
        PrintHelpFor("remove");
        return 1;
    }

    const std::string packageName(name);
    const auto importName = IdentitySegment::Parse(packageName);
    if (!importName) {
        diagnostics.Error(std::format("dependency name '{}' is invalid", packageName));
        diagnostics.Note(Describe(importName.error()));
        diagnostics.Help("pass the dependency's unqualified import name, as shown by 'rux list'");
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        output.Failure("Failed", std::format("to remove dependency '{}' in {}", packageName,
                                             Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        output.Failure("Failed", std::format("to remove dependency '{}' in {}", packageName,
                                             Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    }
    if (!manifest->RemoveDependency(*importName)) {
        diagnostics.Error(std::format("dependency '{}' was not found in '{}'", packageName, manifestPath->string()));
        diagnostics.Help("run 'rux list' to see the dependency import names that can be removed");
        output.Failure("Failed", std::format("to remove dependency '{}' in {}", packageName,
                                             Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    }
    if (!manifest->Save(*manifestPath)) {
        diagnostics.Error(std::format("could not write manifest '{}'", manifestPath->string()));
        diagnostics.Help("check the manifest's file permissions, then retry");
        output.Failure("Failed", std::format("to remove dependency '{}' in {}", packageName,
                                             Reporting::FormatDuration(ElapsedMs(started))));
        return 1;
    }
    output.Success("Removed",
                   std::format("dependency '{}' in {}", packageName, Reporting::FormatDuration(ElapsedMs(started))));
    output.Detail(std::format("Manifest: {}", manifestPath->string()));
    return 0;
}
