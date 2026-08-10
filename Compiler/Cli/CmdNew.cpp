// `rux new` and `rux init` — package scaffolding.

#include "Cli/Cli.h"
#include "Package/Package.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;

namespace {

// The four Version 1 package kinds map one-to-one onto the scaffolding flags,
// which are mutually exclusive rather than letting one silently win.
std::optional<ManifestPackageType> SelectPackageType(const bool executable, const bool shared, const bool staticLibrary,
                                                     const bool source) {
    if (static_cast<int>(executable) + static_cast<int>(shared) + static_cast<int>(staticLibrary) +
            static_cast<int>(source) >
        1) {
        std::print(stderr, "error: '--executable', '--shared', '--static' and '--source' cannot be combined\n\n");
        return std::nullopt;
    }
    if (shared) {
        return ManifestPackageType::SharedLibrary;
    }
    if (staticLibrary) {
        return ManifestPackageType::StaticLibrary;
    }
    if (source) {
        return ManifestPackageType::SourceLibrary;
    }
    return ManifestPackageType::Executable;
}

// Validates the optional `--namespace` operand against the identity grammar.
bool ParseNamespace(const std::string_view value, std::optional<IdentitySegment> &ns) {
    const auto parsed = IdentitySegment::Parse(value);
    if (!parsed) {
        std::print(stderr, "error: '{}' is not a valid namespace: {}\n", value, Describe(parsed.error()));
        return false;
    }
    ns = *parsed;
    return true;
}

// Lower-case description used by the progress lines of both commands.
std::string_view KindLabel(const ManifestPackageType type) {
    switch (type) {
    case ManifestPackageType::SharedLibrary:
        return "shared-library";
    case ManifestPackageType::StaticLibrary:
        return "static-library";
    case ManifestPackageType::SourceLibrary:
        return "source-library";
    case ManifestPackageType::Executable:
        break;
    }
    return "executable";
}

} // namespace

int Cli::RunInit(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool executable = false;
    bool shared = false;
    bool staticLibrary = false;
    bool source = false;
    std::optional<IdentitySegment> ns;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--executable") {
            executable = true;
            continue;
        }
        if (arg == "--shared") {
            shared = true;
            continue;
        }
        if (arg == "--static") {
            staticLibrary = true;
            continue;
        }
        if (arg == "--source") {
            source = true;
            continue;
        }
        if (arg == "--namespace") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--namespace' requires an argument\n");
                return 1;
            }
            if (!ParseNamespace(args[++i], ns)) {
                return 1;
            }
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("init");
            return 0;
        }
        PrintUnknownOption(arg, "init");
        return 1;
    }
    const auto type = SelectPackageType(executable, shared, staticLibrary, source);
    if (!type) {
        PrintHelpFor("init");
        return 1;
    }
    const auto root = std::filesystem::current_path();
    auto name = root.filename().string();
    if (!opts.quiet) {
        std::print("Initializing {} package '{}'\n", KindLabel(*type), name);
    }
    if (!ScaffoldPackage({.root = root, .name = name, .type = *type, .ns = std::move(ns), .initMode = true})) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("Initialized package '{}'\n", name);
    }
    return 0;
}

int Cli::RunNew(const std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view name;
    bool executable = false;
    bool shared = false;
    bool staticLibrary = false;
    bool source = false;
    std::optional<IdentitySegment> ns;
    std::string_view customPath;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "--executable") {
            executable = true;
            continue;
        }
        if (arg == "--shared") {
            shared = true;
            continue;
        }
        if (arg == "--static") {
            staticLibrary = true;
            continue;
        }
        if (arg == "--source") {
            source = true;
            continue;
        }
        if (arg == "--namespace") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--namespace' requires an argument\n");
                return 1;
            }
            if (!ParseNamespace(args[++i], ns)) {
                return 1;
            }
            continue;
        }
        if (arg == "--path" && i + 1 < args.size()) {
            customPath = args[++i];
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("new");
            return 0;
        }
        if (!arg.starts_with('-') && name.empty()) {
            name = arg;
            continue;
        }
        PrintUnknownOption(arg, "new");
        return 1;
    }
    if (name.empty()) {
        std::print(stderr, "error: missing package name\n\n");
        PrintHelpFor("new");
        return 1;
    }
    const auto type = SelectPackageType(executable, shared, staticLibrary, source);
    if (!type) {
        PrintHelpFor("new");
        return 1;
    }
    std::filesystem::path root;
    if (!customPath.empty()) {
        root = std::filesystem::path(customPath) / name;
    }
    else {
        root = std::filesystem::current_path() / name;
    }
    if (!opts.quiet) {
        std::print("Creating {} package '{}'\n", KindLabel(*type), std::string(name));
    }
    if (!ScaffoldPackage(
            {.root = root, .name = std::string(name), .type = *type, .ns = std::move(ns), .initMode = false})) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("Created package '{}' at {}\n", std::string(name), root.string());
    }
    return 0;
}
