// Manifest dependency commands: add and remove.
//
// These handlers own command-line parsing, registry validation, persistence,
// and presentation. Manifest mutation and canonical serialization policy stay
// in Package/Manifest.

#include "Cli/Cli.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Driver/Credentials.h"
#include "Driver/Registry.h"
#include "Package/Manifest.h"

#include <cstdio>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;
using namespace Driver;

int Cli::RunAdd(std::span<const std::string_view> args, const GlobalOptions &opts) {
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

    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    const auto parsedSpec = ParsePackageSpec(spec);
    if (!parsedSpec) {
        std::print(stderr, "error: {}\n", parsedSpec.error());
        return 1;
    }
    const std::string packageName = parsedSpec->name.Text();

    if (!pathArg.empty()) {
        if (parsedSpec->ns) {
            std::print(stderr, "error: a path dependency cannot name a registry namespace\n");
            return 1;
        }
        const bool changed = manifest->AddPathDependency(parsedSpec->name, std::string(pathArg));
        if (!manifest->Save(*manifestPath)) {
            std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
            return 1;
        }
        if (!opts.quiet) {
            if (changed) {
                std::print("Added {} @ path '{}'\n", packageName, pathArg);
            }
            else {
                std::print("Up-to-date {} @ path '{}'\n", packageName, pathArg);
            }
        }
        return 0;
    }

    // A registry dependency records the namespace it resolves under, so the
    // qualified spelling is the only one that can be written.
    if (!parsedSpec->ns) {
        std::print(stderr, "error: a registry dependency needs a namespace; write 'rux add Namespace/{}'\n",
                   packageName);
        return 1;
    }

    // Confirm the package exists before writing a requirement that could never
    // resolve. The index is the cheapest route that answers that question.
    const std::string base = ResolveRegistryBase(registryArg);
    if (!opts.quiet) {
        std::print("Resolving from {}\n", base);
    }
    if (auto entry = FetchPackageIndex(base, *parsedSpec->ns, parsedSpec->name); !entry) {
        std::print(stderr, "error: {}\n",
                   Describe(entry.error(), base, QualifiedIdentity(*parsedSpec->ns, parsedSpec->name)));
        return 1;
    }

    // An omitted requirement accepts any stable release.
    VersionRange requirement = parsedSpec->version.value_or(*VersionRange::Parse("*"));
    const std::string version = requirement.Text();
    const bool changed = manifest->AddRegistryDependency(parsedSpec->name, *parsedSpec->ns, std::move(requirement));
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        if (changed) {
            std::print("Added {}/{} @ {}\n", parsedSpec->ns->Text(), packageName, version);
        }
        else {
            std::print("Up-to-date {}/{} @ {}\n", parsedSpec->ns->Text(), packageName, version);
        }
    }
    return 0;
}

int Cli::RunRemove(std::span<const std::string_view> args, const GlobalOptions &opts) {
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

    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    const std::string packageName(name);
    const auto importName = IdentitySegment::Parse(packageName);
    if (!importName) {
        std::print(stderr, "error: '{}' is not a valid import name: {}\n", packageName, Describe(importName.error()));
        return 1;
    }
    if (!manifest->RemoveDependency(*importName)) {
        std::print(stderr, "error: package '{}' is not a dependency\n", packageName);
        return 1;
    }
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        std::print("Removed {}\n", packageName);
    }
    return 0;
}
