// Command-line entry point: contract-driven validation and command dispatch.

#include "Cli/Cli.h"

#include "Cli/CliSpec.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Rux::CliContract;

namespace {
std::size_t EditDistance(const std::string_view left, const std::string_view right) {
    std::vector<std::size_t> row(right.size() + 1);
    for (std::size_t i = 0; i <= right.size(); ++i)
        row[i] = i;
    for (std::size_t i = 1; i <= left.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= right.size(); ++j) {
            const std::size_t previous = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (left[i - 1] == right[j - 1] ? 0 : 1)});
            diagonal = previous;
        }
    }
    return row.back();
}

template <typename Range, typename Projection>
std::optional<std::string_view> Closest(const std::string_view value, const Range &candidates, Projection projection) {
    std::size_t bestDistance = std::numeric_limits<std::size_t>::max();
    std::string_view best;
    for (const auto &candidate : candidates) {
        const std::string_view name = projection(candidate);
        const auto distance = EditDistance(value, name);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = name;
        }
    }
    const std::size_t threshold = value.size() <= 4 ? 1 : 3;
    return bestDistance <= threshold ? std::optional(best) : std::nullopt;
}

const OptionSpec *FindOption(const std::span<const OptionSpec> options, const std::string_view spelling) {
    const auto it =
        std::ranges::find_if(options, [&](const OptionSpec &option) { return OptionMatches(option, spelling); });
    return it == options.end() ? nullptr : &*it;
}

std::string_view OptionSpelling(const std::string_view argument) {
    return argument.substr(0, argument.find('='));
}

std::optional<std::string_view> AttachedValue(const std::string_view argument) {
    const auto equals = argument.find('=');
    return equals == std::string_view::npos ? std::nullopt : std::optional(argument.substr(equals + 1));
}

bool IsRepeatable(const std::string_view option) {
    return option == "--define" || option == "--emit";
}

bool Contains(const std::vector<std::string_view> &values, const std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}
} // namespace

void Cli::PrintUnknownCommand(const std::string_view command) {
    std::println(stderr, "error: unknown command '{}'", command);
    if (const auto suggestion = Closest(command, Commands(), [](const CommandSpec &spec) { return spec.name; })) {
        std::println(stderr, "  Did you mean '{}'?", *suggestion);
    }
    std::println(stderr, "\nUse 'rux help' for a list of available commands.");
}

void Cli::PrintUnknownOption(const std::string_view option, const std::string_view command) {
    if (command.empty()) {
        std::println(stderr, "error: unknown option '{}'", option);
        if (const auto suggestion = Closest(option, CliContract::GlobalOptions(),
                                            [](const OptionSpec &spec) { return PreferredOptionName(spec); })) {
            std::println(stderr, "  Did you mean '{}'?", *suggestion);
        }
        std::println(stderr, "\nUse 'rux help' for a list of global options.");
        return;
    }

    std::println(stderr, "error: unknown option '{}' for command '{}'", option, command);
    if (const auto *spec = FindCommand(command)) {
        std::vector<OptionSpec> candidates(spec->options.begin(), spec->options.end());
        const auto globals = CliContract::GlobalOptions();
        candidates.insert(candidates.end(), globals.begin(), globals.end());
        if (const auto suggestion = Closest(
                option, candidates, [](const OptionSpec &candidate) { return PreferredOptionName(candidate); })) {
            std::println(stderr, "  Did you mean '{}'?", *suggestion);
        }
    }
    std::println(stderr, "\nUse 'rux help {}' for more information about this command.", command);
}

Cli::Cli(const int argc, char *argv[])
    : args(argv, argc) {
}

int Cli::Run() const {
    std::vector<std::string_view> raw;
    raw.reserve(static_cast<std::size_t>(args.size()));
    for (auto *argument : args.subspan(1))
        raw.emplace_back(argument);
    if (raw.empty()) {
        PrintHelp();
        return 0;
    }

    GlobalOptions options;
    std::string_view command;
    const CommandSpec *commandSpec = nullptr;
    std::vector<std::string_view> commandArgs;
    std::vector<std::string_view> seenOptions;
    std::size_t operandCount = 0;
    bool passthrough = false;

    auto usageError = [](const std::string_view message) {
        std::println(stderr, "error: {}", message);
        return 2;
    };

    auto consumeGlobal = [&](std::size_t &index) -> std::optional<int> {
        const auto argument = raw[index];
        const auto spelling = OptionSpelling(argument);
        if (spelling == "-q" || spelling == "--quiet") {
            if (AttachedValue(argument))
                return usageError(std::format("option '{}' does not take a value", spelling));
            options.quiet = true;
            return 0;
        }
        if (spelling == "-v" || spelling == "--verbose") {
            if (AttachedValue(argument))
                return usageError(std::format("option '{}' does not take a value", spelling));
            options.verbose = true;
            return 0;
        }
        if (spelling != "--color" && spelling != "--manifest")
            return std::nullopt;

        auto value = AttachedValue(argument);
        if (!value) {
            if (index + 1 >= raw.size() || raw[index + 1].starts_with('-')) {
                return usageError(std::format("option '{}' requires a value", spelling));
            }
            value = raw[++index];
        }
        if (value->empty())
            return usageError(std::format("option '{}' requires a value", spelling));
        if (spelling == "--manifest") {
            options.manifest = *value;
        }
        else if (*value == "auto") {
            options.color = ColorMode::Auto;
        }
        else if (*value == "always") {
            options.color = ColorMode::On;
        }
        else if (*value == "never") {
            options.color = ColorMode::Off;
        }
        else {
            return usageError("--color expects one of: auto, always, never");
        }
        return 0;
    };

    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto argument = raw[i];
        if (!passthrough && (argument == "-h" || argument == "--help")) {
            if (command.empty())
                PrintHelp(options.color);
            else
                PrintHelpFor(command, options.color);
            return 0;
        }
        if (!passthrough && (argument == "-V" || argument == "--version")) {
            PrintVersion();
            return 0;
        }
        if (!passthrough) {
            if (const auto global = consumeGlobal(i)) {
                if (*global != 0)
                    return *global;
                continue;
            }
        }

        if (command.empty()) {
            if (argument == "--")
                return usageError("'--' is only valid after the 'run' command");
            if (argument.starts_with('-')) {
                PrintUnknownOption(OptionSpelling(argument));
                return 2;
            }
            command = argument;
            commandSpec = FindCommand(command);
            if (!commandSpec) {
                PrintUnknownCommand(command);
                return 2;
            }
            continue;
        }

        if (!passthrough && argument == "--") {
            if (!PositionalsFor(command).acceptsPassthrough) {
                return usageError(std::format("'--' is not supported by command '{}'", command));
            }
            passthrough = true;
            commandArgs.push_back(argument);
            continue;
        }
        if (passthrough) {
            commandArgs.push_back(argument);
            continue;
        }
        if (argument.starts_with('-')) {
            const auto spelling = OptionSpelling(argument);
            const auto *option = FindOption(commandSpec->options, spelling);
            if (!option) {
                PrintUnknownOption(spelling, command);
                return 2;
            }
            const auto preferred = PreferredOptionName(*option);
            if (!IsRepeatable(preferred) && Contains(seenOptions, preferred)) {
                return usageError(std::format("option '{}' may only be specified once", preferred));
            }
            seenOptions.push_back(preferred);
            commandArgs.push_back(spelling);
            auto value = AttachedValue(argument);
            if (OptionTakesValue(*option)) {
                if (!value) {
                    if (i + 1 >= raw.size() || raw[i + 1].starts_with('-')) {
                        return usageError(std::format("option '{}' requires a value", spelling));
                    }
                    value = raw[++i];
                }
                if (value->empty())
                    return usageError(std::format("option '{}' requires a value", spelling));
                commandArgs.push_back(*value);
            }
            else if (value) {
                return usageError(std::format("option '{}' does not take a value", spelling));
            }
            continue;
        }
        ++operandCount;
        commandArgs.push_back(argument);
    }

    if (command.empty()) {
        PrintHelp(options.color);
        return 0;
    }
    if (options.quiet && options.verbose)
        return usageError("--quiet and --verbose cannot be used together");

    const auto &positionals = PositionalsFor(command);
    if (operandCount < positionals.minimum) {
        return usageError(std::format("command '{}' requires an argument", command));
    }
    if (operandCount > positionals.maximum) {
        return usageError(std::format("too many arguments for command '{}'", command));
    }
    auto conflicts = [&](const std::string_view left, const std::string_view right) -> std::optional<int> {
        if (Contains(seenOptions, left) && Contains(seenOptions, right)) {
            return usageError(std::format("options '{}' and '{}' cannot be used together", left, right));
        }
        return std::nullopt;
    };
    if (const auto error = conflicts("--debug", "--release"))
        return *error;
    if (const auto error = conflicts("--source-only", "--manifest-only"))
        return *error;
    if (const auto error = conflicts("--executable", "--shared"))
        return *error;
    if (const auto error = conflicts("--executable", "--static"))
        return *error;
    if (const auto error = conflicts("--executable", "--source"))
        return *error;
    if (const auto error = conflicts("--shared", "--static"))
        return *error;
    if (const auto error = conflicts("--shared", "--source"))
        return *error;
    if (const auto error = conflicts("--static", "--source"))
        return *error;
    if (command == "uninstall" && operandCount != 0 && Contains(seenOptions, "--global")) {
        return usageError("--global cannot be combined with a package argument");
    }

    const std::span<const std::string_view> rest(commandArgs);
    if (command == "help")
        return RunHelp(rest, options);
    if (command == "version")
        return RunVersion(options);
    if (command == "build")
        return RunBuild(rest, options);
    if (command == "clean")
        return RunClean(rest, options);
    if (command == "doc")
        return RunDoc(rest, options);
    if (command == "fmt")
        return RunFmt(rest, options);
    if (command == "lint")
        return RunLint(rest, options);
    if (command == "init")
        return RunInit(rest, options);
    if (command == "install")
        return RunInstall(rest, options);
    if (command == "uninstall")
        return RunUninstall(rest, options);
    if (command == "list")
        return RunList(rest, options);
    if (command == "login")
        return RunLogin(rest, options);
    if (command == "logout")
        return RunLogout(rest, options);
    if (command == "new")
        return RunNew(rest, options);
    if (command == "pack")
        return RunPack(rest, options);
    if (command == "publish")
        return RunPublish(rest, options);
    if (command == "add")
        return RunAdd(rest, options);
    if (command == "remove")
        return RunRemove(rest, options);
    if (command == "run")
        return RunRun(rest, options);
    if (command == "test")
        return RunTest(rest, options);
    if (command == "update")
        return RunUpdate(rest, options);
    if (command == "info")
        return RunInfo(rest, options);
    if (command == "check")
        return RunCheck(rest, options);
    return 2;
}
