// Copyright (c) Rux contributors
// SPDX-License-Identifier: MIT

#include "Rux/Cli/Cli.h"
#include "Rux/Platform/Defines.h"
#include "Rux/Version.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdio>
#include <functional>
#include <iterator>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>

#if RUX_OS_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif


namespace Rux {
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

        // Matches any type compatible with std::size(), returning a value implicitly convertible to std::size_t.
        template <typename T>
        concept Sizable = requires(const T& val) {
            { std::size(val) } -> std::convertible_to<std::size_t>;
        };

        template <std::ranges::input_range Range, typename Proj = std::identity>
            requires Sizable<std::indirect_result_t<Proj, std::ranges::iterator_t<Range>>>
        constexpr auto MaxSize(const Range& range, Proj proj = {}) -> std::size_t {
            std::size_t maxSize = 0;

            for (const auto& item : range) {
                const auto& value = std::invoke(proj, item);
                maxSize = std::max(static_cast<std::size_t>(std::size(value)), maxSize);
            }

            return maxSize;
        }

        namespace Layout {
            constexpr std::size_t DefaultWidth = 80;
            constexpr std::size_t MinWidth = 40;

            constexpr std::size_t BlockIndent = 2;
            constexpr std::size_t AlignedPadding = 4;

            constexpr auto Whitespace = " \t"sv;
            constexpr auto CliName = "rux"sv;
        } // namespace Layout


        auto GetTerminalWidth() -> std::size_t {
#if RUX_OS_WINDOWS
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) == TRUE) {
                const auto width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
                return std::max(static_cast<std::size_t>(width), Layout::MinWidth);
            }
#else
            winsize w{};
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1 && w.ws_col > 0) {
                return std::max(static_cast<std::size_t>(w.ws_col), Layout::MinWidth);
            }
#endif

            return Layout::DefaultWidth;
        }

        constexpr auto UsableWidth(const std::size_t terminalWidth, const std::size_t indent) -> std::size_t {
            if (terminalWidth <= indent) {
                return Layout::MinWidth;
            }

            return std::max(terminalWidth - indent, Layout::MinWidth);
        }

        template <typename Callback>
        constexpr auto ProcessLine(std::string_view line, const std::size_t width, Callback callback) -> void {
            while (!line.empty()) {
                // Skip leading spaces by sliding the view forward
                const auto firstNonSpace = line.find_first_not_of(Layout::Whitespace);
                if (firstNonSpace == std::string_view::npos) {
                    break;
                }
                line.remove_prefix(firstNonSpace);

                std::size_t cutLength = line.size();

                // If the remaining line exceeds the allowed width, find a break point
                if (width < line.size()) {
                    cutLength = line.find_last_of(Layout::Whitespace, width);

                    // If no space exists within the width constraint, force a hard break at the NEXT space
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
        constexpr auto Wrap(std::string_view text, std::size_t width, Callback callback) -> void {
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

        auto PrintCmdLine(std::string_view cmd, std::string_view suffix) -> void {
            std::print("{0:<{1}}{2}", "", Layout::BlockIndent, Layout::CliName);

            if (!cmd.empty()) {
                std::print(" {}", cmd);
            }

            if (!suffix.empty()) {
                std::print(" {}", suffix);
            }

            std::println();
        }

        auto PrintBlock(std::string_view title, std::string_view text, size_t term_width) -> void {
            if (text.empty()) {
                return;
            }

            if (!title.empty()) {
                std::println("{}:", title);
            }

            const size_t usable = UsableWidth(term_width, Layout::BlockIndent);

            Wrap(text, usable, [&](std::string_view line) -> void {
                // If the line is empty, pad with 0 spaces (prints just a newline)
                std::println("{0:<{1}}{2}", "", line.empty() ? 0 : Layout::BlockIndent, line);
            });

            std::println("");
        }

        auto PrintAligned(const std::string_view left,
                          const std::string_view right,
                          const size_t leftWidth,
                          const size_t termWidth) -> void {
            const size_t indent = leftWidth + Layout::AlignedPadding;
            const size_t width = UsableWidth(termWidth, indent);
            bool first = true;

            Wrap(right, width, [&](std::string_view line) {
                if (first) {
                    std::println(
                        "{0:<{1}}{2:<{3}}{4}", "", Layout::BlockIndent, left, leftWidth + Layout::AlignedPadding, line);

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
            constexpr std::array add_opts = {
                OptionDoc{.flags = "--path <path>"sv, .desc = "Add a local path-based dependency"sv}};
            constexpr std::array add_exs = {"Std"sv, "Std@0.1.0"sv, "Json --path ../Json"sv};

            // Build
            constexpr std::array build_usage = {"[options]"sv};
            constexpr std::array build_opts = {
                OptionDoc{.flags = "--debug"sv, .desc = "Build with debug symbols (unoptimized output)"sv},
                OptionDoc{.flags = "--profile <n>"sv, .desc = "Build using a custom profile defined in Rux.toml"sv},
                OptionDoc{.flags = "--release"sv, .desc = "Build with release profile (optimized, no debug info)"sv},
                OptionDoc{.flags = "--stats"sv,
                          .desc = "Print build timing, source, performance, and output statistics"sv},
                OptionDoc{.flags = "--target <triple>"sv,
                          .desc = "Build for the specified target platform (e.g. x86, x64)"sv},
                OptionDoc{.flags = "-q, --quiet"sv, .desc = "Suppress non-essential output (only errors are shown)"sv},
                OptionDoc{.flags = "-v, --verbose"sv, .desc = "Enable verbose output for detailed build information"sv},
                OptionDoc{.flags = "--dump-asm"sv, .desc = "Write x86-64 assembly to Temp/Asm/out.asm"sv},
                OptionDoc{.flags = "--dump-ast"sv, .desc = "Write the parsed AST to Temp/Ast/<file>.ast"sv},
                OptionDoc{.flags = "--dump-hir"sv, .desc = "Write the high-level IR to Temp/Hir/hir.txt"sv},
                OptionDoc{.flags = "--dump-lir"sv, .desc = "Write the low-level IR to Temp/Lir/lir.txt"sv},
                OptionDoc{.flags = "--dump-rcu"sv,
                          .desc = "Write RCU object files to Temp/Obj/ and text dumps to Temp/Rcu/"sv},
                OptionDoc{.flags = "--dump-sema"sv, .desc = "Write semantic analysis results to Temp/Sema/sema.txt"sv},
                OptionDoc{.flags = "--dump-tokens"sv, .desc = "Write the token stream to Temp/Tokens/<file>.tokens"sv}};
            constexpr std::array build_exs = {""sv,
                                              "--debug"sv,
                                              "--release"sv,
                                              "--stats"sv,
                                              "--verbose --release"sv,
                                              "--dump-ast"sv,
                                              "--dump-hir"sv,
                                              "--dump-lir"sv,
                                              "--dump-asm"sv,
                                              "--dump-rcu"sv};

            // Check
            constexpr std::array check_usage = {"[options]"sv};
            constexpr std::array check_opts = {
                OptionDoc{.flags = "--json"sv, .desc = "Output diagnostics as JSON"sv},
                OptionDoc{.flags = "--target <triple>"sv, .desc = "Check for a specific target"sv}};
            constexpr std::array check_exs = {""sv, "--json"sv, "--target windows-x64"sv};

            // Clean
            constexpr std::array clean_usage = {"[options]"sv};
            constexpr std::array clean_opts = {
                OptionDoc{.flags = "--temp"sv, .desc = "Removes only Temp/ directory"sv}};
            constexpr std::array clean_exs = {""sv, "--temp"sv};

            // Doc
            constexpr std::array doc_usage = {"[options]"sv};
            constexpr std::array doc_opts = {
                OptionDoc{.flags = "--open"sv, .desc = "Open documentation after the generation"sv}};
            constexpr std::array doc_exs = {""sv, "--open"sv};

            // Fmt
            constexpr std::array fmt_usage = {"[options]"sv};
            constexpr std::array fmt_opts = {
                OptionDoc{.flags = "--check"sv, .desc = "Check formatting without modifying files"sv},
                OptionDoc{.flags = "--manifest"sv, .desc = "Format only the manifest (Rux.toml)"sv}};
            constexpr std::array fmt_exs = {""sv, "--check"sv, "--manifest"sv};

            // Info
            constexpr std::array info_usage = {"[package name]"sv};
            constexpr std::array info_opts = {
                OptionDoc{.flags = "--json"sv, .desc = "Returns a json instead of a string"sv}};
            constexpr std::array info_exs = {"Std"sv, "Windows"sv};

            // Init
            constexpr std::array init_usage = {"[options]"sv};
            constexpr std::array init_opts = {OptionDoc{.flags = "--bin"sv, .desc = "Create a binary package"sv},
                                              OptionDoc{.flags = "--lib"sv, .desc = "Create a library package"sv}};
            constexpr std::array init_exs = {""sv, "--bin"sv};

            // Install
            constexpr std::array install_usage = {""sv, "[package]"sv, "[package]@[version]"sv, "--dev [package]"sv};
            constexpr std::array install_opts = {
                OptionDoc{.flags = "--dev"sv,
                          .desc = "Clone the package repository's dev branch instead of the default branch"sv}};
            constexpr std::array install_exs = {""sv, "Std"sv, "Std@0.1.0"sv, "--dev Std"sv, "--dev Windows"sv};

            // List
            constexpr std::array list_usage = {"[options]"sv};
            constexpr std::array list_opts = {OptionDoc{
                .flags = "--global"sv, .desc = "List all packages in the global cache instead of the manifest"sv}};
            constexpr std::array list_exs = {""sv, "--global"sv};

            // New
            constexpr std::array new_usage = {"[name] [options]"sv};
            constexpr std::array new_opts = {
                OptionDoc{.flags = "--bin"sv, .desc = "Create a binary application (default)"sv},
                OptionDoc{.flags = "--lib"sv, .desc = "Create a library package"sv},
                OptionDoc{.flags = "--path <dir>"sv, .desc = "Create in a specific directory"sv}};
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
            constexpr std::array test_opts = {
                OptionDoc{.flags = "--release"sv, .desc = "Build with release profile"sv}};
            constexpr std::array test_exs = {""sv, "--release"sv};

            // Uninstall
            constexpr std::array uninstall_usage = {""sv, "[package]"sv};
            constexpr std::array uninstall_exs = {""sv, "Json"sv};

            // Update
            constexpr std::array update_usage = {"[options]"sv};
            constexpr std::array update_opts = {OptionDoc{
                .flags = "--global"sv,
                .desc = "Update all packages in the global cache instead of only those listed in the manifest"sv}};
            constexpr std::array update_exs = {""sv, "--global"sv};

            // Version
            constexpr std::array version_exs = {""sv, "rux -V"sv, "rux --version"sv};
        } // namespace Data

        namespace GlobalOpts {
            constexpr std::array catalog = {
                OptionDoc{.flags = "--color <auto|on|off>"sv, .desc = "Control colored output"sv},
                OptionDoc{.flags = "-h, --help"sv, .desc = "Show help information"sv},
                OptionDoc{.flags = "-q, --quiet"sv, .desc = "Do not show log messages"sv},
                OptionDoc{.flags = "-v, --verbose"sv, .desc = "Use verbose output"sv},
                OptionDoc{.flags = "-V, --version"sv, .desc = "Show version information"sv}};
        } // namespace GlobalOpts

        constexpr std::array G_COMMAND_HELP_MAPS = {
            CommandDoc{.name = "add"sv,
                       .shortDesc = "Add a dependency to the manifest"sv,
                       .description = "Add a dependency to the current package"sv,
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
                       .footer =
                           "Artifacts are stored under [Build].Output, defaulting to Bin/Debug/ or Bin/Release/."sv,
                       .examples = Data::build_exs,
                       .options = Data::build_opts},

            CommandDoc{.name = "check"sv,
                       .shortDesc = "Check package source code for errors"sv,
                       .description = {} /* Fallback */,
                       .usage = Data::check_usage,
                       .postUsage = {},
                       .footer = {},
                       .examples = Data::check_exs,
                       .options = Data::check_opts},

            CommandDoc{.name = "clean"sv,
                       .shortDesc = "Remove build artifacts"sv,
                       .description = "Remove all build artifacts and temporary files"sv,
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
                       .description = "Format all *.rux source files"sv,
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
                       .shortDesc = "Show package metadata and manifest"sv,
                       .description = "Show information about an installed Rux package"sv,
                       .usage = Data::info_usage,
                       .postUsage = {},
                       .footer = {},
                       .examples = Data::info_exs,
                       .options = Data::info_opts},

            CommandDoc{.name = "init"sv,
                       .shortDesc = "Initialize a Rux package in the directory"sv,
                       .description = "Initialize a new package in the current directory"sv,
                       .usage = Data::init_usage,
                       .postUsage = "If Rux.toml does not exist, it will be created."sv,
                       .footer = {},
                       .examples = Data::init_exs,
                       .options = Data::init_opts},

            CommandDoc{
                .name = "install"sv,
                .shortDesc = "Install dependencies"sv,
                .description = {} /* Fallback */,
                .usage = Data::install_usage,
                .postUsage =
                    "Without a package name, downloads all registry dependencies listed in Rux.toml.\nWith a package "
                    "name, adds it to Rux.toml and downloads it to the local cache."sv,
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

            CommandDoc{
                .name = "uninstall"sv,
                .shortDesc = "Uninstall dependencies"sv,
                .description = "Uninstall dependencies from the local cache"sv,
                .usage = Data::uninstall_usage,
                .postUsage =
                    "Without a package name, removes all registry dependencies listed in Rux.toml from the local "
                    "cache.\nWith a package name, removes only that package."sv,
                .footer = {},
                .examples = Data::uninstall_exs,
                .options = {}},

            CommandDoc{
                .name = "update"sv,
                .shortDesc = "Update dependencies"sv,
                .description = {} /* Fallback */,
                .usage = Data::update_usage,
                .postUsage = {},
                .footer =
                    "Without --global, checks all registry dependencies listed in Rux.toml and pulls the latest "
                    "changes. Missing packages are cloned from the registry.\nWith --global, updates every package "
                    "present in the local cache."sv,
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

            return std::ranges::all_of(G_COMMAND_HELP_MAPS, [](const CommandDoc& cmd) -> bool {
                if (cmd.name.empty() || cmd.shortDesc.empty()) {
                    return false;
                }
                if (!cmd.description.empty() && cmd.description == cmd.shortDesc) {
                    return false;
                }

                const bool hasBadOptions = std::ranges::any_of(cmd.options, [](const OptionDoc& opt) -> bool {
                    return opt.flags.empty() || opt.desc.empty() || opt.flags.starts_with(' ') ||
                        opt.flags.ends_with(' ');
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
                      "G_COMMAND_HELP_MAPS registry map entries must be strictly sorted alphabetically by name!");

        static_assert(VerifyRegistryIntegrity(),
                      "Duplicate commands, empty descriptions, or redundant metadata detected!");

        constexpr auto G_CMD_WIDTH = MaxSize(G_COMMAND_HELP_MAPS, &CommandDoc::name);
        constexpr auto G_OPT_WIDTH = MaxSize(GlobalOpts::catalog, &OptionDoc::flags);
    } // namespace

    auto Cli::PrintHelp() -> void {
        const size_t termWidth = GetTerminalWidth();

        std::println("Rux compiler and package manager\n");
        std::println("Usage: rux [command] [options] [-- args...]\n");

        std::println("Commands:");
        for (const auto& cmd : G_COMMAND_HELP_MAPS) {
            PrintAligned(cmd.name, cmd.shortDesc, G_CMD_WIDTH, termWidth);
        }

        std::println("\nOptions:");
        for (const auto& opt : GlobalOpts::catalog) {
            PrintAligned(opt.flags, opt.desc, G_OPT_WIDTH, termWidth);
        }

        std::println("\nUse 'rux help <command>' for more information about a command.");
    }

    auto Cli::PrintHelpFor(const std::string_view command) -> void {
        if (command == "help") {
            PrintHelp();
            return;
        }

        const auto it = std::ranges::lower_bound(G_COMMAND_HELP_MAPS, command, std::less<>{}, &CommandDoc::name);
        if (it == G_COMMAND_HELP_MAPS.end() || it->name != command) {
            PrintUnknownCommand(command);
            return;
        }

        const auto& [name, shortDesc, description, usage, postUsage, footer, examples, options] = *it;
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
        PrintBlock(""sv, footer, termWidth);

        if (!options.empty()) {
            const auto optWidth = MaxSize(options, &OptionDoc::flags);

            std::println("Options:");
            for (const auto& [flags, desc] : options) {
                PrintAligned(flags, desc, optWidth, termWidth);
            }
            std::println("");
        }

        if (!examples.empty()) {
            std::println("Examples:");
            for (const auto example : examples) {
                PrintCmdLine(name, example);
            }
            std::println("");
        }
    }

    auto Cli::PrintVersion() -> void {
        std::println("Rux {} ({} {})", RUX_VERSION, RUX_BUILD_DATE, RUX_BUILD_TIME);
    }

    auto Cli::PrintUnknownCommand(std::string_view command) -> void {
        std::println(stderr, "error: unknown command '{}'", command);
        std::println(stderr, "\nUse 'rux help' for a list of available commands.");
    }

    auto Cli::PrintUnknownOption(std::string_view option, std::string_view command) -> void {
        if (command.empty()) {
            std::println(stderr, "error: unknown option '{}'", option);
            std::println(stderr, "\nUse 'rux help' for a list of global options.");
        }
        else {
            std::println(stderr, "error: unknown option '{}' for command '{}'", option, command);
            std::println(stderr, "\nUse 'rux help {}' for more information about this command.", command);
        }
    }
} // namespace Rux
