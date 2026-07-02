// `rux new` and `rux init` — package scaffolding.

#include "Cli/Cli.h"
#include "Package/Package.h"

#include <cstdio>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>

using namespace Rux;

int Cli::RunInit(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool bin = false;
    bool lib = false;
    for (auto &arg : args) {
        if (arg == "--bin") {
            bin = true;
            continue;
        }
        if (arg == "--lib") {
            lib = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("init");
            return 0;
        }
        PrintUnknownOption(arg, "init");
        return 1;
    }
    const auto type = (lib && !bin) ? PackageType::SharedLibrary : PackageType::Executable;
    const auto root = std::filesystem::current_path();
    auto name = root.filename().string();
    if (!opts.quiet) {
        std::print("  Initializing {} package '{}'\n", type == PackageType::Executable ? "binary" : "library", name);
    }
    if (!ScaffoldPackage(root, name, type, /*initMode=*/true)) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("   Initialized package '{}'\n", name);
    }
    return 0;
}

int Cli::RunNew(const std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view name;
    bool bin = false;
    bool lib = false;
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
    const auto type = (lib && !bin) ? PackageType::SharedLibrary : PackageType::Executable;
    std::filesystem::path root;
    if (!customPath.empty()) {
        root = std::filesystem::path(customPath) / name;
    }
    else {
        root = std::filesystem::current_path() / name;
    }
    if (!opts.quiet) {
        std::print("Creating {} package '{}'\n", type == PackageType::Executable ? "binary" : "library",
                   std::string(name));
    }
    if (!ScaffoldPackage(root, std::string(name), type, /*initMode=*/false)) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("Created package '{}' at {}\n", std::string(name), root.string());
    }
    return 0;
}
