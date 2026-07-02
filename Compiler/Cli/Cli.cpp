#include "Cli/Cli.h"

#include "Backend/Link/Linker.h"
#include "Backend/Rcu/Rcu.h"
#include "Backend/X64/Asm.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Frontend/Ast/Ast.h"
#include "Frontend/Lexer.h"
#include "Frontend/Parser/Parser.h"
#include "Frontend/Sema/Sema.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Package/Manifest.h"
#include "Package/Package.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"
#include "Platform/Target.h"
#include "Platform/Terminal.h"
#include "Support/Diagnostics.h"
#include "Support/Version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * This is separate from the other ifdef because otherwise clang-format attempts
 * to change the order, which makes MSVC cry.
 */

#if RUX_OS_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <windows.h>
#endif

#if RUX_OS_WINDOWS
    #include <psapi.h>
#else
    #include <fcntl.h>
    #include <sys/ioctl.h>
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #if defined(__has_include)
        #if __has_include(<sys/termios.h>)
            #include <sys/termios.h>
        #elif __has_include(<termios.h>)
            #include <termios.h>
        #endif
        #if __has_include(<stropts.h>)
            #include <stropts.h>
        #endif
    #endif
#endif

#include "Frontend/SourceLoader.h"

using namespace Rux;
using namespace Platform;
using namespace Misc;
using namespace std::string_view_literals;

namespace {
struct OptionDoc {
    std::string_view flags;
    std::string_view desc;
};

struct CommandDoc {
    std::string_view name;
    std::string_view shortDesc;
    std::string_view description; // If empty, falls back to shortDesc

    std::span<const std::string_view> usage;
    std::string_view postUsage;
    std::string_view footer;
    std::span<const std::string_view> examples;
    std::span<const OptionDoc> options;
};

// Matches any type compatible with std::size(), returning a value
// implicitly convertible to std::size_t.
template <typename T>
concept Sizable = requires(const T &val) {
    { std::size(val) } -> std::convertible_to<std::size_t>;
};

template <std::ranges::input_range Range, typename Proj = std::identity>
    requires Sizable<std::indirect_result_t<Proj, std::ranges::iterator_t<Range>>>
constexpr std::size_t MaxSize(const Range &range, Proj proj = {}) {
    std::size_t maxSize = 0;

    for (const auto &item : range) {
        const auto &value = std::invoke(proj, item);
        maxSize = std::max(static_cast<std::size_t>(std::size(value)), maxSize);
    }

    return maxSize;
}

namespace Layout {
constexpr std::size_t DefaultWidth = 80;
constexpr std::size_t MinTerminalWidth = 48;
constexpr std::size_t MinDescriptionWidth = 24;

constexpr std::size_t BlockIndent = 4;
constexpr std::size_t AlignedPadding = 2;

constexpr auto Whitespace = " \t"sv;
constexpr auto CliName = "rux"sv;
} // namespace Layout

std::size_t GetTerminalWidth() {
#if RUX_OS_WINDOWS
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) == TRUE) {
        const auto width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        return std::max(static_cast<std::size_t>(width), Layout::MinTerminalWidth);
    }
#else
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1 && w.ws_col > 0) {
        return std::max(static_cast<std::size_t>(w.ws_col), Layout::MinTerminalWidth);
    }
#endif

    return Layout::DefaultWidth;
}

constexpr std::size_t UsableWidth(const std::size_t terminalWidth, const std::size_t indent) {
    if (terminalWidth <= indent) {
        return Layout::MinDescriptionWidth;
    }

    return std::max(terminalWidth - indent, Layout::MinDescriptionWidth);
}

template <typename Callback>
constexpr void ProcessLine(std::string_view line, const std::size_t width, Callback callback) {
    while (!line.empty()) {
        // Skip leading spaces by sliding the view forward
        const auto firstNonSpace = line.find_first_not_of(Layout::Whitespace);
        if (firstNonSpace == std::string_view::npos) {
            break;
        }
        line.remove_prefix(firstNonSpace);
        std::size_t cutLength = line.size();
        // If the remaining line exceeds the allowed width, find a break
        // point
        if (width < line.size()) {
            cutLength = line.find_last_of(Layout::Whitespace, width);
            // If no space exists within the width constraint, force a
            // hard break at the NEXT space
            if (cutLength == std::string_view::npos) {
                cutLength = line.find_first_of(Layout::Whitespace, width);
                if (cutLength == std::string_view::npos) {
                    cutLength = line.size();
                }
            }
        }
        std::string_view piece = line.substr(0, cutLength);
        // Trim trailing spaces in O(1) via pointer arithmetic
        const auto lastNonSpace = piece.find_last_not_of(Layout::Whitespace);
        piece.remove_suffix(piece.size() - lastNonSpace - 1);
        callback(piece);
        // Slide the view forward for the next iteration
        line.remove_prefix(cutLength);
    }
}

template <typename Callback>
constexpr void Wrap(std::string_view text, const std::size_t width, Callback callback) {
    while (!text.empty()) {
        const auto newLinePos = text.find('\n');
        // substr handles npos gracefully when anchored at 0
        if (const std::string_view line = text.substr(0, newLinePos); line.empty()) {
            callback(""sv);
        }
        else {
            ProcessLine(line, width, callback);
        }
        if (newLinePos == std::string_view::npos) {
            break;
        }
        // Slide past the processed line and the newline character
        text.remove_prefix(newLinePos + 1);
    }
}

void PrintCmdLine(std::string_view cmd, std::string_view suffix) {
    std::print("{0:<{1}}", "", Layout::BlockIndent);
    // If the example is already a complete command line starting with
    // "rux", print it directly and bail out early.
    if (suffix.starts_with(Layout::CliName)) {
        std::println("{}", suffix);
        return;
    }
    // Otherwise, construct the standard "rux <cmd> <suffix>" layout
    std::print("{}", Layout::CliName);
    if (!cmd.empty()) {
        std::print(" {}", cmd);
    }
    if (!suffix.empty()) {
        std::print(" {}", suffix);
    }
    std::println();
}

void PrintBlock(std::string_view title, const std::string_view text, const size_t termWidth,
                const size_t indent = Layout::BlockIndent) {
    if (text.empty()) {
        return;
    }
    if (!title.empty()) {
        std::println("{}:", title);
    }
    const size_t usable = UsableWidth(termWidth, indent);
    Wrap(text, usable, [&](std::string_view line) -> void {
        // If the line is empty, pad with 0 spaces (prints just a
        // newline)
        std::println("{0:<{1}}{2}", "", line.empty() ? 0 : indent, line);
    });
    std::println("");
}

void PrintAligned(const std::string_view left, const std::string_view right, const size_t leftWidth,
                  const size_t termWidth) {
    const size_t indent = Layout::BlockIndent + leftWidth + Layout::AlignedPadding;
    const size_t width = UsableWidth(termWidth, indent);
    bool first = true;
    Wrap(right, width, [&](std::string_view line) {
        if (first) {
            std::println("{0:<{1}}{2:<{3}}{4}", "", Layout::BlockIndent, left, leftWidth + Layout::AlignedPadding,
                         line);
            first = false;
        }
        else {
            std::println("{0:<{1}}{2}", "", line.empty() ? 0 : indent, line);
        }
    });
    if (first) {
        std::println("{0:<{1}}{2}", "", Layout::BlockIndent, left);
    }
}

namespace Data {
// Add
constexpr std::array add_usage = {"[package]"sv, "[package]@[version]"sv, "[package] --path [path]"sv};
constexpr std::array add_opts = {OptionDoc{.flags = "--path <path>"sv, .desc = "Add a local path-based dependency"sv}};
constexpr std::array add_exs = {"Std"sv, "Std@0.1.0"sv, "Json --path ../Json"sv};

// Build
constexpr std::array build_usage = {"[options]"sv};
constexpr std::array build_opts = {
    OptionDoc{.flags = "--debug"sv, .desc = "Build with debug symbols (unoptimized output)"sv},
    OptionDoc{.flags = "--profile <n>"sv, .desc = "Build using a custom profile defined in Rux.toml"sv},
    OptionDoc{.flags = "--release"sv, .desc = "Build with release profile (optimized, no debug info)"sv},
    OptionDoc{.flags = "--stats"sv, .desc = "Print build timing, source, performance, and output statistics"sv},
    OptionDoc{.flags = "--target <triple>"sv, .desc = "Build for the specified target platform (e.g. x86, x64)"sv},
    OptionDoc{.flags = "-q, --quiet"sv, .desc = "Suppress non-essential output (only errors are shown)"sv},
    OptionDoc{.flags = "-v, --verbose"sv, .desc = "Enable verbose output for detailed build information"sv},
    OptionDoc{.flags = "--dump-asm"sv, .desc = "Write x86-64 assembly to Temp/Asm/out.asm"sv},
    OptionDoc{.flags = "--dump-ast"sv, .desc = "Write the parsed AST to Temp/Ast/<file>.ast"sv},
    OptionDoc{.flags = "--dump-hir"sv, .desc = "Write the high-level IR to Temp/Hir/hir.txt"sv},
    OptionDoc{.flags = "--dump-lir"sv, .desc = "Write the low-level IR to Temp/Lir/lir.txt"sv},
    OptionDoc{.flags = "--dump-rcu"sv, .desc = "Write RCU object files to Temp/Obj/ and text dumps to Temp/Rcu/"sv},
    OptionDoc{.flags = "--dump-sema"sv, .desc = "Write semantic analysis results to Temp/Sema/sema.txt"sv},
    OptionDoc{.flags = "--dump-tokens"sv, .desc = "Write the token stream to Temp/Tokens/<file>.tokens"sv}};
constexpr std::array build_exs = {
    ""sv,           "--debug"sv,    "--release"sv,  "--stats"sv,    "--verbose --release"sv,
    "--dump-ast"sv, "--dump-hir"sv, "--dump-lir"sv, "--dump-asm"sv, "--dump-rcu"sv};

// Check
constexpr std::array check_usage = {"[options]"sv};
constexpr std::array check_opts = {
    OptionDoc{.flags = "--json"sv, .desc = "Output diagnostics in JSON format"sv},
    OptionDoc{.flags = "--target <triple>"sv, .desc = "Check code health for a specific target platform"sv}};
constexpr std::array check_exs = {""sv, "--json"sv, "--target windows-x64"sv};

// Clean
constexpr std::array clean_usage = {"[options]"sv};
constexpr std::array clean_opts = {
    OptionDoc{.flags = "--temp"sv, .desc = "Remove only the temporary build directory (Temp/)"sv}};
constexpr std::array clean_exs = {""sv, "--temp"sv};

// Doc
constexpr std::array doc_usage = {"[options]"sv};
constexpr std::array doc_opts = {
    OptionDoc{.flags = "--open"sv, .desc = "Open the generated documentation index in a browser"sv}};
constexpr std::array doc_exs = {""sv, "--open"sv};

// Fmt
constexpr std::array fmt_usage = {"[options]"sv};
constexpr std::array fmt_opts = {
    OptionDoc{.flags = "--check"sv, .desc = "Check file formatting status without modifying source files"sv},
    OptionDoc{.flags = "--manifest-only"sv, .desc = "Format only the manifest configuration file (Rux.toml)"sv}};
constexpr std::array fmt_exs = {""sv, "--check"sv, "--manifest-only"sv};

// Info
constexpr std::array info_usage = {"[package name]"sv};
constexpr std::array info_opts = {OptionDoc{.flags = "--json"sv, .desc = "Output package metadata in JSON format"sv}};
constexpr std::array info_exs = {"Std"sv, "Windows"sv};

// Init
constexpr std::array init_usage = {"[options]"sv};
constexpr std::array init_opts = {
    OptionDoc{.flags = "--bin"sv, .desc = "Create an executable binary package configuration"sv},
    OptionDoc{.flags = "--lib"sv, .desc = "Create a library package configuration"sv}};
constexpr std::array init_exs = {""sv, "--bin"sv};

// Install
constexpr std::array install_usage = {""sv, "[package]"sv, "[package]@[version]"sv, "--dev [package]"sv};
constexpr std::array install_opts = {
    OptionDoc{.flags = "--dev"sv, .desc = "Clone the package repository's development branch"sv}};
constexpr std::array install_exs = {""sv, "Std"sv, "Std@0.1.0"sv, "--dev Std"sv, "--dev Windows"sv};

// List
constexpr std::array list_usage = {"[options]"sv};
constexpr std::array list_opts = {OptionDoc{
    .flags = "--global"sv, .desc = "List packages in the global environment cache instead of the local manifest"sv}};
constexpr std::array list_exs = {""sv, "--global"sv};

// New
constexpr std::array new_usage = {"[name] [options]"sv};
constexpr std::array new_opts = {
    OptionDoc{.flags = "--bin"sv, .desc = "Create an executable binary package application (default)"sv},
    OptionDoc{.flags = "--lib"sv, .desc = "Create a library code package"sv},
    OptionDoc{.flags = "--path <dir>"sv, .desc = "Create the workspace in a specific directory"sv}};
constexpr std::array new_exs = {"Program"sv, "Program --bin"sv};

// Remove
constexpr std::array remove_usage = {"[name]"sv};
constexpr std::array remove_exs = {"Json"sv, "Random"sv};

// Run
constexpr std::array run_usage = {"[options] [-- args...]"sv};
constexpr std::array run_opts = {OptionDoc{.flags = "--release"sv, .desc = "Build with release profile"sv}};
constexpr std::array run_exs = {""sv, "--release"sv, "-- --port 8080"sv};

// Test
constexpr std::array test_usage = {"[options]"sv};
constexpr std::array test_opts = {OptionDoc{.flags = "--release"sv, .desc = "Build with release profile"sv}};
constexpr std::array test_exs = {""sv, "--release"sv};

// Uninstall
constexpr std::array uninstall_usage = {""sv, "[package]"sv};
constexpr std::array uninstall_exs = {""sv, "Json"sv};

// Update
constexpr std::array update_usage = {"[options]"sv};
constexpr std::array update_opts = {
    OptionDoc{.flags = "--global"sv, .desc = "Update all entries in the global environment cache"sv}};
constexpr std::array update_exs = {""sv, "--global"sv};

// Version
constexpr std::array version_exs = {""sv, "rux -V"sv, "rux --version"sv};
} // namespace Data

namespace GlobalOpts {
constexpr std::array catalog = {
    OptionDoc{.flags = "--color <auto|on|off>"sv, .desc = "Control colored console output"sv},
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
               .options = Data::add_opts},

    CommandDoc{.name = "build"sv,
               .shortDesc = "Build the current package"sv,
               .description = {} /* Fallback */,
               .usage = Data::build_usage,
               .postUsage = {},
               .footer = "Artifacts are stored under [Build].Output, defaulting to Bin/Debug/ or Bin/Release/."sv,
               .examples = Data::build_exs,
               .options = Data::build_opts},

    CommandDoc{
        .name = "check"sv,
        .shortDesc = "Check package source code for errors without building"sv,
        .description =
            "Parse and analyze the source workspace files for syntactic or semantic safety flaws without emitting compilation binaries."sv,
        .usage = Data::check_usage,
        .postUsage = {},
        .footer = {},
        .examples = Data::check_exs,
        .options = Data::check_opts},

    CommandDoc{
        .name = "clean"sv,
        .shortDesc = "Remove build artifacts"sv,
        .description =
            "Purge compiled code modules, system logs, and cached local build tracking states from the environment workspace."sv,
        .usage = Data::clean_usage,
        .postUsage = "Removes the configured build output directory and Temp/ folder."sv,
        .footer = {},
        .examples = Data::clean_exs,
        .options = Data::clean_opts},

    CommandDoc{.name = "doc"sv,
               .shortDesc = "Generate package documentation"sv,
               .description = {} /* Fallback */,
               .usage = Data::doc_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::doc_exs,
               .options = Data::doc_opts},

    CommandDoc{.name = "fmt"sv,
               .shortDesc = "Format source files and manifests"sv,
               .description = "Format all *.rux source files and *.toml manifests"sv,
               .usage = Data::fmt_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::fmt_exs,
               .options = Data::fmt_opts},

    CommandDoc{.name = "help"sv,
               .shortDesc = "Show help information"sv,
               .description = {},
               .usage = {},
               .postUsage = {},
               .footer = {},
               .examples = {},
               .options = {}},

    CommandDoc{.name = "info"sv,
               .shortDesc = "Show package metadata and manifest information"sv,
               .description = "Show information about an installed Rux package"sv,
               .usage = Data::info_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::info_exs,
               .options = Data::info_opts},

    CommandDoc{.name = "init"sv,
               .shortDesc = "Initialize a Rux package in the current directory"sv,
               .description = {},
               .usage = Data::init_usage,
               .postUsage = {},
               .footer = "If Rux.toml does not exist, it will be created."sv,
               .examples = Data::init_exs,
               .options = Data::init_opts},

    CommandDoc{
        .name = "install"sv,
        .shortDesc = "Install dependencies"sv,
        .description = {} /* Fallback */,
        .usage = Data::install_usage,
        .postUsage =
            "Without a package name, downloads all registry dependencies listed in Rux.toml.\nWith a package name, adds it to Rux.toml and downloads it to the local cache."sv,
        .footer = {},
        .examples = Data::install_exs,
        .options = Data::install_opts},

    CommandDoc{.name = "list"sv,
               .shortDesc = "List dependencies"sv,
               .description = "List packages in the manifest file"sv,
               .usage = Data::list_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::list_exs,
               .options = Data::list_opts},

    CommandDoc{.name = "new"sv,
               .shortDesc = "Create a new Rux package"sv,
               .description = "Create a new Rux package in a new directory"sv,
               .usage = Data::new_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::new_exs,
               .options = Data::new_opts},

    CommandDoc{.name = "remove"sv,
               .shortDesc = "Remove a dependency from the manifest"sv,
               .description = {} /* Fallback */,
               .usage = Data::remove_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::remove_exs,
               .options = {}},

    CommandDoc{.name = "run"sv,
               .shortDesc = "Build and run the main executable"sv,
               .description = "Build and execute a runnable target"sv,
               .usage = Data::run_usage,
               .postUsage = "Arguments after '--' are forwarded to the executable."sv,
               .footer = {},
               .examples = Data::run_exs,
               .options = Data::run_opts},

    CommandDoc{.name = "test"sv,
               .shortDesc = "Run all test targets"sv,
               .description = "Run package unit tests"sv,
               .usage = Data::test_usage,
               .postUsage = {},
               .footer = {},
               .examples = Data::test_exs,
               .options = Data::test_opts},

    CommandDoc{.name = "uninstall"sv,
               .shortDesc = "Uninstall dependencies"sv,
               .description = "Uninstall dependencies from the local cache"sv,
               .usage = Data::uninstall_usage,
               .postUsage = "Without a package name, removes all registry dependencies "
                            "listed in Rux.toml from the local "
                            "cache.\nWith a package name, removes only that package."sv,
               .footer = {},
               .examples = Data::uninstall_exs,
               .options = {}},

    CommandDoc{
        .name = "update"sv,
        .shortDesc = "Update dependencies"sv,
        .description = {} /* Fallback */,
        .usage = Data::update_usage,
        .postUsage =
            "Without --global, checks all registry dependencies listed in Rux.toml and pulls the latest changes. Missing packages are cloned from the registry.\nWith --global, updates every package present in the local cache."sv,
        .footer = {},
        .examples = Data::update_exs,
        .options = Data::update_opts},

    CommandDoc{.name = "version"sv,
               .shortDesc = "Show version information"sv,
               .description = "Show information about the Rux toolchain version"sv,
               .usage = {},
               .postUsage = {},
               .footer = {},
               .examples = Data::version_exs,
               .options = {}}};

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

constexpr auto G_CMD_WIDTH = MaxSize(G_COMMAND_HELP_MAPS, &CommandDoc::name);
constexpr auto G_OPT_WIDTH = MaxSize(GlobalOpts::catalog, &OptionDoc::flags);
} // namespace

void Cli::PrintHelp() {
    const size_t termWidth = GetTerminalWidth();
    std::println("Rux compiler and package manager\n");
    std::println("Usage: {} [command] [options] [-- args...]\n", Layout::CliName);
    std::println("Commands:");
    for (const auto &cmd : G_COMMAND_HELP_MAPS) {
        PrintAligned(cmd.name, cmd.shortDesc, G_CMD_WIDTH, termWidth);
    }
    std::println("\nOptions:");
    for (const auto &[flags, desc] : GlobalOpts::catalog) {
        PrintAligned(flags, desc, G_OPT_WIDTH, termWidth);
    }
    std::println("\nUse 'rux help <command>' for more information about a command.");
}

void Cli::PrintHelpFor(const std::string_view command) {
    if (command == "help") {
        PrintHelp();
        return;
    }
    const auto it = std::ranges::lower_bound(G_COMMAND_HELP_MAPS, command, std::less<>{}, &CommandDoc::name);
    if (it == G_COMMAND_HELP_MAPS.end() || it->name != command) {
        PrintUnknownCommand(command);
        return;
    }
    const auto &[name, shortDesc, description, usage, postUsage, footer, examples, options] = *it;
    const size_t termWidth = GetTerminalWidth();
    const std::string_view longOrShortDescription = !description.empty() ? description : shortDesc;
    Wrap(longOrShortDescription, termWidth, [&](std::string_view line) -> void { std::println("{}", line); });
    std::println("");
    std::println("Usage:");
    if (usage.empty()) {
        PrintCmdLine(name, ""sv);
    }
    else {
        for (const std::string_view usageVariant : usage) {
            PrintCmdLine(name, usageVariant);
        }
    }
    std::println("");
    PrintBlock(""sv, postUsage, termWidth);
    if (!options.empty()) {
        const auto optWidth = MaxSize(options, &OptionDoc::flags);
        std::println("Options:");
        for (const auto &[flags, desc] : options) {
            PrintAligned(flags, desc, optWidth, termWidth);
        }
        std::println("");
    }
    PrintBlock(""sv, footer, termWidth, 0);
    if (!examples.empty()) {
        std::println("Examples:");
        for (const auto example : examples) {
            PrintCmdLine(name, example);
        }
        std::println("");
    }
}

void Cli::PrintVersion() {
    std::println("Rux {} ({} {})", RUX_VERSION, RUX_BUILD_DATE, RUX_BUILD_TIME);
}

void Cli::PrintUnknownCommand(std::string_view command) {
    std::println(stderr, "error: unknown command '{}'", command);
    std::println(stderr, "\nUse 'rux help' for a list of available commands.");
}

void Cli::PrintUnknownOption(std::string_view option, std::string_view command) {
    if (command.empty()) {
        std::println(stderr, "error: unknown option '{}'", option);
        std::println(stderr, "\nUse 'rux help' for a list of global options.");
    }
    else {
        std::println(stderr, "error: unknown option '{}' for command '{}'", option, command);
        std::println(stderr, "\nUse 'rux help {}' for more information about this command.", command);
    }
}

Cli::Cli(const int argc, char *argv[])
    : args(argv, argc) {
}

// Parse global options from the command line arguments.
GlobalOptions Cli::ParseGlobalOptions(std::span<const std::string_view> args) {
    GlobalOptions opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-q" || arg == "--quiet") {
            opts.quiet = true;
            continue;
        }
        if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
            continue;
        }
        if (arg == "--color") {
            if (i + 1 < args.size()) {
                if (const std::string_view val = args[++i]; val == "on") {
                    opts.color = ColorMode::On;
                }
                else if (val == "off") {
                    opts.color = ColorMode::Off;
                }
                else {
                    opts.color = ColorMode::Auto;
                }
            }
            continue;
        }
        if (arg.starts_with("--color=")) {
            if (const std::string_view val = arg.substr(8); val == "on") {
                opts.color = ColorMode::On;
            }
            else if (val == "off") {
                opts.color = ColorMode::Off;
            }
            else {
                opts.color = ColorMode::Auto;
            }
            continue;
        }
        if (arg == "--manifest") {
            if (i + 1 < args.size()) {
                opts.manifest = args[++i];
            }
            continue;
        }
    }
    return opts;
}

int Cli::Run() const {
    // Collect all arguments as string_views (skip argv[0])
    std::vector<std::string_view> sv;
    sv.reserve(static_cast<std::size_t>(args.size()));
    for (auto *a : args.subspan(1)) {
        sv.emplace_back(a);
    }
    if (sv.empty()) {
        PrintHelp();
        return 0;
    }
    // Walk through arguments collecting global flags and finding the command.
    // Global flags may appear before OR after the command name.
    std::string_view command;
    bool foundCommand = false;
    std::vector<std::string_view> preCommandGlobals;
    std::vector<std::string_view> cmdArgs;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        std::string_view arg = sv[i];
        if (!foundCommand) {
            if (arg == "-h" || arg == "--help") {
                PrintHelp();
                return 0;
            }
            if (arg == "-V" || arg == "--version") {
                PrintVersion();
                return 0;
            }
            if (arg == "-q" || arg == "--quiet" || arg == "-v" || arg == "--verbose") {
                preCommandGlobals.push_back(arg);
                continue;
            }
            if (arg == "--color") {
                preCommandGlobals.push_back(arg);
                if (i + 1 < sv.size()) {
                    preCommandGlobals.push_back(sv[++i]);
                }
                continue;
            }
            if (arg.starts_with("--color=")) {
                preCommandGlobals.push_back(arg);
                continue;
            }
            if (arg == "--manifest") {
                preCommandGlobals.push_back(arg);
                if (i + 1 < sv.size()) {
                    preCommandGlobals.push_back(sv[++i]);
                }
                continue;
            }
            command = arg;
            foundCommand = true;
        }
        else {
            cmdArgs.push_back(arg);
        }
    }
    if (!foundCommand) {
        PrintHelp();
        return 0;
    }
    // Merge pre-command globals with command args for option parsing
    std::vector<std::string_view> allArgs;
    allArgs.insert(allArgs.end(), preCommandGlobals.begin(), preCommandGlobals.end());
    allArgs.insert(allArgs.end(), cmdArgs.begin(), cmdArgs.end());
    GlobalOptions opts = ParseGlobalOptions(allArgs);
    // Filter global options out of cmdArgs so command handlers never see them.
    std::vector<std::string_view> filteredCmdArgs;
    for (std::size_t i = 0; i < cmdArgs.size(); ++i) {
        const std::string_view arg = cmdArgs[i];
        if (arg == "-q" || arg == "--quiet" || arg == "-v" || arg == "--verbose") {
            continue;
        }
        if (arg == "--color") {
            ++i; // skip value
            continue;
        }
        if (arg.starts_with("--color=")) {
            continue;
        }
        if (arg == "--manifest") {
            ++i; // skip value
            continue;
        }
        filteredCmdArgs.push_back(arg);
    }
    std::span<const std::string_view> rest(filteredCmdArgs);
    if (command == "help") {
        return RunHelp(rest, opts);
    }
    if (command == "version") {
        return RunVersion(opts);
    }
    if (command == "build") {
        return RunBuild(rest, opts);
    }
    if (command == "clean") {
        return RunClean(rest, opts);
    }
    if (command == "doc") {
        return RunDoc(rest, opts);
    }
    if (command == "fmt") {
        return RunFmt(rest, opts);
    }
    if (command == "init") {
        return RunInit(rest, opts);
    }
    if (command == "install") {
        return RunInstall(rest, opts);
    }
    if (command == "uninstall") {
        return RunUninstall(rest, opts);
    }
    if (command == "list") {
        return RunList(rest, opts);
    }
    if (command == "new") {
        return RunNew(rest, opts);
    }
    if (command == "add") {
        return RunAdd(rest, opts);
    }
    if (command == "remove") {
        return RunRemove(rest, opts);
    }
    if (command == "run") {
        return RunRun(rest, opts);
    }
    if (command == "test") {
        return RunTest(rest, opts);
    }
    if (command == "update") {
        return RunUpdate(rest, opts);
    }
    if (command == "info") {
        return RunInfo(rest, opts);
    }
    if (command == "check") {
        return RunCheck(rest, opts);
    }
    PrintUnknownCommand(command);
    return 1;
}

int Cli::RunBuild(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const auto t0 = std::chrono::steady_clock::now();
    bool isRelease = false;
    bool isDebug = false;
    std::string_view profile;
    std::string_view target;
    bool dumpTokens = false;
    bool dumpAst = false;
    bool dumpSema = false;
    bool dumpHir = false;
    bool dumpLir = false;
    bool dumpAsm = false;
    bool dumpRcu = false;
    bool showStats = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "--debug") {
            isDebug = true;
            continue;
        }
        if (arg == "-q" || arg == "--quiet") {
            continue;
        }
        if (arg == "-v" || arg == "--verbose") {
            continue;
        }
        if (arg == "--stats") {
            showStats = true;
            continue;
        }
        if (arg == "--dump-tokens") {
            dumpTokens = true;
            continue;
        }
        if (arg == "--dump-ast") {
            dumpAst = true;
            continue;
        }
        if (arg == "--dump-sema") {
            dumpSema = true;
            continue;
        }
        if (arg == "--dump-hir") {
            dumpHir = true;
            continue;
        }
        if (arg == "--dump-lir") {
            dumpLir = true;
            continue;
        }
        if (arg == "--dump-asm") {
            dumpAsm = true;
            continue;
        }
        if (arg == "--dump-rcu") {
            dumpRcu = true;
            continue;
        }
        if (arg == "--profile" && i + 1 < args.size()) {
            profile = args[++i];
            continue;
        }
        if (arg == "--target" && i + 1 < args.size()) {
            target = args[++i];
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("build");
            return 0;
        }
        PrintUnknownOption(arg, "build");
        return 1;
    }
    (void)isDebug; // Stop -Wunused-but-set-variable
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::string targetName = target.empty() ? HostTargetTriple() : std::string(target);
    if (!IsSupportedTargetTriple(targetName)) {
        std::print(stderr,
                   "error: unsupported target '{}'; supported targets are "
                   "linux-x64, windows-x64, macos-x64, macos-arm64, "
                   "freebsd-x64, openbsd-x64, netbsd-x64, illumos-x64\n",
                   targetName);
        return 1;
    }
    const std::string hostTarget = HostTargetTriple();
    if (hostTarget != "unknown" && targetName != hostTarget) {
        // Target selection is currently used for source/dependency choice.
        // Linking foreign executable formats is kept explicit until the
        // backends support it end-to-end.
        std::print(stderr,
                   "error: cross-target build from '{}' to '{}' is not "
                   "supported yet\n",
                   hostTarget, targetName);
        return 1;
    }
    std::string_view profileName = isRelease ? "Release" : "Debug";
    if (!profile.empty()) {
        profileName = profile;
    }
    if (!opts.quiet && !showStats) {
        std::print("Compiling {} v{} [{}]\n", manifest->package.name, manifest->package.version,
                   manifestPath->parent_path().string());
    }
    Misc::BuildStats stats;
    auto loadResult = SourceLoader::Load(manifestPath->parent_path());
    if (!loadResult) {
        return 1;
    }
    stats.localFiles = loadResult->files.size();
    for (const auto &file : loadResult->files) {
        stats.localLines += CountLines(file.source);
        stats.localSourceSize += file.source.size();
    }
    for (const auto &err : loadResult->errors) {
        std::print(stderr, "{}", err);
    }
    bool lexErrors = false;
    std::vector<LexerResult> lexResults;
    lexResults.reserve(loadResult->files.size());
    const auto localLexingStart = std::chrono::steady_clock::now();
    for (const auto &file : loadResult->files) {
        if (opts.verbose) {
            std::print("     Lexing {}\n", file.path.string());
        }
        Lexer lexer(file.source, file.path.string());
        auto lexResult = lexer.Tokenize();
        stats.localTokens += CountTokens(lexResult);
        PrintDiagnostics(lexResult.diagnostics);
        if (lexResult.HasErrors()) {
            lexErrors = true;
        }
        if (dumpTokens) {
            auto tempDir = manifestPath->parent_path() / "Temp" / "Tokens";
            std::filesystem::create_directories(tempDir);
            auto rel = std::filesystem::relative(file.path, manifestPath->parent_path() / "Src");
            auto tokPath = tempDir / rel;
            tokPath.replace_extension(".tokens");
            Lexer::DumpTokens(lexResult, tokPath);
        }
        lexResults.push_back(std::move(lexResult));
    }
    const auto localLexingEnd = std::chrono::steady_clock::now();
    stats.lexing += ElapsedMs(localLexingStart, localLexingEnd);
    if (lexErrors) {
        return 1;
    }
    // Parse
    bool parseErrors = false;
    std::vector<ParseResult> parseResults;
    parseResults.reserve(loadResult->files.size());
    const auto localParsingStart = std::chrono::steady_clock::now();
    for (std::size_t fileIndex = 0; fileIndex < loadResult->files.size(); ++fileIndex) {
        const auto &file = loadResult->files[fileIndex];
        if (opts.verbose) {
            std::print("    Parsing {}\n", file.path.string());
        }
        auto &lexResult = lexResults[fileIndex];
        if (lexResult.HasErrors()) {
            continue;
        }
        Parser parser(std::move(lexResult.tokens), file.path.string());
        auto parseResult = parser.Parse();
        PrintDiagnostics(parseResult.diagnostics);
        if (parseResult.HasErrors()) {
            parseErrors = true;
            continue;
        }
        PruneModuleForTarget(parseResult.module, targetName);
        if (dumpAst) {
            auto tempDir = manifestPath->parent_path() / "Temp" / "Ast";
            std::filesystem::create_directories(tempDir);
            auto rel = std::filesystem::relative(file.path, manifestPath->parent_path() / "Src");
            auto astPath = (tempDir / rel).replace_extension(".ast");
            Parser::DumpAst(parseResult, astPath);
        }
        parseResults.push_back(std::move(parseResult));
    }
    stats.parsing += ElapsedMs(localParsingStart);
    if (parseErrors) {
        return 1;
    }

    // Load dependency packages referenced by import declarations
    std::vector<ParseResult> depParseResults;
    std::vector<std::string> loadedPackages;    // parallel: package name per entry
    std::vector<std::string> loadedModuleNames; // parallel: source name per entry
    {
        struct PendingPackage {
            std::string name;
            std::filesystem::path root;
            Manifest manifest;
        };

        std::vector<PendingPackage> pendingPackages;
        std::unordered_set<std::string> queuedPackageNames;
        auto enqueueDependency = [&](const std::string &pkgName, const Manifest &ownerManifest,
                                     const std::filesystem::path &ownerRoot) -> bool {
            if (queuedPackageNames.count(pkgName)) {
                return true;
            }
            // Resolve imports through the target view of the owning
            // manifest. This is what maps Platform to Linux on linux-x64.
            const auto deps = ownerManifest.EffectiveDependencies(targetName);
            const Dependency *dep = nullptr;
            for (const auto &d : deps) {
                if (d.name == pkgName) {
                    dep = &d;
                    break;
                }
            }
            if (!dep) {
                std::print(stderr, "error: package '{}' is not listed in [Dependencies]\n", pkgName);
                return false;
            }
            std::filesystem::path depRoot;
            if (dep->path.empty()) {
                depRoot = RegistryPackagesDir() / DependencyPackageName(*dep);
                if (!std::filesystem::exists(depRoot)) {
                    std::print(stderr,
                               "error: package '{}' is not installed — run "
                               "'rux install'\n",
                               DependencyPackageName(*dep));
                    return false;
                }
            }
            else {
                depRoot = (ownerRoot / dep->path).lexically_normal();
            }
            auto depManifest = Manifest::Load(depRoot / "Rux.toml");
            if (!depManifest) {
                std::print(stderr, "error: dependency package '{}' was not found at '{}'\n", pkgName, depRoot.string());
                return false;
            }
            queuedPackageNames.insert(pkgName);
            // Keep the import name as the package namespace loaded into
            // Sema, even when the files came from another package name.
            pendingPackages.push_back({dep->name, depRoot, std::move(*depManifest)});
            return true;
        };
        std::vector<std::string> imports;
        auto collectImports = [&](this auto &&self, const Decl &decl) -> void {
            if (const auto *ud = dynamic_cast<const UseDecl *>(&decl)) {
                if (!DeclMatchesTarget(*ud, targetName)) {
                    return;
                }
                if (!ud->path.empty()) {
                    imports.push_back(ud->path[0]);
                }
                return;
            }
            if (const auto *mod = dynamic_cast<const ModuleDecl *>(&decl)) {
                for (const auto &item : mod->items) {
                    if (item) {
                        self(*item);
                    }
                }
            }
        };
        for (const auto &pr : parseResults) {
            imports.clear();
            for (const auto &decl : pr.module.items) {
                if (decl) {
                    collectImports(*decl);
                }
            }
            for (const auto &pkgName : imports) {
                if (pkgName == manifest->package.name) {
                    continue;
                }
                if (!enqueueDependency(pkgName, *manifest, manifestPath->parent_path())) {
                    return 1;
                }
            }
        }
        for (std::size_t pendingIndex = 0; pendingIndex < pendingPackages.size(); ++pendingIndex) {
            const std::filesystem::path pendingRoot = pendingPackages[pendingIndex].root;
            const Manifest pendingManifest = pendingPackages[pendingIndex].manifest;
            const std::string packageName = pendingPackages[pendingIndex].name;
            if (opts.verbose) {
                std::print("  Loading package {} from {}\n", packageName, pendingRoot.string());
            }
            auto depLoadResult = SourceLoader::Load(pendingRoot);
            if (!depLoadResult) {
                return 1;
            }
            stats.dependencyFiles += depLoadResult->files.size();
            for (const auto &depFile : depLoadResult->files) {
                stats.dependencyLines += Rux::Misc::CountLines(depFile.source);
                stats.dependencySourceSize += depFile.source.size();
            }
            for (const auto &error : depLoadResult->errors) {
                std::print(stderr, "{}", error);
            }
            if (!depLoadResult->errors.empty()) {
                return 1;
            }
            std::vector<ParseResult> packageParseResults;
            packageParseResults.reserve(depLoadResult->files.size());
            for (const auto &depFile : depLoadResult->files) {
                const auto depLexingStart = std::chrono::steady_clock::now();
                Lexer depLexer(depFile.source, depFile.path.string());
                auto depLex = depLexer.Tokenize();
                const auto depLexingEnd = std::chrono::steady_clock::now();
                stats.lexing += ElapsedMs(depLexingStart, depLexingEnd);
                stats.dependencyTokens += CountTokens(depLex);
                PrintDiagnostics(depLex.diagnostics);
                if (depLex.HasErrors()) {
                    return 1;
                }
                const auto depParsingStart = std::chrono::steady_clock::now();
                Parser depParser(std::move(depLex.tokens), depFile.path.string());
                auto depParse = depParser.Parse();
                stats.parsing += ElapsedMs(depParsingStart);
                PrintDiagnostics(depParse.diagnostics);
                if (depParse.HasErrors()) {
                    return 1;
                }
                PruneModuleForTarget(depParse.module, targetName);
                packageParseResults.push_back(std::move(depParse));
            }
            imports.clear();
            for (const auto &pr : packageParseResults) {
                for (const auto &decl : pr.module.items) {
                    if (decl) {
                        collectImports(*decl);
                    }
                }
            }
            for (const auto &pkgName : imports) {
                if (pkgName == pendingManifest.package.name || pkgName == packageName) {
                    continue;
                }
                if (!enqueueDependency(pkgName, pendingManifest, pendingRoot)) {
                    return 1;
                }
            }
            for (auto &depParse : packageParseResults) {
                loadedModuleNames.push_back(depParse.module.name);
                depParseResults.push_back(std::move(depParse));
                loadedPackages.push_back(packageName);
            }
        }
    }

    // Semantic analysis
    const auto semanticStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("  Analyzing {}\n", manifest->package.name);
    }
    std::vector<const Module *> userModules;
    userModules.reserve(parseResults.size());
    for (const auto &pr : parseResults) {
        userModules.push_back(&pr.module);
    }
    // Build per-package dep info so Sema can isolate imported package symbols.
    std::vector<DepPackage> depPackages;
    {
        std::unordered_map<std::string, std::size_t> pkgIdx;
        for (std::size_t i = 0; i < depParseResults.size(); ++i) {
            const std::string &pkgName = loadedPackages[i];
            auto [it, inserted] = pkgIdx.emplace(pkgName, depPackages.size());
            if (inserted) {
                depPackages.push_back({pkgName, {}});
            }
            depPackages[it->second].modules.push_back({loadedModuleNames[i], &depParseResults[i].module});
        }
    }
    Sema sema(std::move(userModules), std::move(depPackages), manifest->package.name,
              std::string(TargetOsName(targetName)));
    auto semaResult = sema.Analyze();
    PrintDiagnostics(semaResult.diagnostics);
    if (dumpSema) {
        auto semaDir = manifestPath->parent_path() / "Temp" / "Sema";
        std::filesystem::create_directories(semaDir);
        Sema::DumpResult(semaResult, semaDir / "sema.txt");
    }
    if (semaResult.HasErrors()) {
        return 1;
    }
    stats.semantic = ElapsedMs(semanticStart);

    // HIR
    const auto hirStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("  Lowering {}\n", manifest->package.name);
    }
    std::vector<const Module *> hirModules;
    hirModules.reserve(depParseResults.size() + parseResults.size());
    for (const auto &pr : depParseResults) {
        hirModules.push_back(&pr.module);
    }
    for (const auto &pr : parseResults) {
        hirModules.push_back(&pr.module);
    }
    Hir hir(hirModules);
    auto hirPackage = hir.Generate();
    if (dumpHir) {
        auto hirDir = manifestPath->parent_path() / "Temp" / "Hir";
        std::filesystem::create_directories(hirDir);
        Hir::Dump(hirPackage, hirDir / "hir.txt");
    }
    stats.hir = ElapsedMs(hirStart);

    // LIR
    const auto lirStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("  Emitting LIR for {}\n", manifest->package.name);
    }
    Lir lir(std::move(hirPackage));
    auto lirPackage = lir.Generate();
    if (dumpLir) {
        auto lirDir = manifestPath->parent_path() / "Temp" / "Lir";
        std::filesystem::create_directories(lirDir);
        Lir::Dump(lirPackage, lirDir / "lir.txt");
    }
    stats.lir = ElapsedMs(lirStart);

    // Assembly dump (optional)
    const auto codegenStart = std::chrono::steady_clock::now();
    if (dumpAsm) {
        if (opts.verbose) {
            std::print("  Emitting assembly for {}\n", manifest->package.name);
        }
        auto asmDir = manifestPath->parent_path() / "Temp" / "Asm";
        std::filesystem::create_directories(asmDir);
        Asm::Emit(lirPackage, asmDir / "out.asm");
    }

    // RCU object generation
    if (opts.verbose) {
        std::print("  Emitting RCU objects for {}\n", manifest->package.name);
    }
    Rcu rcu(lirPackage, std::string(manifest->package.name));
    auto rcuFiles = rcu.Generate();
    if (dumpRcu) {
        auto objDir = manifestPath->parent_path() / "Temp" / "Obj";
        auto dumpDir = manifestPath->parent_path() / "Temp" / "Rcu";
        std::filesystem::create_directories(objDir);
        std::filesystem::create_directories(dumpDir);

        for (const auto &rcuFile : rcuFiles) {
            std::filesystem::path stem = rcuFile.sourcePath.empty() ? std::filesystem::path("out")
                                                                    : std::filesystem::path(rcuFile.sourcePath).stem();
            Rcu::Emit(rcuFile, objDir / (stem.string() + ".rcu"));
            Rcu::Dump(rcuFile, dumpDir / (stem.string() + ".rcu.txt"));
        }
    }
    stats.codegen = ElapsedMs(codegenStart);

    // Link
    const auto linkingStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("   Linking {}\n", manifest->package.name);
    }
    const auto root = manifestPath->parent_path();
    const auto binDir = ResolveBuildOutputDir(root, *manifest, profileName);
    const bool buildDll = (manifest->package.type == "Dll" || manifest->package.type == "dll");
    std::string outputName = manifest->package.name;
    if constexpr (HostOS == OS::Windows) {
        outputName += buildDll ? ".dll" : ".exe";
    }
    const auto exePath = binDir / outputName;
    Linker linker(std::move(rcuFiles), std::string(manifest->package.name), {root}, buildDll);
    if (!linker.Link(exePath)) {
        for (const auto &err : linker.Errors()) {
            std::print(stderr, "error: {}\n", err.message);
        }
        return 1;
    }
    stats.linking = ElapsedMs(linkingStart);

    // Done
    const auto buildEnd = std::chrono::steady_clock::now();
    stats.total = ElapsedMs(t0, buildEnd);
    stats.totalSeconds = ElapsedSeconds(t0, buildEnd);
    std::error_code sizeError;
    stats.executableSize = std::filesystem::file_size(exePath, sizeError);
    if (sizeError) {
        stats.executableSize = 0;
    }
    stats.peakMemoryBytes = PeakMemoryBytes();
    if (!opts.quiet && showStats) {
        PrintBuildStats(exePath, profileName, stats);
        return 0;
    }
    if (!opts.quiet) {
        PrintBuildSummary(exePath, profileName, stats);
    }
    return 0;
}

int Cli::RunClean(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool tempOnly = false;
    for (auto &arg : args) {
        if (arg == "--temp") {
            tempOnly = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("clean");
            return 0;
        }
        PrintUnknownOption(arg, "clean");
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
    const auto root = manifestPath->parent_path();
    const auto outputDir = manifest->build.output.empty() ? root / "Bin"
                                                          : (std::filesystem::path(manifest->build.output).is_relative()
                                                                 ? root / manifest->build.output
                                                                 : std::filesystem::path(manifest->build.output));
    auto removeDir = [&](const std::filesystem::path &dir) -> bool {
        std::error_code ec;
        if (!std::filesystem::exists(dir)) {
            return true;
        }
        std::filesystem::remove_all(dir, ec);
        if (ec) {
            std::print(stderr, "error: failed to remove '{}': {}\n", dir.string(), ec.message());
            return false;
        }
        if (!opts.quiet) {
            std::print("     Removed {}\n", dir.string());
        }
        return true;
    };
    bool ok = true;
    if (!tempOnly) {
        ok &= removeDir(outputDir);
    }
    ok &= removeDir(root / "Temp");
    return ok ? 0 : 1;
}

int Cli::RunRun(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool isRelease = false;
    std::vector<std::string_view> runArgs;
    bool passThroughMode = false;
    for (auto arg : args) {
        if (passThroughMode) {
            runArgs.push_back(arg);
            continue;
        }
        if (arg == "--") {
            passThroughMode = true;
            continue;
        }
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("run");
            return 0;
        }
        PrintUnknownOption(arg, "run");
        return 1;
    }
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    // Build first
    GlobalOptions buildOpts = opts;
    if (!opts.verbose) {
        buildOpts.quiet = true;
    }

    std::vector<std::string_view> buildArgs;
    if (isRelease) {
        buildArgs.emplace_back("--release");
    }
    if (buildOpts.quiet) {
        buildArgs.emplace_back("--quiet");
    }
    if (buildOpts.verbose) {
        buildArgs.emplace_back("--verbose");
    }
    int rc = RunBuild(buildArgs, buildOpts);
    if (rc != 0) {
        return rc;
    }
    std::string_view profileName = isRelease ? "Release" : "Debug";
    auto root = manifestPath->parent_path();
    auto binDir = ResolveBuildOutputDir(root, *manifest, profileName);
    const bool runDll = (manifest->package.type == "Dll" || manifest->package.type == "dll");
    if (runDll) {
        std::print(stderr, "error: cannot run a DLL package directly\n");
        return 1;
    }
    std::string exeName = manifest->package.name;

    if constexpr (HostOS == OS::Windows) {
        exeName.append(".exe");
    }

    auto exePath = binDir / exeName;
    if (!std::filesystem::exists(exePath)) {
        std::print(stderr, "error: executable not found: '{}'\n", exePath.string());
        return 1;
    }
    if (opts.verbose && !opts.quiet) {
        std::print("     Running `{}`\n", exePath.string());
    }
#if RUX_OS_WINDOWS
    std::string cmdLine = "\"" + exePath.string() + "\"";
    for (const auto &a : runArgs) {
        cmdLine += " \"";
        cmdLine += std::string(a);
        cmdLine += '"';
    }
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        std::print(stderr, "error: failed to launch '{}' (code {})\n", exePath.string(), GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
#else
    std::vector<std::string> argStrings;
    argStrings.push_back(exePath.string());
    for (const auto &a : runArgs) {
        argStrings.emplace_back(a);
    }

    std::vector<char *> argv;
    for (auto &s : argStrings) {
        argv.push_back(s.data());
    }

    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        std::print(stderr, "error: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        execv(exePath.c_str(), argv.data());
        std::print(stderr, "error: failed to launch '{}'\n", exePath.string());
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

struct PendingPackage {
    std::string name;
    std::filesystem::path root;
    Manifest manifest;
};

struct ImportCollector {
    std::vector<std::string> &imports;
    std::string_view target;

    void collect(const Decl &decl) {
        if (const auto *ud = dynamic_cast<const UseDecl *>(&decl)) {
            if (!DeclMatchesTarget(*ud, target)) {
                return;
            }
            if (!ud->path.empty()) {
                imports.push_back(ud->path[0]);
            }
            return;
        }
        if (const auto *mod = dynamic_cast<const ModuleDecl *>(&decl)) {
            for (const auto &item : mod->items) {
                if (item) {
                    collect(*item);
                }
            }
        }
    }
};

struct DependencyQueue {
    const std::string pkgName;
    const Manifest ownerManifest;
    const std::filesystem::path ownerRoot;
    std::unordered_set<std::string> queuedPackageNames;
    std::vector<PendingPackage> pendingPackages;
    std::string targetName;
};

auto enqueueDependency(DependencyQueue &queue, const std::function<void(Diagnostic)> &EmitDiag) -> bool {
    if (queue.queuedPackageNames.count(queue.pkgName)) {
        return true;
    }

    const auto deps = queue.ownerManifest.EffectiveDependencies(queue.targetName);
    std::optional<Rux::Dependency> targetDep; // Add Rux:: here
    for (const auto &d : deps) {
        if (d.name == queue.pkgName) {
            targetDep = d;
            break;
        }
    }
    if (!targetDep) {
        EmitDiag(ErrorDiagnostic("package '" + queue.pkgName + "' is not listed in [Dependencies]"));
        return false;
    }
    std::filesystem::path depRoot;
    if (targetDep->path.empty()) {
        depRoot = RegistryPackagesDir() / DependencyPackageName(*targetDep);
        if (!std::filesystem::exists(depRoot)) {
            EmitDiag(ErrorDiagnostic("package '" + DependencyPackageName(*targetDep) +
                                     "' is not installed — run 'rux install'"));
            return false;
        }
    }
    else {
        depRoot = (queue.ownerRoot / targetDep->path).lexically_normal();

        auto rel = depRoot.lexically_relative(queue.ownerRoot);
        if (!rel.empty() && rel.begin()->string() == "..") {
            EmitDiag(ErrorDiagnostic("package '" + queue.pkgName + "' contains an invalid path escaping root bounds"));
            return false;
        }
    }

    auto depManifest = Manifest::Load(depRoot / "Rux.toml");
    if (!depManifest) {
        EmitDiag(
            ErrorDiagnostic("dependency package '" + queue.pkgName + "' was not found at '" + depRoot.string() + "'"));
        return false;
    }

    queue.queuedPackageNames.insert(queue.pkgName);
    queue.pendingPackages.push_back({targetDep->name, depRoot, std::move(*depManifest)});
    return true;
}

void HandleErrors(bool &hadErrors, const std::vector<ParseResult> &parseResults,
                  const std::vector<ParseResult> &depParseResults, const std::vector<std::string> &loadedPackages,
                  const std::vector<std::string> &loadedModuleNames, const Manifest &manifest,
                  const std::string &targetName, const std::function<void(Diagnostic)> &EmitDiag) {
    std::vector<const Module *> userModules;
    userModules.reserve(parseResults.size());
    for (const auto &pr : parseResults) {
        userModules.push_back(&pr.module);
    }
    std::vector<DepPackage> depPackages;
    std::unordered_map<std::string, std::size_t> pkgIdx;
    for (std::size_t i = 0; i < depParseResults.size(); ++i) {
        const std::string &pkgName = loadedPackages[i];
        auto [it, inserted] = pkgIdx.emplace(pkgName, depPackages.size());
        if (inserted) {
            depPackages.push_back({pkgName, {}});
        }
        depPackages[it->second].modules.push_back({loadedModuleNames[i], &depParseResults[i].module});
    }
    Sema sema(std::move(userModules), std::move(depPackages), manifest.package.name,
              std::string(TargetOsName(targetName)));
    auto semaResult = sema.Analyze();
    for (const auto &diag : semaResult.diagnostics) {
        EmitDiag(diag);
        if (diag.IsError()) {
            hadErrors = true;
        }
    }
    if (semaResult.HasErrors()) {
        hadErrors = true;
    }
}

int HandlePendingIndex(bool &hadErrors, const GlobalOptions &opts, bool jsonOutput,
                       std::vector<PendingPackage> &pendingPackages, const std::string &targetName,
                       std::vector<std::string> &imports, ImportCollector &collector,
                       std::vector<ParseResult> &depParseResults, std::vector<std::string> &loadedPackages,
                       std::vector<std::string> &loadedModuleNames, std::unordered_set<std::string> &queuedPackageNames,
                       const std::function<void(Diagnostic)> &EmitDiag) {
    for (std::size_t pendingIndex = 0; pendingIndex < pendingPackages.size(); ++pendingIndex) {
        const auto &pendingPkg = pendingPackages[pendingIndex];
        if (opts.verbose && !jsonOutput) {
            std::print(" Loading package {} from {}\n", pendingPkg.name, pendingPkg.root.string());
        }
        auto depLoadResult = SourceLoader::Load(pendingPkg.root);
        if (!depLoadResult) {
            hadErrors = true;
            break;
        };
        for (const auto &error : depLoadResult->errors) {
            if (jsonOutput) {
                EmitDiag(ErrorDiagnostic(error));
                hadErrors = true;
            }
            else {
                std::print(stderr, "{}", error);
            }
        }
        if (!depLoadResult->errors.empty()) {
            hadErrors = true;
            break;
        }
        std::vector<ParseResult> packageParseResults;
        packageParseResults.reserve(depLoadResult->files.size());
        for (const auto &depFile : depLoadResult->files) {
            Lexer depLexer(depFile.source, depFile.path.string());
            auto depLex = depLexer.Tokenize();

            for (const auto &diag : depLex.diagnostics) {
                EmitDiag(diag);
                if (diag.IsError()) {
                    hadErrors = true;
                }
            }
            if (depLex.HasErrors()) {
                hadErrors = true;
                break;
            }
            Parser depParser(std::move(depLex.tokens), depFile.path.string());
            auto depParse = depParser.Parse();
            for (const auto &diag : depParse.diagnostics) {
                EmitDiag(diag);
                if (diag.IsError()) {
                    hadErrors = true;
                }
            }
            if (depParse.HasErrors()) {
                hadErrors = true;
                break;
            }
            PruneModuleForTarget(depParse.module, targetName);
            packageParseResults.push_back(std::move(depParse));
        }
        if (hadErrors) {
            break;
        }
        imports.clear();
        for (const auto &pr : packageParseResults) {
            for (const auto &decl : pr.module.items) {
                if (decl) {
                    collector.collect(*decl);
                }
            }
        }
        for (const auto &pkgName : imports) {
            const auto &currentPkg = pendingPackages[pendingIndex];
            if (pkgName == currentPkg.manifest.package.name || pkgName == currentPkg.name) {
                continue;
            }
            DependencyQueue depQueue{.pkgName = pkgName,
                                     .ownerManifest = currentPkg.manifest,
                                     .ownerRoot = currentPkg.root,
                                     .queuedPackageNames = queuedPackageNames,
                                     .pendingPackages = pendingPackages,
                                     .targetName = targetName};
            if (!enqueueDependency(depQueue, EmitDiag)) {
                hadErrors = true;
                break;
            }
        }
        if (hadErrors) {
            break;
        }
        for (auto &depParse : packageParseResults) {
            loadedModuleNames.push_back(depParse.module.name);
            depParseResults.push_back(std::move(depParse));
            loadedPackages.push_back(pendingPackages[pendingIndex].name);
        }
    }
    return 0;
}

int Cli::RunCheck(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool jsonOutput = false;
    std::string_view target;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-q" || arg == "--quiet") {
            continue;
        }
        if (arg == "-v" || arg == "--verbose") {
            continue;
        }
        if (arg == "--json") {
            jsonOutput = true;
            continue;
        }
        if (arg == "--target" && i + 1 < args.size()) {
            target = args[++i];
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("check");
            return 0;
        }
        PrintUnknownOption(arg, "check");
        return 1;
    }
    std::vector<Diagnostic> jsonDiags;
    bool hadErrors = false;
    auto EmitDiag = [&](Diagnostic diag) {
        if (jsonOutput) {
            jsonDiags.push_back(std::move(diag));
        }
        else {
            PrintDiagnostic(diag);
        }
    };
    auto EmitFatal = [&](std::string message) {
        EmitDiag(ErrorDiagnostic(std::move(message)));
        hadErrors = true;
    };
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        if (jsonOutput) {
            EmitFatal("could not find 'Rux.toml' in current directory or any "
                      "parent directory");
        }
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        if (jsonOutput) {
            EmitFatal("failed to parse 'Rux.toml'");
        }
        return 1;
    }
    std::string targetName = target.empty() ? HostTargetTriple() : std::string(target);
    if (!IsSupportedTargetTriple(targetName)) {
        if (jsonOutput) {
            EmitFatal("unsupported target '" + targetName + "'");
        }
        else {
            std::print(stderr,
                       "error: unsupported target '{}'; supported targets are "
                       "linux-x64, windows-x64, macos-x64, macos-aarch64, "
                       "freebsd-x64, openbsd-x64, netbsd-x64, dragonfly-x64, "
                       "illumos-x64\n",
                       targetName);
        }
        return 1;
    }
    const std::string hostTarget = HostTargetTriple();
    if (hostTarget != "unknown" && targetName != hostTarget) {
        if (jsonOutput) {
            EmitFatal("cross-target build from '" + hostTarget + "' to '" + targetName + "' is not supported yet");
        }
        else {
            std::print(stderr,
                       "error: cross-target build from '{}' to '{}' is not "
                       "supported yet\n",
                       hostTarget, targetName);
        }
        return 1;
    }
    if (!opts.quiet && !jsonOutput) {
        std::print("Checking {} v{} [{}]\n", manifest->package.name, manifest->package.version,
                   manifestPath->parent_path().string());
    }
    auto loadResult = SourceLoader::Load(manifestPath->parent_path());
    if (!loadResult) {
        if (jsonOutput) {
            EmitFatal("failed to load source files");
        }
        return 1;
    }
    for (const auto &err : loadResult->errors) {
        if (jsonOutput) {
            EmitDiag(ErrorDiagnostic(err));
            hadErrors = true;
        }
        else {
            std::print(stderr, "{}", err);
        }
    }
    bool lexErrors = false;
    std::vector<LexerResult> lexResults;
    lexResults.reserve(loadResult->files.size());
    for (const auto &file : loadResult->files) {
        if (opts.verbose && !jsonOutput) {
            std::print("    Lexing {}\n", file.path.string());
        }
        Lexer lexer(file.source, file.path.string());
        auto lexResult = lexer.Tokenize();
        for (const auto &diag : lexResult.diagnostics) {
            EmitDiag(diag);
            if (diag.IsError()) {
                lexErrors = true;
            }
        }
        lexResults.push_back(std::move(lexResult));
    }
    if (lexErrors) {
        hadErrors = true;
    }
    bool parseErrors = false;
    std::vector<ParseResult> parseResults;
    parseResults.reserve(loadResult->files.size());
    for (std::size_t fileIndex = 0; fileIndex < loadResult->files.size(); ++fileIndex) {
        const auto &file = loadResult->files[fileIndex];
        if (opts.verbose && !jsonOutput) {
            std::print("    Parsing {}\n", file.path.string());
        }
        auto &lexResult = lexResults[fileIndex];
        if (lexResult.HasErrors()) {
            continue;
        }
        Parser parser(std::move(lexResult.tokens), file.path.string());
        auto parseResult = parser.Parse();
        for (const auto &diag : parseResult.diagnostics) {
            EmitDiag(diag);
            if (diag.IsError()) {
                parseErrors = true;
            }
        }
        if (!parseResult.HasErrors()) {
            PruneModuleForTarget(parseResult.module, targetName);
            parseResults.push_back(std::move(parseResult));
        }
    }
    if (parseErrors) {
        hadErrors = true;
    }
    std::vector<ParseResult> depParseResults;
    std::vector<std::string> loadedPackages;
    std::vector<std::string> loadedModuleNames;
    std::vector<PendingPackage> pendingPackages;
    std::unordered_set<std::string> queuedPackageNames;
    std::vector<std::string> imports;
    ImportCollector collector{imports, targetName};
    for (const auto &pr : parseResults) {
        imports.clear();
        for (const auto &decl : pr.module.items) {
            if (decl) {
                collector.collect(*decl);
            }
        }
        for (const auto &pkgName : imports) {
            if (pkgName == manifest->package.name) {
                continue;
            }
            DependencyQueue depQueue{.pkgName = pkgName,
                                     .ownerManifest = *manifest,
                                     .ownerRoot = manifestPath->parent_path(),
                                     .queuedPackageNames = queuedPackageNames,
                                     .pendingPackages = pendingPackages,
                                     .targetName = targetName};
            if (!enqueueDependency(depQueue, EmitDiag)) {
                hadErrors = true;
                break;
            }
        }
    }
    if (!hadErrors) {
        HandlePendingIndex(hadErrors, opts, jsonOutput, pendingPackages, targetName, imports, collector,
                           depParseResults, loadedPackages, loadedModuleNames, queuedPackageNames, EmitDiag);
    }
    if (!hadErrors) {
        HandleErrors(hadErrors, parseResults, depParseResults, loadedPackages, loadedModuleNames, *manifest, targetName,
                     EmitDiag);
    }
    if (jsonOutput) {
        PrintDiagnosticsJson(jsonDiags, !hadErrors);
    }
    return hadErrors ? 1 : 0;
}

int Cli::RunInstall(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view packageSpec;
    bool packageFromDev = false;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("install");
            return 0;
        }
        if (arg == "--dev") {
            packageFromDev = true;
            continue;
        }
        if (!arg.starts_with('-') && packageSpec.empty()) {
            packageSpec = arg;
            continue;
        }
        PrintUnknownOption(arg, "install");
        return 1;
    }
    std::vector<std::string> queue;
    std::unordered_set<std::string> queued;
    const std::string installTarget = HostTargetTriple();
    // Seed the work queue: either the explicitly named package, or every
    // registry dependency declared by the current project's manifest. From
    // here both cases share the same transitive resolution loop below.
    if (!packageSpec.empty()) {
        auto [pkgName, pkgVersion] = ParsePackageSpec(packageSpec);
        queue.push_back(pkgName);
        queued.insert(pkgName);
    }
    else {
        const auto manifestPath = RequireManifest(opts.manifest);
        if (!manifestPath) {
            return 1;
        }
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            return 1;
        }
        for (const auto &dep : manifest->EffectiveDependencies(installTarget)) {
            if (const std::string packageName = DependencyPackageName(dep);
                dep.path.empty() && !queued.contains(packageName)) {
                queue.push_back(packageName);
                queued.insert(packageName);
            }
        }
        if (queue.empty()) {
            if (!opts.quiet) {
                std::print("  No registry dependencies to install.\n");
            }
            return 0;
        }
    }
    if (!opts.quiet) {
        std::print("     Fetching registry...\n");
    }
    const auto jsonOptInstall = FetchUrl(std::string(kRegistryUrl));
    if (!jsonOptInstall) {
        std::print(stderr, "error: failed to fetch package registry\n");
        return 1;
    }
    int installed = 0;
    int upToDate = 0;
    // Breadth-first install: each downloaded package's own dependencies are
    // appended to the queue, so the whole transitive graph is resolved
    // (e.g. installing Std also pulls in its platform packages).
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const std::string &pkgName = queue[i];
        const std::string repoUrl = JsonFindPackageRepository(*jsonOptInstall, pkgName);
        if (repoUrl.empty()) {
            std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
            return 1;
        }
        const std::filesystem::path pkgDir = RegistryPackagesDir() / pkgName;
        std::error_code ec;
        create_directories(pkgDir.parent_path(), ec);
        if (exists(pkgDir)) {
            if (!opts.quiet) {
                std::print("   Up-to-date {}\n", pkgName);
            }
            ++upToDate;
        }
        else {
            if (!opts.quiet) {
                std::print("  Downloading {} from {}...\n", pkgName, repoUrl);
            }
            if (!GitClone(repoUrl, pkgDir, packageFromDev)) {
                std::print(stderr, "error: failed to clone '{}'\n", repoUrl);
                return 1;
            }
            if (!opts.quiet) {
                std::print("    Installed {} at {}\n", pkgName, pkgDir.string());
            }
            ++installed;
        }
        if (const auto depManifest = Manifest::Load(pkgDir / "Rux.toml")) {
            for (const auto &dep : depManifest->EffectiveDependencies(installTarget)) {
                if (const std::string depPackageName = DependencyPackageName(dep);
                    dep.path.empty() && !queued.contains(depPackageName)) {
                    queue.push_back(depPackageName);
                    queued.insert(depPackageName);
                }
            }
        }
    }
    if (!opts.quiet) {
        std::print("     Summary: {} installed, {} already up-to-date\n", installed, upToDate);
    }
    return 0;
}

int Cli::RunUninstall(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view packageName;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("uninstall");
            return 0;
        }
        if (!arg.starts_with('-') && packageName.empty()) {
            packageName = arg;
            continue;
        }
        PrintUnknownOption(arg, "uninstall");
        return 1;
    }

    if (!packageName.empty()) {
        const std::filesystem::path pkgDir = RegistryPackagesDir() / std::string(packageName);
        if (!std::filesystem::exists(pkgDir)) {
            std::print(stderr, "error: package '{}' is not installed\n", packageName);
            return 1;
        }
        std::error_code ec;
        std::filesystem::remove_all(pkgDir, ec);
        if (ec) {
            std::print(stderr, "error: failed to remove '{}': {}\n", pkgDir.string(), ec.message());
            return 1;
        }
        if (!opts.quiet) {
            std::print("   Uninstalled {}\n", packageName);
        }
        return 0;
    }
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::vector<std::string> toRemove;
    for (const auto &dep : manifest->EffectiveDependencies(HostTargetTriple())) {
        if (dep.path.empty()) {
            toRemove.push_back(DependencyPackageName(dep));
        }
    }
    if (toRemove.empty()) {
        if (!opts.quiet) {
            std::print("  No registry dependencies to uninstall.\n");
        }
        return 0;
    }
    int removed = 0;
    int notFound = 0;
    for (const auto &pkgName : toRemove) {
        const std::filesystem::path pkgDir = RegistryPackagesDir() / pkgName;
        if (!std::filesystem::exists(pkgDir)) {
            if (!opts.quiet) {
                std::print("  Not installed {}\n", pkgName);
            }
            ++notFound;
            continue;
        }
        std::error_code ec;
        std::filesystem::remove_all(pkgDir, ec);
        if (ec) {
            std::print(stderr, "error: failed to remove '{}': {}\n", pkgDir.string(), ec.message());
            return 1;
        }
        if (!opts.quiet) {
            std::print("   Uninstalled {}\n", pkgName);
        }
        ++removed;
    }
    if (!opts.quiet) {
        std::print("     Summary: {} uninstalled, {} not installed\n", removed, notFound);
    }
    return 0;
}

int Cli::RunAdd(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view spec;
    std::string_view pathArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
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
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    auto [pkgName, pkgVersion] = ParsePackageSpec(spec);

    if (!pathArg.empty()) {
        const bool changed = manifest->AddPathDependency(pkgName, std::string(pathArg));
        if (!manifest->Save(*manifestPath)) {
            std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
            return 1;
        }
        if (!opts.quiet) {
            if (changed) {
                std::print("Added {} @ path '{}'\n", pkgName, pathArg);
            }
            else {
                std::print("Up-to-date {} @ path '{}'\n", pkgName, pathArg);
            }
        }
        return 0;
    }
    if (!opts.quiet) {
        std::print("     Fetching registry...\n");
    }
    const auto jsonOpt = FetchUrl(std::string(kRegistryUrl));
    if (!jsonOpt) {
        std::print(stderr, "error: failed to fetch package registry\n");
        return 1;
    }
    if (JsonFindPackageRepository(*jsonOpt, pkgName).empty()) {
        std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
        return 1;
    }
    const bool changed = manifest->AddDependency(pkgName, pkgVersion);
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        const std::string ver = pkgVersion.empty() ? "latest" : pkgVersion;
        if (changed) {
            std::print("Added {} @ {}\n", pkgName, ver);
        }
        else {
            std::print("Up-to-date {} @ {}\n", pkgName, ver);
        }
    }
    return 0;
}

int Cli::RunRemove(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view name;
    for (auto arg : args) {
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
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::string pkgName(name);
    if (!manifest->RemoveDependency(pkgName)) {
        std::print(stderr, "error: package '{}' is not a dependency\n", pkgName);
        return 1;
    }
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        std::print("     Removed {}\n", pkgName);
    }
    return 0;
}

int Cli::RunTest(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool isRelease = false;
    for (auto &arg : args) {
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("test");
            return 0;
        }
        PrintUnknownOption(arg, "test");
        return 1;
    }
    std::optional<std::filesystem::path> manifestPath;
    if (!opts.manifest.empty()) {
        manifestPath = opts.manifest;
        if (!std::filesystem::exists(*manifestPath)) {
            std::print(stderr, "error: specified manifest '{}' not found\n", manifestPath->string());
            return 1;
        }
    }
    else {
        manifestPath = Manifest::Find();
    }
    std::filesystem::path projectRoot;
    std::filesystem::path testsDir;
    if (manifestPath) {
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            return 1;
        }
        if (!opts.quiet) {
            std::print("Testing {} v{}\n", manifest->package.name, manifest->package.version);
        }
        projectRoot = manifestPath->parent_path();
        testsDir = projectRoot / "Tests";
    }
    else {
        projectRoot = std::filesystem::current_path();
        testsDir = projectRoot / "Tests";
        std::error_code ec;
        if (!std::filesystem::exists(testsDir, ec)) {
            static_cast<void>(RequireManifest()); // Prints standard "Rux.toml not found" error
            return 1;
        }
        if (!opts.quiet) {
            std::print("Testing workspace\n");
        }
    }
    const std::string_view profileName = isRelease ? "Release" : "Debug";
    // Collect test package directories: any subdirectory of Tests/ that
    // contains a Rux.toml with Type = "bin".
    std::vector<std::filesystem::path> testPackages;
    {
        std::error_code ec;
        if (!std::filesystem::exists(testsDir, ec)) {
            if (!opts.quiet) {
                std::print("  No Tests/ directory found — nothing to run.\n");
            }
            return 0;
        }
        for (const auto &entry : std::filesystem::directory_iterator(testsDir, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto toml = entry.path() / "Rux.toml";
            if (!std::filesystem::exists(toml)) {
                continue;
            }
            auto pkgManifest = Manifest::Load(toml);
            if (!pkgManifest) {
                continue;
            }
            // Only run binary packages (not DLLs / shared libraries).
            const auto &type = pkgManifest->package.type;
            if (type != "bin" && type != "Bin") {
                continue;
            }
            testPackages.push_back(entry.path());
        }
        std::sort(testPackages.begin(), testPackages.end());
    }
    if (testPackages.empty()) {
        if (!opts.quiet) {
            std::print("  No test packages found in Tests/.\n");
        }
        return 0;
    }

    // Outcome of running a single test package. On failure the combined
    // stdout/stderr of the test binary is captured so it can be replayed in a
    // dedicated `failures:` section after the whole suite has run.
    enum class TestStatus {
        Passed,
        Failed,
        BuildError,
        LaunchError
    };

    struct TestOutcome {
        TestStatus status = TestStatus::Passed;
        int exitCode = 0;
        std::string output;
        std::chrono::milliseconds duration{0};
    };

    // Helper: build a test package quietly, then execute the resulting binary
    // with its output captured.
    auto runOne = [&](const std::filesystem::path &pkgDir) -> TestOutcome {
        TestOutcome outcome;

        // Load the package manifest to derive the executable name and output path.
        auto pkgManifest = Manifest::Load(pkgDir / "Rux.toml");
        if (!pkgManifest) {
            std::print(stderr, "error: failed to parse '{}'\n", (pkgDir / "Rux.toml").string());
            outcome.status = TestStatus::BuildError;
            return outcome;
        }

        // Build: temporarily change the working directory into the package root
        // so that RequireManifest() and source paths resolve correctly.
        const auto savedCwd = std::filesystem::current_path();
        std::error_code ec;
        std::filesystem::current_path(pkgDir, ec);
        if (ec) {
            std::print(stderr, "error: cannot chdir into '{}': {}\n", pkgDir.string(), ec.message());
            outcome.status = TestStatus::BuildError;
            return outcome;
        }

        GlobalOptions buildOpts = opts;
        buildOpts.quiet = true; // suppress per-file build output for tests
        std::vector<std::string_view> buildArgs;
        if (isRelease) {
            buildArgs.emplace_back("--release");
        }
        buildArgs.emplace_back("--quiet");

        const int buildRc = RunBuild(buildArgs, buildOpts);
        std::filesystem::current_path(savedCwd, ec); // always restore CWD

        if (buildRc != 0) {
            outcome.status = TestStatus::BuildError;
            return outcome;
        }

        // Locate the built executable.
        const auto binDir = ResolveBuildOutputDir(pkgDir, *pkgManifest, profileName);
        std::string exeName = pkgManifest->package.name;
#if RUX_OS_WINDOWS
        exeName += ".exe";
#endif
        const auto exePath = binDir / exeName;

        if (!std::filesystem::exists(exePath)) {
            std::print(stderr, "error: built executable not found at '{}'\n", exePath.string());
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }

        if (opts.verbose)
            std::print("     Running `{}`\n", exePath.string());

        const auto start = std::chrono::steady_clock::now();

        // Execute the test binary, capturing its combined stdout/stderr.
#if RUX_OS_WINDOWS
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        // The read end must stay in this process only.
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        HANDLE hNul =
            CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::string cmdLine = "\"" + exePath.string() + "\"";
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.hStdInput = hNul != INVALID_HANDLE_VALUE ? hNul : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.dwFlags = STARTF_USESTDHANDLES;
        if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            std::print(stderr, "error: failed to launch '{}' (code {})\n", exePath.string(), GetLastError());
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            if (hNul != INVALID_HANDLE_VALUE)
                CloseHandle(hNul);
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        // Close our copy of the write end so ReadFile returns EOF once the
        // child exits and no writable handle remains.
        CloseHandle(writePipe);
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
            outcome.output.append(buf, n);
        }
        CloseHandle(readPipe);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (hNul != INVALID_HANDLE_VALUE)
            CloseHandle(hNul);
        outcome.exitCode = static_cast<int>(exitCode);
#else
        const std::string exeStr = exePath.string();
        const char *argv[] = {exeStr.c_str(), nullptr};
        int fds[2];
        if (pipe(fds) != 0) {
            std::print(stderr, "error: pipe failed\n");
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        const pid_t pid = fork();
        if (pid < 0) {
            std::print(stderr, "error: fork failed\n");
            close(fds[0]);
            close(fds[1]);
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, 0);
                close(devnull);
            }
            dup2(fds[1], 1);
            dup2(fds[1], 2);
            close(fds[0]);
            close(fds[1]);
            execv(exeStr.c_str(), const_cast<char *const *>(argv));
            _exit(127);
        }
        close(fds[1]);
        char buf[4096];
        ssize_t n = 0;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
            outcome.output.append(buf, static_cast<std::size_t>(n));
        }
        close(fds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        outcome.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif

        outcome.duration = ElapsedMs(start);
        outcome.status = outcome.exitCode == 0 ? TestStatus::Passed : TestStatus::Failed;
        return outcome;
    };
    const AnsiStyle style{ColorEnabled(opts.color)};
    if (!opts.quiet) {
        std::print("Running {} {}\n\n", testPackages.size(), testPackages.size() == 1 ? "test" : "tests");
    }
    // Width of the test-name column, so the trailing timings line up.
    std::size_t nameWidth = 0;
    for (const auto &pkgDir : testPackages) {
        nameWidth = std::max(nameWidth, pkgDir.filename().string().size());
    }

    // Run every discovered test package and tally results, buffering the output
    // of any failures for replay after the run.
    struct Failure {
        std::string label;
        TestStatus status;
        int exitCode;
        std::string output;
    };

    std::vector<Failure> failures;
    int passed = 0;
    int failed = 0;
    const auto suiteStart = std::chrono::steady_clock::now();
    for (const auto &pkgDir : testPackages) {
        const std::string label = pkgDir.filename().string();
        TestOutcome outcome = runOne(pkgDir);
        // Trailing detail: a timing for tests that ran, otherwise the failure kind.
        std::string detail;
        switch (outcome.status) {
        case TestStatus::BuildError:
            detail = "build error";
            break;
        case TestStatus::LaunchError:
            detail = "launch error";
            break;
        default:
            detail = std::format("{} ms", outcome.duration.count());
            break;
        }
        if (outcome.status == TestStatus::Passed) {
            ++passed;
            if (!opts.quiet) {
                std::print("{}[PASSED]{} {:<{}} ({})\n", style.Green(), style.Reset(), label, nameWidth, detail);
            }
        }
        else {
            ++failed;
            if (!opts.quiet) {
                std::print("{}[FAILED]{} {:<{}} ({})\n", style.Red(), style.Reset(), label, nameWidth, detail);
            }
            failures.push_back({label, outcome.status, outcome.exitCode, std::move(outcome.output)});
        }
    }
    const double elapsed = ElapsedSeconds(suiteStart);
    // Detail each failure, replaying any captured output.
    if (!failures.empty() && !opts.quiet) {
        std::print("\nFailures:\n\n");
        int index = 0;
        for (const auto &f : failures) {
            ++index;
            std::print("{}) {}\n", index, f.label);
            switch (f.status) {
            case TestStatus::Failed:
                std::print("   Exit code: {}\n", f.exitCode);
                break;
            case TestStatus::BuildError:
                std::print("   Build error\n");
                break;
            default:
                std::print("   Launch error\n");
                break;
            }
            if (!f.output.empty()) {
                std::print("   Output:\n");
                std::string_view out{f.output};
                std::size_t pos = 0;
                while (pos < out.size()) {
                    const auto nl = out.find('\n', pos);
                    const auto line = out.substr(pos, nl == std::string_view::npos ? out.size() - pos : nl - pos);
                    std::print("     {}\n", line);
                    if (nl == std::string_view::npos) {
                        break;
                    }
                    pos = nl + 1;
                }
            }
            std::print("\n");
        }
    }
    // Summary block.
    const int total = passed + failed;
    if (!opts.quiet || failed > 0) {
        std::print("Test Result:\n");
        std::print("  Passed: {}{}{}\n", style.Green(), passed, style.Reset());
        if (failed > 0) {
            std::print("  Failed: {}{}{}\n", style.Red(), failed, style.Reset());
        }
        else {
            std::print("  Failed: {}\n", failed);
        }
        std::print("  Total : {}\n", total);
        std::print("  Time  : {:.2f}s\n", elapsed);
    }
    return failed == 0 ? 0 : 1;
}

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

int Cli::RunHelp(std::span<const std::string_view> args, const GlobalOptions &) {
    if (!args.empty()) {
        PrintHelpFor(args.front());
    }
    else {
        PrintHelp();
    }
    return 0;
}

int Cli::RunVersion(const GlobalOptions &) {
    PrintVersion();
    return 0;
}

int Cli::RunFmt(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool check = false;
    bool manifestOnly = false;
    for (auto &arg : args) {
        if (arg == "--check") {
            check = true;
            continue;
        }
        if (arg == "--manifest-only") {
            manifestOnly = true;
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
    if (manifestOnly) {
        auto manifest = Manifest::Load(*manifestPath);
        if (!manifest) {
            std::print(stderr, "error: failed to parse manifest file '{}'\n", manifestPath->string());
            return 1;
        }
        // Get formatted content by saving to a temp file and reading it
        auto tempPath = manifestPath->parent_path() / "Rux.toml.fmt.tmp";
        if (!manifest->Save(tempPath)) {
            std::print(stderr, "error: failed to serialize formatted manifest\n");
            return 1;
        }
        std::string formattedContent;
        {
            std::ifstream tempFile(tempPath, std::ios::binary);
            if (tempFile) {
                formattedContent.assign(std::istreambuf_iterator<char>(tempFile), std::istreambuf_iterator<char>());
            }
        }
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
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
                return 1;
            }
            if (!opts.quiet) {
                std::print("  Manifest is already formatted: {}\n", manifestPath->string());
            }
            return 0;
        }
        if (originalContent != formattedContent) {
            if (!opts.quiet) {
                std::print("  Formatting {}\n", manifestPath->string());
            }
            if (!manifest->Save(*manifestPath)) {
                std::print(stderr, "error: failed to write manifest file '{}'\n", manifestPath->string());
                return 1;
            }
        }
        else {
            if (!opts.quiet) {
                std::print("  Manifest is already formatted: {}\n", manifestPath->string());
            }
        }
        return 0;
    }
    auto sourceDir = root / "Source";
    if (!std::filesystem::exists(sourceDir)) {
        if (!opts.quiet) {
            std::print("  No source directory found.\n");
        }
        return 0;
    }
    int fileCount = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(sourceDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".rux") {
            continue;
        }
        ++fileCount;
        if (!opts.quiet) {
            if (check) {
                std::print("  Checking   {}\n", entry.path().string());
            }
            else {
                std::print("  Formatting {}\n", entry.path().string());
            }
        }
        // TODO: source formatter
    }
    if (fileCount == 0 && !opts.quiet) {
        std::print("  No .rux files found.\n");
    }
    return 0;
}

int Cli::RunDoc(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool openAfter = false;
    for (auto &arg : args) {
        if (arg == "--open") {
            openAfter = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("doc");
            return 0;
        }
        PrintUnknownOption(arg, "doc");
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
    if (!opts.quiet) {
        std::print("  Generating documentation for {} v{}\n", manifest->package.name, manifest->package.version);
    }

    // TODO: documentation generator

    if (openAfter && !opts.quiet) {
        std::print("     Opening documentation...\n");
    }

    return 0;
}

int Cli::RunList(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool global = false;
    for (auto arg : args) {
        if (arg == "--global") {
            global = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("list");
            return 0;
        }
        PrintUnknownOption(arg, "list");
        return 1;
    }
    if (global) {
        const auto cacheDir = RegistryPackagesDir();
        std::vector<std::string> packages;
        std::error_code ec;
        if (std::filesystem::exists(cacheDir, ec)) {
            for (const auto &entry : std::filesystem::directory_iterator(cacheDir, ec)) {
                if (entry.is_directory()) {
                    packages.push_back(entry.path().filename().string());
                }
            }
            std::ranges::sort(packages);
        }
        if (packages.empty()) {
            if (!opts.quiet) {
                std::print("  Global cache is empty ({})\n", cacheDir.string());
            }
            return 0;
        }
        std::print("Global cache ({} package{} at {}):\n", packages.size(), packages.size() == 1 ? "" : "s",
                   cacheDir.string());
        for (const auto &pkg : packages) {
            std::print("  {}\n", pkg);
        }
        return 0;
    }
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    if (manifest->dependencies.empty()) {
        if (!opts.quiet) {
            std::print("  No dependencies.\n");
        }
        return 0;
    }
    std::print("Dependencies ({}):\n", manifest->dependencies.size());
    for (const auto &dep : manifest->dependencies) {
        if (!dep.path.empty()) {
            std::print("  {} (path: {})\n", dep.name, dep.path);
        }
        else {
            const std::string ver = dep.version.empty() ? "latest" : dep.version;
            std::print("  {} @ {}\n", dep.name, ver);
        }
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

int Cli::RunUpdate(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool global = false;
    for (auto &arg : args) {
        if (arg == "--global") {
            global = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("update");
            return 0;
        }
        PrintUnknownOption(arg, "update");
        return 1;
    }
    if (global) {
        const auto cacheDir = RegistryPackagesDir();
        std::vector<std::filesystem::path> pkgDirs;
        std::error_code ec;
        if (std::filesystem::exists(cacheDir, ec)) {
            for (const auto &entry : std::filesystem::directory_iterator(cacheDir, ec)) {
                if (entry.is_directory()) {
                    pkgDirs.push_back(entry.path());
                }
            }
        }
        if (pkgDirs.empty()) {
            if (!opts.quiet) {
                std::print("  No packages in global cache to update.\n");
            }
            return 0;
        }
        int updated = 0;
        for (const auto &pkgDir : pkgDirs) {
            const std::string pkgName = pkgDir.filename().string();
            if (!opts.quiet) {
                std::print("    Updating {}...\n", pkgName);
            }
            if (!GitPull(pkgDir)) {
                std::print(stderr, "error: failed to update '{}'\n", pkgName);
                return 1;
            }
            ++updated;
        }
        if (!opts.quiet) {
            std::print("     Summary: {} updated\n", updated);
        }
        return 0;
    }
    const auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::vector<std::string> queue;
    std::unordered_set<std::string> queued;
    const std::string updateTarget = HostTargetTriple();
    for (const auto &dep : manifest->EffectiveDependencies(updateTarget)) {
        const std::string packageName = DependencyPackageName(dep);
        if (dep.path.empty() && !queued.count(packageName)) {
            queue.push_back(packageName);
            queued.insert(packageName);
        }
    }
    if (queue.empty()) {
        if (!opts.quiet) {
            std::print("  No registry dependencies to update.\n");
        }
        return 0;
    }
    if (!opts.quiet) {
        std::print("     Fetching registry...\n");
    }
    const auto jsonOpt = FetchUrl(std::string(kRegistryUrl));
    if (!jsonOpt) {
        std::print(stderr, "error: failed to fetch package registry\n");
        return 1;
    }
    int updated = 0;
    int installed = 0;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const std::string &pkgName = queue[i];
        const std::string repoUrl = JsonFindPackageRepository(*jsonOpt, pkgName);
        if (repoUrl.empty()) {
            std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
            return 1;
        }
        const std::filesystem::path pkgDir = RegistryPackagesDir() / pkgName;
        std::error_code ec;
        std::filesystem::create_directories(pkgDir.parent_path(), ec);

        if (std::filesystem::exists(pkgDir)) {
            if (!opts.quiet) {
                std::print("    Updating {}...\n", pkgName);
            }
            if (!GitPull(pkgDir)) {
                std::print(stderr, "error: failed to update '{}'\n", pkgName);
                return 1;
            }
            ++updated;
        }
        else {
            if (!opts.quiet) {
                std::print("  Downloading {} from {}...\n", pkgName, repoUrl);
            }
            if (!GitClone(repoUrl, pkgDir, false)) {
                std::print(stderr, "error: failed to clone '{}'\n", repoUrl);
                return 1;
            }
            if (!opts.quiet) {
                std::print("    Installed {} at {}\n", pkgName, pkgDir.string());
            }
            ++installed;
        }
        // Enqueue registry deps declared by this package
        if (const auto depManifest = Manifest::Load(pkgDir / "Rux.toml")) {
            for (const auto &dep : depManifest->EffectiveDependencies(updateTarget)) {
                const std::string depPackageName = DependencyPackageName(dep);
                if (dep.path.empty() && !queued.count(depPackageName)) {
                    queue.push_back(depPackageName);
                    queued.insert(depPackageName);
                }
            }
        }
    }
    if (!opts.quiet) {
        std::print("     Summary: {} updated, {} newly installed\n", updated, installed);
    }
    return 0;
}

// TODO: Make this look in the registry instead of installed packages
// TODO: Extend Package manifest metadata support
int Cli::RunInfo(std::span<const std::string_view> args, const GlobalOptions &opts) {
    (void)opts;
    std::string_view packageName;
    bool jsonOutput = false;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("info");
            return 0;
        }
        if (arg == "--json") {
            jsonOutput = true;
            continue;
        }
        if (!arg.starts_with('-') && packageName.empty()) {
            packageName = arg;
            continue;
        }
        PrintUnknownOption(arg, "info");
        return 1;
    }
    std::filesystem::path manifestPath;
    if (!opts.manifest.empty()) {
        manifestPath = opts.manifest;
        if (!std::filesystem::exists(manifestPath)) {
            std::print(stderr, "error: specified manifest '{}' not found\n", manifestPath.string());
            return 1;
        }
    }
    else if (packageName.empty()) {
        auto localManifestOpt = Manifest::Find(std::filesystem::current_path());
        if (!localManifestOpt) {
            std::print(stderr, "error: missing package name, and no Rux.toml found in current directory\n");
            return 1;
        }
        manifestPath = *localManifestOpt;
    }
    else {
        const auto packageDir = RegistryPackagesDir() / std::string(packageName);
        manifestPath = packageDir / "Rux.toml";

        if (!std::filesystem::exists(manifestPath)) {
            std::print(stderr, "error: package '{}' is not installed\n", packageName);
            return 1;
        }
    }
    auto manifest = Manifest::Load(manifestPath);
    if (!manifest) {
        std::print(stderr, "error: failed to parse '{}'\n", manifestPath.string());
        return 1;
    }
    // not using nlohmann/json.hpp to keep compiler as small and fast as
    // possible
    if (jsonOutput) {
        std::print("{}\n", "{");
        std::print("  \"name\": \"{}\",\n", manifest->package.name);
        std::print("  \"version\": \"{}\",\n", manifest->package.version);
        std::print("  \"type\": \"{}\",\n", manifest->package.type);
        if (!manifest->package.description.empty()) {
            std::print("  \"description\": \"{}\",\n", manifest->package.description);
        }
        if (!manifest->package.authors.empty()) {
            std::print("  \"authors\": \"{}\",\n", manifest->package.authors);
        }
        if (!manifest->package.license.empty()) {
            std::print("  \"license\": \"{}\",\n", manifest->package.license);
        }
        if (!manifest->package.repository.empty()) {
            std::print("  \"repository\": \"{}\",\n", manifest->package.repository);
        }
        if (!manifest->package.homepage.empty()) {
            std::print("  \"homepage\": \"{}\",\n", manifest->package.homepage);
        }
        std::print("  \"dependencies\": [\n");
        for (size_t i = 0; i < manifest->dependencies.size(); ++i) {
            const auto &dep = manifest->dependencies[i];
            std::print("    {}", "{");
            std::print("\"name\": \"{}\"", dep.name);

            if (!dep.path.empty()) {
                std::print(", \"path\": \"{}\"", dep.path);
            }
            else {
                std::print(", \"version\": \"{}\"", dep.version.empty() ? "*" : dep.version);
            }
            // Only add a comma if this isn't the last element in the vector
            if (i + 1 < manifest->dependencies.size()) {
                std::print("    {},\n", "}");
            }
            else {
                std::print("    {}\n", "}");
            }
        }
        std::print("  ]\n");
        std::print("{}\n", "}");
    }
    else {
        std::print("Name:        {}\n"
                   "Version:     {}\n"
                   "Type:        {}\n",
                   manifest->package.name, manifest->package.version, manifest->package.type);
        if (!manifest->package.description.empty()) {
            std::print("Description: {}\n", manifest->package.description);
        }
        if (!manifest->package.authors.empty()) {
            std::print("Authors:     {}\n", manifest->package.authors);
        }
        if (!manifest->package.license.empty()) {
            std::print("License:     {}\n", manifest->package.license);
        }
        if (!manifest->package.repository.empty()) {
            std::print("Repository:  {}\n", manifest->package.repository);
        }
        if (!manifest->package.homepage.empty()) {
            std::print("Homepage:    {}\n", manifest->package.homepage);
        }
        if (!manifest->dependencies.empty()) {
            std::print("\nDependencies:\n");
            for (const auto &dep : manifest->dependencies) {
                if (!dep.path.empty()) {
                    std::print("  - {} (path: {})\n", dep.name, dep.path);
                }
                else {
                    std::print("  - {} @ {}\n", dep.name, dep.version.empty() ? "*" : dep.version);
                }
            }
        }
    }
    return 0;
}
