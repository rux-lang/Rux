// Immutable command-line contract shared by parsing and help rendering.

#include "Cli/CliSpec.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

using namespace Rux::CliContract;
using namespace std::string_view_literals;

namespace {
using OptionDoc = OptionSpec;
using CommandDoc = CommandSpec;

namespace Data {
// Add
constexpr std::array add_usage = {"[namespace]/[package]"sv, "[namespace]/[package]@[requirement]"sv,
                                  "[package] --path [path]"sv};
constexpr std::array add_opts = {
    OptionDoc{.flags = "--path <path>"sv, .desc = "Add a local path-based dependency"sv},
    OptionDoc{.flags = "--registry <url>"sv, .desc = "Registry API base URL to check the package against"sv}};
constexpr std::array add_exs = {"Rux/Io"sv, "Rux/Io@^0.1.0"sv, "Json --path ../Json"sv};

// Build
constexpr std::array build_usage = {"[options]"sv};
constexpr std::array build_opts = {
    OptionDoc{.flags = "--all"sv,
              .desc =
                  "Build all 16 target/profile cells; incompatible with --target, --debug, --release, and --emit"sv},
    OptionDoc{.flags = "--debug"sv, .desc = "Build with debug symbols (unoptimized output)"sv},
    OptionDoc{.flags = "--define <name[=value]>"sv, .desc = "Set or override a config compile-time value"sv},
    OptionDoc{.flags = "--emit <kind[,kind...]>"sv,
              .desc = "Additionally emit tokens, ast, sema, hir, lir, asm, or rcu inspection output"sv},
    OptionDoc{.flags = "--release"sv, .desc = "Build with release profile (optimized, no debug info)"sv},
    OptionDoc{.flags = "--stats"sv, .desc = "Print build timing, source, performance, and output statistics"sv},
    OptionDoc{.flags = "--target <triple>"sv,
              .desc = "Build for the specified target platform (e.g. linux-x86_64, linux-aarch64)"sv},
    OptionDoc{.flags = "-q, --quiet"sv, .desc = "Suppress non-essential output (only errors are shown)"sv},
    OptionDoc{.flags = "-v, --verbose"sv, .desc = "Enable verbose output for detailed build information"sv}};
constexpr std::array build_conflicts = {
    OptionConflict{.left = "--all"sv, .right = "--target"sv},
    OptionConflict{.left = "--all"sv, .right = "--debug"sv},
    OptionConflict{.left = "--all"sv, .right = "--release"sv},
    OptionConflict{.left = "--all"sv, .right = "--emit"sv},
    OptionConflict{.left = "--debug"sv, .right = "--release"sv},
};
constexpr std::array build_exs = {
    ""sv,        "--all --stats"sv,       "--debug"sv,        "--release --target linux-x86_64"sv,
    "--stats"sv, "--verbose --release"sv, "--emit ast,sema"sv};

// Check
constexpr std::array check_usage = {"[options]"sv};
constexpr std::array check_opts = {
    OptionDoc{.flags = "--define <name[=value]>"sv, .desc = "Set or override a config compile-time value"sv},
    OptionDoc{.flags = "--json"sv, .desc = "Output diagnostics in JSON format"sv},
    OptionDoc{.flags = "--target <triple>"sv, .desc = "Check code health for a specific target platform"sv}};
constexpr std::array check_exs = {""sv, "--json"sv, "--target windows-x86_64"sv};

// Clean
constexpr std::array clean_usage = {"[options]"sv};
constexpr std::array clean_opts = {
    OptionDoc{.flags = "--temp"sv, .desc = "Remove only the temporary build directory (Temp/)"sv}};
constexpr std::array clean_exs = {""sv, "--temp"sv};

// Doc
constexpr std::array doc_usage = {"[options]"sv};
constexpr std::array doc_opts = {
    OptionDoc{.flags = "--define <name[=value]>"sv, .desc = "Set or override a config compile-time value"sv},
    OptionDoc{.flags = "--document-private-items"sv, .desc = "Include private declarations and members"sv},
    OptionDoc{.flags = "-o, --output <dir>"sv, .desc = "Write the generated site to this directory"sv},
    OptionDoc{.flags = "--open"sv, .desc = "Open the generated documentation index in a browser"sv},
    OptionDoc{.flags = "--target <triple>"sv, .desc = "Document APIs enabled for a supported target"sv}};
constexpr std::array doc_exs = {""sv, "--open"sv};

// Fmt
constexpr std::array fmt_usage = {"[options]"sv};
constexpr std::array fmt_opts = {
    OptionDoc{.flags = "--check"sv, .desc = "Check file formatting status without modifying source files"sv},
    OptionDoc{.flags = "--manifest-only"sv, .desc = "Format only the manifest configuration file (Rux.toml)"sv},
    OptionDoc{.flags = "--source-only"sv, .desc = "Format only Rux source files"sv}};
constexpr std::array fmt_conflicts = {
    OptionConflict{.left = "--source-only"sv, .right = "--manifest-only"sv},
};
constexpr std::array fmt_exs = {""sv, "--check"sv, "--manifest-only"sv};

// Lint
constexpr std::array lint_usage = {"[options]"sv};
constexpr std::array<OptionDoc, 0> lint_opts = {};
constexpr std::array lint_exs = {""sv, "--verbose"sv};

// Info
constexpr std::array info_usage = {""sv, "[namespace]/[package]"sv, "[namespace]/[package]@[requirement]"sv};
constexpr std::array info_opts = {
    OptionDoc{.flags = "--json"sv, .desc = "Output package metadata in JSON format"sv},
    OptionDoc{.flags = "--registry <url>"sv, .desc = "Registry API base URL to look an uninstalled package up on"sv}};
constexpr std::array info_exs = {""sv, "Rux/Io"sv, "Rux/Io@0.1.0"sv, "--json"sv};

// Init
constexpr std::array init_usage = {"[options]"sv};
constexpr std::array init_opts = {
    OptionDoc{.flags = "--executable"sv, .desc = "Create an Executable package with an entry point (default)"sv},
    OptionDoc{.flags = "--shared"sv, .desc = "Create a SharedLibrary package"sv},
    OptionDoc{.flags = "--static"sv, .desc = "Create a StaticLibrary package"sv},
    OptionDoc{.flags = "--source"sv, .desc = "Create a SourceLibrary package compiled into its dependents"sv},
    OptionDoc{.flags = "--namespace <ns>"sv, .desc = "Set the registry namespace required to publish"sv}};
constexpr std::array init_exs = {""sv, "--executable"sv, "--shared --namespace Rux"sv, "--static"sv, "--source"sv};

constexpr std::array package_kind_conflicts = {
    OptionConflict{.left = "--executable"sv, .right = "--shared"sv},
    OptionConflict{.left = "--executable"sv, .right = "--static"sv},
    OptionConflict{.left = "--executable"sv, .right = "--source"sv},
    OptionConflict{.left = "--shared"sv, .right = "--static"sv},
    OptionConflict{.left = "--shared"sv, .right = "--source"sv},
    OptionConflict{.left = "--static"sv, .right = "--source"sv},
};

// Install
constexpr std::array install_usage = {""sv, "[namespace]/[package]"sv, "[namespace]/[package]@[requirement]"sv};
constexpr std::array install_opts = {
    OptionDoc{.flags = "--registry <url>"sv, .desc = "Registry API base URL to resolve and download from"sv},
    OptionDoc{.flags = "--target <triple>"sv, .desc = "Install dependencies applicable to a target platform"sv}};
constexpr std::array install_exs = {""sv, "Rux/Io"sv, "Rux/Io@^0.1.0"sv, "--target windows-x86_64"sv,
                                    "--registry http://localhost:8080"sv};

// List
constexpr std::array list_usage = {"[options]"sv};
constexpr std::array list_opts = {OptionDoc{
    .flags = "--global"sv, .desc = "List packages in the global environment cache instead of the local manifest"sv}};
constexpr std::array list_exs = {""sv, "--global"sv};

// Login
constexpr std::array login_usage = {"[options]"sv};
constexpr std::array login_opts = {
    OptionDoc{.flags = "--registry <url>"sv, .desc = "Registry API base URL to authenticate to"sv}};
constexpr std::array login_exs = {""sv, "--registry http://localhost:8080"sv};

// Logout
constexpr std::array logout_usage = {"[options]"sv};
constexpr std::array logout_opts = {
    OptionDoc{.flags = "--registry <url>"sv, .desc = "Registry API base URL to forget the token for"sv}};
constexpr std::array logout_exs = {""sv, "--registry http://localhost:8080"sv};

// New
constexpr std::array new_usage = {"[name] [options]"sv};
constexpr std::array new_opts = {
    OptionDoc{.flags = "--executable"sv, .desc = "Create an Executable package with an entry point (default)"sv},
    OptionDoc{.flags = "--shared"sv, .desc = "Create a SharedLibrary package"sv},
    OptionDoc{.flags = "--static"sv, .desc = "Create a StaticLibrary package"sv},
    OptionDoc{.flags = "--source"sv, .desc = "Create a SourceLibrary package compiled into its dependents"sv},
    OptionDoc{.flags = "--namespace <ns>"sv, .desc = "Set the registry namespace required to publish"sv},
    OptionDoc{.flags = "--path <dir>"sv, .desc = "Create the workspace in a specific directory"sv}};
constexpr std::array new_exs = {"App"sv, "App --executable"sv, "Io --shared --namespace Rux"sv, "Math --static"sv,
                                "Json --source"sv};

// Pack
constexpr std::array pack_usage = {"[options]"sv};
constexpr std::array pack_opts = {
    OptionDoc{.flags = "-o, --output <path>"sv, .desc = "Write the archive to a specific path"sv}};
constexpr std::array pack_exs = {""sv, "--output Dist/Math.ruxpkg"sv};

// Publish
constexpr std::array publish_usage = {"[options]"sv};
constexpr std::array publish_opts = {
    OptionDoc{.flags = "--registry <url>"sv, .desc = "Registry API base URL to publish to"sv},
    OptionDoc{.flags = "--dry-run"sv, .desc = "Validate and build the archive without uploading"sv}};
constexpr std::array publish_exs = {""sv, "--dry-run"sv, "--registry http://localhost:8080"sv};

// Remove
constexpr std::array remove_usage = {"[name]"sv};
constexpr std::array remove_exs = {"Json"sv, "Random"sv};

// Run
constexpr std::array run_usage = {"[options] [-- args...]"sv};
constexpr std::array run_opts = {
    OptionDoc{.flags = "--define <name[=value]>"sv, .desc = "Set or override a config compile-time value"sv},
    OptionDoc{.flags = "--release"sv, .desc = "Build with release profile"sv}};
constexpr std::array run_exs = {""sv, "--release"sv, "-- --port 8080"sv};

// Test
constexpr std::array test_usage = {"[options]"sv};
constexpr std::array test_opts = {
    OptionDoc{.flags = "--define <name[=value]>"sv, .desc = "Set or override a config compile-time value"sv},
    OptionDoc{.flags = "--release"sv, .desc = "Build with release profile"sv},
    OptionDoc{.flags = "--target <triple>"sv, .desc = "Build and run the tests for the specified target platform"sv}};
constexpr std::array test_exs = {""sv, "--release"sv};

// Uninstall
constexpr std::array uninstall_usage = {""sv, "[namespace]/[package]"sv, "[namespace]/[package]@[requirement]"sv,
                                        "[options]"sv};
constexpr std::array uninstall_opts = {
    OptionDoc{.flags = "--global"sv, .desc = "Uninstall every package in the global environment cache"sv}};
constexpr std::array uninstall_exs = {""sv, "Rux/Json"sv, "Rux/Json@0.1.0"sv, "--global"sv};

// Update
constexpr std::array update_usage = {"[options]"sv};
constexpr std::array update_opts = {
    OptionDoc{.flags = "--global"sv, .desc = "Update all entries in the global environment cache"sv},
    OptionDoc{.flags = "--registry <url>"sv, .desc = "Registry API base URL to resolve and download from"sv},
    OptionDoc{.flags = "--target <triple>"sv, .desc = "Update dependencies applicable to a target platform"sv}};
constexpr std::array update_exs = {""sv, "--global"sv, "--target linux-aarch64"sv,
                                   "--registry http://localhost:8080"sv};

// Version
constexpr std::array version_exs = {""sv, "rux -V"sv, "rux --version"sv};

constexpr std::array help_opts = {
    OptionDoc{.flags = "--json"sv, .desc = "Print the versioned command contract as JSON"sv}};
} // namespace Data

namespace GlobalOpts {
constexpr std::array catalog = {
    OptionDoc{.flags = "--color <auto|always|never>"sv, .desc = "Control colored console output"sv},
    OptionDoc{.flags = "-h, --help"sv, .desc = "Show help information"sv},
    OptionDoc{.flags = "--manifest <path>"sv, .desc = "Use the specified manifest file instead of Rux.toml"sv},
    OptionDoc{.flags = "-q, --quiet"sv, .desc = "Do not show log messages"sv},
    OptionDoc{.flags = "-v, --verbose"sv, .desc = "Use verbose output"sv},
    OptionDoc{.flags = "-V, --version"sv, .desc = "Show version information"sv}};
} // namespace GlobalOpts

constexpr std::array G_COMMAND_HELP_MAPS = {
    CommandDoc{.name = "add"sv,
               .shortDesc = "Add a dependency to the manifest"sv,
               .description =
                   "Add a brand new external dependency link directly into the package configuration file."sv,
               .usage = Data::add_usage,
               .postUsage = {},
               .footer = "This command updates Rux.toml accordingly."sv,
               .examples = Data::add_exs,
               .options = Data::add_opts,
               .conflicts = {}},

    CommandDoc{.name = "build"sv,
               .shortDesc = "Build the current package"sv,
               .description = {} /* Fallback */,
               .usage = Data::build_usage,
               .postUsage = {},
               .footer = "Artifacts are stored under <Output>/<Profile>/<Target>, defaulting below Bin/."sv,
               .examples = Data::build_exs,
               .options = Data::build_opts,
               .conflicts = Data::build_conflicts},

    CommandDoc{.name = "check"sv,
               .shortDesc = "Check package source code for errors without building"sv,
               .description = "Parse and analyze the source workspace files for syntactic or semantic safety flaws "
                              "without emitting compilation binaries."sv,
               .usage = Data::check_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::check_exs,
               .options = Data::check_opts,
               .conflicts = {}},

    CommandDoc{.name = "clean"sv,
               .shortDesc = "Remove build artifacts"sv,
               .description = "Purge compiled code modules, system logs, and cached local build tracking states from "
                              "the environment workspace."sv,
               .usage = Data::clean_usage,
               .postUsage = "Removes the configured build output directory and Temp/ folder."sv,
               .footer = {},
               .examples = Data::clean_exs,
               .options = Data::clean_opts,
               .conflicts = {}},

    CommandDoc{.name = "doc"sv,
               .shortDesc = "Generate package documentation"sv,
               .description = {} /* Fallback */,
               .usage = Data::doc_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::doc_exs,
               .options = Data::doc_opts,
               .conflicts = {}},

    CommandDoc{.name = "fmt"sv,
               .shortDesc = "Format source files and manifests"sv,
               .description = "Format all *.rux source files and *.toml manifests"sv,
               .usage = Data::fmt_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::fmt_exs,
               .options = Data::fmt_opts,
               .conflicts = Data::fmt_conflicts},

    CommandDoc{.name = "help"sv,
               .shortDesc = "Show help information"sv,
               .description = {},
               .usage = {},
               .postUsage = {},
               .footer = {},
               .examples = {},
               .options = Data::help_opts,
               .conflicts = {}},

    CommandDoc{.name = "info"sv,
               .shortDesc = "Show package metadata and manifest information"sv,
               .description = "Show information about the current or an installed Rux package"sv,
               .usage = Data::info_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::info_exs,
               .options = Data::info_opts,
               .conflicts = {}},

    CommandDoc{.name = "init"sv,
               .shortDesc = "Initialize a Rux package in the current directory"sv,
               .description = {},
               .usage = Data::init_usage,
               .postUsage = {},
               .footer = "If Rux.toml does not exist, it will be created."sv,
               .examples = Data::init_exs,
               .options = Data::init_opts,
               .conflicts = Data::package_kind_conflicts},

    CommandDoc{.name = "install"sv,
               .shortDesc = "Install dependencies"sv,
               .description = {} /* Fallback */,
               .usage = Data::install_usage,
               .postUsage = "Without a package name, downloads all registry dependencies listed in Rux.toml. For a "
                            "workspace, dependencies from every member package are installed in one pass. If no "
                            "manifest exists, packages are discovered from Tests/ and immediate member directories.\n"
                            "With a package name, downloads it and its transitive dependencies to the package cache; "
                            "use 'rux add' to declare it in Rux.toml.\n"
                            "TargetOS conditions use the host target unless --target selects another supported "
                            "target triple.\n"
                            "Each package is resolved through the registry index, downloaded as a published archive "
                            "and checked against the SHA-256 the registry publishes for it. Versions are cached side "
                            "by side, so a build selects the one its requirement matches."sv,
               .footer = {},
               .examples = Data::install_exs,
               .options = Data::install_opts,
               .conflicts = {}},

    CommandDoc{.name = "lint"sv,
               .shortDesc = "Lint package source files"sv,
               .description = "Run source-level diagnostics without emitting build artifacts"sv,
               .usage = Data::lint_usage,
               .postUsage = {},
               .footer = "Use 'rux check' for complete package or workspace semantic validation."sv,
               .examples = Data::lint_exs,
               .options = Data::lint_opts,
               .conflicts = {}},

    CommandDoc{.name = "list"sv,
               .shortDesc = "List dependencies"sv,
               .description = "List packages in the manifest file"sv,
               .usage = Data::list_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::list_exs,
               .options = Data::list_opts,
               .conflicts = {}},

    CommandDoc{.name = "login"sv,
               .shortDesc = "Store a registry token for publishing"sv,
               .description = "Authenticate to a Rux package registry"sv,
               .usage = Data::login_usage,
               .postUsage = "The token is read from stdin, never from an option, so it stays out of shell "
                            "history and the process list. A terminal is prompted without echo; otherwise a "
                            "single line is read, which is what makes 'echo $TOKEN | rux login' work.\n"
                            "It is stored per registry in a file only you can read, and RUX_TOKEN still "
                            "overrides it when set."sv,
               .footer = {},
               .examples = Data::login_exs,
               .options = Data::login_opts,
               .conflicts = {}},

    CommandDoc{.name = "logout"sv,
               .shortDesc = "Remove a stored registry token"sv,
               .description = "Forget the token stored for a Rux package registry"sv,
               .usage = Data::logout_usage,
               .postUsage = "Only the entry for the selected registry is removed; tokens stored for other "
                            "registries are kept. RUX_TOKEN is an environment variable and is unaffected."sv,
               .footer = {},
               .examples = Data::logout_exs,
               .options = Data::logout_opts,
               .conflicts = {}},

    CommandDoc{.name = "new"sv,
               .shortDesc = "Create a new Rux package"sv,
               .description = "Create a new Rux package in a new directory"sv,
               .usage = Data::new_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::new_exs,
               .options = Data::new_opts,
               .conflicts = Data::package_kind_conflicts},

    CommandDoc{.name = "pack"sv,
               .shortDesc = "Build the publishable package archive"sv,
               .description = "Build the .ruxpkg archive that 'rux publish' uploads"sv,
               .usage = Data::pack_usage,
               .postUsage = "The archive contains Rux.toml, every file below Src/, and the files named by "
                            "ReadmeFile and LicenseFile. Without --output it is written to the build output "
                            "directory as <Name>-<Version>.ruxpkg.\n"
                            "The package must satisfy the same rules 'rux publish' enforces."sv,
               .footer = {},
               .examples = Data::pack_exs,
               .options = Data::pack_opts,
               .conflicts = {}},

    CommandDoc{.name = "publish"sv,
               .shortDesc = "Publish the package to the registry"sv,
               .description = "Upload the package archive to a Rux package registry"sv,
               .usage = Data::publish_usage,
               .postUsage = "Publication requires [Package].Namespace and [Manifest].MinRux, and rejects "
                            "workspaces and path dependencies. Published versions are immutable.\n"
                            "The bearer credential comes from the RUX_TOKEN environment variable, or from the "
                            "token 'rux login' stored for this registry, and must carry the 'publish' scope. "
                            "The registry defaults to RUX_REGISTRY_URL when set."sv,
               .footer = {},
               .examples = Data::publish_exs,
               .options = Data::publish_opts,
               .conflicts = {}},

    CommandDoc{.name = "remove"sv,
               .shortDesc = "Remove a dependency from the manifest"sv,
               .description = {} /* Fallback */,
               .usage = Data::remove_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::remove_exs,
               .options = {},
               .conflicts = {}},

    CommandDoc{.name = "run"sv,
               .shortDesc = "Build and run the main executable"sv,
               .description = "Build and execute a runnable target"sv,
               .usage = Data::run_usage,
               .postUsage = "Arguments after '--' are forwarded to the executable."sv,
               .footer = {},
               .examples = Data::run_exs,
               .options = Data::run_opts,
               .conflicts = {}},

    CommandDoc{.name = "test"sv,
               .shortDesc = "Run all test targets"sv,
               .description = "Run package unit tests"sv,
               .usage = Data::test_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::test_exs,
               .options = Data::test_opts,
               .conflicts = {}},

    CommandDoc{.name = "uninstall"sv,
               .shortDesc = "Uninstall dependencies"sv,
               .description = "Uninstall dependencies from the local cache"sv,
               .usage = Data::uninstall_usage,
               .postUsage = "Without a package name, removes every installed version of the registry dependencies "
                            "listed in Rux.toml from the local cache.\nWith a package name, removes every installed "
                            "version of it, or only the versions an added @requirement matches.\nWith --global, "
                            "removes every package present in the cache, whether or not Rux.toml declares it."sv,
               .footer = {},
               .examples = Data::uninstall_exs,
               .options = Data::uninstall_opts,
               .conflicts = {}},

    CommandDoc{.name = "update"sv,
               .shortDesc = "Update dependencies"sv,
               .description = {} /* Fallback */,
               .usage = Data::update_usage,
               .postUsage = "Without --global, re-resolves every registry dependency listed in Rux.toml against the "
                            "registry and installs the newest version each requirement allows.\nWith --global, moves "
                            "every package present in the local cache to its newest published release.\nOlder "
                            "versions stay installed; 'rux uninstall' removes them. TargetOS conditions use the host "
                            "target unless --target selects another supported target triple."sv,
               .footer = {},
               .examples = Data::update_exs,
               .options = Data::update_opts,
               .conflicts = {}},

    CommandDoc{.name = "version"sv,
               .shortDesc = "Show version information"sv,
               .description = "Show information about the Rux toolchain version"sv,
               .usage = {},
               .postUsage = {},
               .footer = {},
               .examples = Data::version_exs,
               .options = {},
               .conflicts = {}}};

constexpr auto VerifyRegistryIntegrity() -> bool {
    if constexpr (constexpr auto dup = std::ranges::adjacent_find(G_COMMAND_HELP_MAPS, {}, &CommandDoc::name);
                  dup != G_COMMAND_HELP_MAPS.end()) {
        return false;
    }
    return std::ranges::all_of(G_COMMAND_HELP_MAPS, [](const CommandDoc &cmd) -> bool {
        if (cmd.name.empty() || cmd.shortDesc.empty()) {
            return false;
        }
        if (!cmd.description.empty() && cmd.description == cmd.shortDesc) {
            return false;
        }
        const bool hasBadOptions = std::ranges::any_of(cmd.options, [](const OptionDoc &opt) -> bool {
            return opt.flags.empty() || opt.desc.empty() || opt.flags.starts_with(' ') || opt.flags.ends_with(' ');
        });
        if (hasBadOptions) {
            return false;
        }
        const bool hasBadConflict = std::ranges::any_of(cmd.conflicts, [](const OptionConflict &conflict) -> bool {
            return conflict.left.empty() || conflict.right.empty() || conflict.left == conflict.right;
        });
        if (hasBadConflict) {
            return false;
        }
        const bool hasBadUsage = std::ranges::any_of(cmd.usage, [](const std::string_view usage) -> bool {
            return usage.starts_with(' ') || usage.ends_with(' ');
        });
        return !hasBadUsage;
    });
}

static_assert(std::ranges::is_sorted(G_COMMAND_HELP_MAPS, std::less<>{}, &CommandDoc::name),
              "G_COMMAND_HELP_MAPS registry map entries must be strictly sorted "
              "alphabetically by name!");

static_assert(VerifyRegistryIntegrity(), "Duplicate commands, empty descriptions, or redundant "
                                         "metadata detected!");

} // namespace

namespace Rux::CliContract {
std::span<const OptionSpec> GlobalOptions() {
    return GlobalOpts::catalog;
}

std::span<const CommandSpec> Commands() {
    return G_COMMAND_HELP_MAPS;
}

const CommandSpec *FindCommand(const std::string_view name) {
    const auto it = std::ranges::lower_bound(G_COMMAND_HELP_MAPS, name, std::less<>{}, &CommandSpec::name);
    return it != G_COMMAND_HELP_MAPS.end() && it->name == name ? &*it : nullptr;
}

bool OptionMatches(const OptionSpec &option, std::string_view spelling) {
    const auto equals = spelling.find('=');
    if (equals != std::string_view::npos) {
        spelling = spelling.substr(0, equals);
    }
    std::string_view flags = option.flags;
    while (!flags.empty()) {
        const auto comma = flags.find(',');
        auto flag = flags.substr(0, comma);
        flag.remove_prefix(std::min(flag.find_first_not_of(' '), flag.size()));
        flag = flag.substr(0, flag.find(' '));
        if (flag == spelling) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        flags.remove_prefix(comma + 1);
    }
    return false;
}

bool OptionTakesValue(const OptionSpec &option) {
    return option.flags.find('<') != std::string_view::npos;
}

std::string_view PreferredOptionName(const OptionSpec &option) {
    std::string_view preferred;
    std::string_view flags = option.flags;
    while (!flags.empty()) {
        const auto comma = flags.find(',');
        auto flag = flags.substr(0, comma);
        flag.remove_prefix(std::min(flag.find_first_not_of(' '), flag.size()));
        flag = flag.substr(0, flag.find(' '));
        if (preferred.empty() || flag.starts_with("--")) {
            preferred = flag;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        flags.remove_prefix(comma + 1);
    }
    return preferred;
}

const PositionalSpec &PositionalsFor(const std::string_view command) {
    static constexpr std::array packageArg = {
        ArgumentSpec{.name = "package", .description = "A namespaced package identity and optional requirement"}};
    static constexpr std::array requiredPackageArg = {ArgumentSpec{
        .name = "package", .description = "A namespaced package identity and optional requirement", .required = true}};
    static constexpr std::array nameArg = {
        ArgumentSpec{.name = "name", .description = "The package or dependency name", .required = true}};
    static constexpr std::array commandArg = {
        ArgumentSpec{.name = "command", .description = "The command whose help should be shown"}};
    static constexpr std::array specs = {
        PositionalSpec{.command = "add", .arguments = requiredPackageArg, .minimum = 1, .maximum = 1},
        PositionalSpec{.command = "help", .arguments = commandArg, .minimum = 0, .maximum = 1},
        PositionalSpec{.command = "info", .arguments = packageArg, .minimum = 0, .maximum = 1},
        PositionalSpec{.command = "install", .arguments = packageArg, .minimum = 0, .maximum = 1},
        PositionalSpec{.command = "new", .arguments = nameArg, .minimum = 1, .maximum = 1},
        PositionalSpec{.command = "remove", .arguments = nameArg, .minimum = 1, .maximum = 1},
        PositionalSpec{.command = "run", .arguments = {}, .minimum = 0, .maximum = 0, .acceptsPassthrough = true},
        PositionalSpec{.command = "uninstall", .arguments = packageArg, .minimum = 0, .maximum = 1}};
    static constexpr PositionalSpec none{};
    const auto it = std::ranges::find(specs, command, &PositionalSpec::command);
    return it == specs.end() ? none : *it;
}
} // namespace Rux::CliContract
