// `rux new` and `rux init` — package scaffolding.

#include "Cli/Cli.h"
#include "Cli/TerminalStyle.h"
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
using namespace CliSupport;

namespace {

// The three Version 1 package kinds map one-to-one onto the scaffolding flags,
// which are mutually exclusive rather than letting one silently win.
std::optional<ManifestPackageType> SelectPackageType(const bool bin, const bool lib, const bool source) {
    if (static_cast<int>(bin) + static_cast<int>(lib) + static_cast<int>(source) > 1) {
        std::print(stderr, "error: '--bin', '--lib' and '--source' cannot be combined\n\n");
        return std::nullopt;
    }
    if (lib) {
        return ManifestPackageType::Library;
    }
    if (source) {
        return ManifestPackageType::Source;
    }
    return ManifestPackageType::Program;
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
    case ManifestPackageType::Library:
        return "library";
    case ManifestPackageType::Source:
        return "source";
    case ManifestPackageType::Program:
        break;
    }
    return "program";
}

} // namespace

int Cli::RunInit(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool bin = false;
    bool lib = false;
    bool source = false;
    std::optional<IdentitySegment> ns;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--bin") {
            bin = true;
            continue;
        }
        if (arg == "--lib") {
            lib = true;
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
    const auto type = SelectPackageType(bin, lib, source);
    if (!type) {
        PrintHelpFor("init");
        return 1;
    }
    const auto root = std::filesystem::current_path();
    auto name = root.filename().string();
    if (!opts.quiet) {
        std::print("{} {} package '{}'\n", Status("Initializing"), KindLabel(*type), name);
    }
    if (!ScaffoldPackage({.root = root, .name = name, .type = *type, .ns = std::move(ns), .initMode = true})) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("{} package '{}'\n", Status("Initialized"), name);
    }
    return 0;
}

int Cli::RunNew(const std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view name;
    bool bin = false;
    bool lib = false;
    bool source = false;
    std::optional<IdentitySegment> ns;
    std::string_view customPath;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "--bin") {
            bin = true;
            continue;
        }
        if (arg == "--lib") {
            lib = true;
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
    const auto type = SelectPackageType(bin, lib, source);
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
