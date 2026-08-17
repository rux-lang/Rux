// `rux new` and `rux init` — package scaffolding.

#include "Cli/Cli.h"
#include "Cli/Reporter.h"
#include "Driver/BuildReport.h"
#include "Package/Package.h"
#include "Reporting/Reporting.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;
using namespace Rux::Driver;

namespace {

/// The four Version 1 package kinds map one-to-one onto the scaffolding flags, which are mutually exclusive rather than
/// letting one silently win.
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

/// Validates the optional `--namespace` operand against the identity grammar.
bool ParseNamespace(const std::string_view value, std::optional<IdentitySegment> &ns,
                    const CliSupport::Reporter &diagnostics) {
    const auto parsed = IdentitySegment::Parse(value);
    if (!parsed) {
        diagnostics.Error(std::format("namespace '{}' is invalid", value));
        diagnostics.Note(Describe(parsed.error()));
        diagnostics.Help("use 1 to 64 ASCII letters or digits, separated only by single '-' or '_' characters");
        return false;
    }
    ns = *parsed;
    return true;
}

bool ValidatePackageName(const std::string_view value, const CliSupport::Reporter &diagnostics) {
    const auto parsed = IdentitySegment::Parse(value);
    if (parsed) {
        return true;
    }
    diagnostics.Error(std::format("package name '{}' is invalid", value));
    diagnostics.Note(Describe(parsed.error()));
    diagnostics.Help("use 1 to 64 ASCII letters or digits, separated only by single '-' or '_' characters");
    return false;
}

/// Lower-case description used by the progress lines of both commands.
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

int ReportScaffoldFailure(const ScaffoldError &error, const std::string_view name, const bool initMode,
                          const CliSupport::Reporter &diagnostics, const CliSupport::Reporter &output,
                          const std::chrono::steady_clock::time_point started) {
    switch (error.kind) {
    case ScaffoldErrorKind::ExistingDestination:
        diagnostics.Error(std::format("destination '{}' already exists", error.path.string()));
        diagnostics.Help(std::format("choose another package name or run 'rux init' inside '{}'", error.path.string()));
        break;
    case ScaffoldErrorKind::InvalidName:
        diagnostics.Error(std::format("package name '{}' is invalid", name));
        diagnostics.Note(error.detail);
        diagnostics.Help("use 1 to 64 ASCII letters or digits, separated only by single '-' or '_' characters");
        break;
    case ScaffoldErrorKind::CreateDirectory:
        diagnostics.Error(std::format("could not create package directory '{}'", error.path.string()));
        diagnostics.Note(error.detail);
        diagnostics.Help("check the destination path and its directory permissions, then retry");
        break;
    case ScaffoldErrorKind::WriteFile:
        diagnostics.Error(std::format("could not write package file '{}'", error.path.string()));
        diagnostics.Note(error.detail);
        diagnostics.Help("remove a conflicting directory or check file permissions, then retry");
        break;
    }
    const auto partial = error.partialChanges.filesWritten + error.partialChanges.directoryTreesCreated;
    if (partial > 0) {
        diagnostics.Note("the destination contains files or directories created by this attempt");
        if (initMode) {
            diagnostics.Help("fix the reported path and run 'rux init' again");
        }
        else {
            diagnostics.Help("remove the partially created destination before retrying for a clean scaffold");
        }
    }
    output.Failure("Failed", std::format("to {} package '{}' in {}", initMode ? "initialize" : "create", name,
                                         Reporting::FormatDuration(ElapsedMs(started))));
    return 1;
}

void ReportScaffoldSuccess(const ScaffoldChanges &changes, const std::filesystem::path &root,
                           const std::string_view name, const ManifestPackageType type, const bool initMode,
                           const CliSupport::Reporter &output, const std::chrono::steady_clock::time_point started) {
    output.Success(initMode ? "Initialized" : "Created",
                   initMode ? std::format("package '{}' in {}", name, Reporting::FormatDuration(ElapsedMs(started)))
                            : std::format("{} package '{}' in {}", KindLabel(type), name,
                                          Reporting::FormatDuration(ElapsedMs(started))));
    output.Detail(std::format("Path: {}", root.string()));
    output.Detail(std::format("Files: {} written, {} preserved", changes.filesWritten, changes.filesPreserved));
}

} // namespace

int Cli::RunInit(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
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
            if (!ParseNamespace(args[++i], ns, diagnostics)) {
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
    if (!ValidatePackageName(name, diagnostics)) {
        return 1;
    }
    output.Progress("Initializing", std::format("package '{}'", name));
    const auto started = std::chrono::steady_clock::now();
    auto scaffold = ScaffoldPackage({.root = root, .name = name, .type = *type, .ns = std::move(ns), .initMode = true});
    if (!scaffold) {
        return ReportScaffoldFailure(scaffold.error(), name, true, diagnostics, output, started);
    }
    ReportScaffoldSuccess(*scaffold, root, name, *type, true, output, started);
    return 0;
}

int Cli::RunNew(const std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
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
            if (!ParseNamespace(args[++i], ns, diagnostics)) {
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
    if (!ValidatePackageName(name, diagnostics)) {
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
    output.Progress("Creating", std::format("{} package '{}'", KindLabel(*type), name));
    const auto started = std::chrono::steady_clock::now();
    auto scaffold = ScaffoldPackage(
        {.root = root, .name = std::string(name), .type = *type, .ns = std::move(ns), .initMode = false});
    if (!scaffold) {
        return ReportScaffoldFailure(scaffold.error(), name, false, diagnostics, output, started);
    }
    ReportScaffoldSuccess(*scaffold, root, name, *type, false, output, started);
    return 0;
}
