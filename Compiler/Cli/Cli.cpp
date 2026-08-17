// Command-line entry point: contract-driven validation and command dispatch.

#include "Cli/Cli.h"

#include "Cli/CliSpec.h"
#include "Cli/Reporter.h"
#include "Driver/BuildTarget.h"

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
/// Levenshtein distance, used to suggest what a mistyped command or option probably meant. Suggesting the nearest known
/// spelling turns an unknown-name error into an actionable one.
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
/// The nearest candidate to `value`, if one is near enough to be worth suggesting.
///
/// @return nullopt when nothing is close, since a wrong suggestion is worse than none
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

/// The flag part of an argument, with any `=value` tail removed, so `--target=linux-x86_64` matches `--target`.
std::string_view OptionSpelling(const std::string_view argument) {
    return argument.substr(0, argument.find('='));
}

/// The value written as `--flag=value`.
///
/// @return nullopt when the argument carries no attached value, meaning it takes the next argument instead
std::optional<std::string_view> AttachedValue(const std::string_view argument) {
    const auto equals = argument.find('=');
    return equals == std::string_view::npos ? std::nullopt : std::optional(argument.substr(equals + 1));
}

/// Whether giving the option more than once is meaningful rather than a mistake, as it is for `--define`.
bool IsRepeatable(const std::string_view option) {
    return option == "--define" || option == "--emit";
}

bool Contains(const std::vector<std::string_view> &values, const std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

std::string HelpCommand(const std::string_view command) {
    return command.empty() ? "rux help" : std::format("rux help {}", command);
}

int ReportUsageError(const ColorMode color, const std::string_view command, const std::string_view message,
                     const std::string_view note = {}, const std::string_view correction = {}) {
    const CliSupport::Reporter reporter(stderr, {.color = color});
    reporter.Error(message);
    if (!note.empty()) {
        reporter.Note(note);
    }
    if (!correction.empty()) {
        reporter.Help(correction);
    }
    reporter.Help(std::format("run '{}' for usage information", HelpCommand(command)));
    return 2;
}

std::string_view ValueDescription(const OptionSpec &option) {
    const auto preferred = PreferredOptionName(option);
    if (preferred == "--target")
        return "target triple";
    if (preferred == "--manifest")
        return "manifest path";
    if (preferred == "--color")
        return "color mode";
    if (preferred == "--registry")
        return "registry URL";
    if (preferred == "--namespace")
        return "namespace";
    if (preferred == "--define")
        return "compile-time definition";
    if (preferred == "--emit")
        return "output kind";
    if (option.flags.contains("<dir>"))
        return "directory";
    return "path";
}

/// A realistic value for this option, so a usage error can show a complete invocation rather than a placeholder the
/// reader still has to fill in.
std::string_view SampleValue(const std::string_view command, const OptionSpec &option) {
    const auto preferred = PreferredOptionName(option);
    if (preferred == "--target")
        return "linux-x86_64";
    if (preferred == "--manifest")
        return "Rux.toml";
    if (preferred == "--color")
        return "auto";
    if (preferred == "--registry")
        return "http://localhost:8080";
    if (preferred == "--namespace")
        return "Rux";
    if (preferred == "--define")
        return "Feature=true";
    if (preferred == "--emit")
        return "lir";
    if (preferred == "--output")
        return option.flags.contains("<dir>") ? "Docs" : "Dist/Package.ruxpkg";
    if (preferred == "--path")
        return command == "add" ? "../Json" : ".";
    return "value";
}

std::string OptionInvocation(const std::string_view command, const OptionSpec &option) {
    std::string invocation = command.empty() ? "rux" : std::format("rux {}", command);
    invocation += std::format(" {}", PreferredOptionName(option));
    if (OptionTakesValue(option)) {
        invocation += std::format(" {}", SampleValue(command, option));
    }
    return invocation;
}

std::string FirstCommandExample(const CommandSpec &command) {
    const auto example = std::ranges::find_if(
        command.examples, [](const std::string_view value) { return !value.empty() && !value.starts_with("rux"); });
    return example == command.examples.end() ? std::format("rux {}", command.name)
                                             : std::format("rux {} {}", command.name, *example);
}
} // namespace

void Cli::PrintUnknownCommand(const std::string_view command, const ColorMode color) {
    std::string correction;
    if (const auto suggestion = Closest(command, Commands(), [](const CommandSpec &spec) { return spec.name; })) {
        correction = std::format("try 'rux {}'", *suggestion);
    }
    static_cast<void>(ReportUsageError(color, {}, std::format("unknown command '{}'", command), {}, correction));
}

void Cli::PrintUnknownOption(const std::string_view option, const std::string_view command, const ColorMode color) {
    std::string correction;
    if (command.empty()) {
        if (const auto suggestion = Closest(option, CliContract::GlobalOptions(),
                                            [](const OptionSpec &spec) { return PreferredOptionName(spec); })) {
            const auto globals = CliContract::GlobalOptions();
            const auto *spec = FindOption(globals, *suggestion);
            if (spec != nullptr) {
                correction = std::format("try '{}'", OptionInvocation({}, *spec));
            }
        }
        static_cast<void>(ReportUsageError(color, {}, std::format("unknown option '{}'", option), {}, correction));
        return;
    }

    if (const auto *spec = FindCommand(command)) {
        std::vector<OptionSpec> candidates(spec->options.begin(), spec->options.end());
        const auto globals = CliContract::GlobalOptions();
        candidates.insert(candidates.end(), globals.begin(), globals.end());
        if (const auto suggestion = Closest(
                option, candidates, [](const OptionSpec &candidate) { return PreferredOptionName(candidate); })) {
            const auto *suggested = FindOption(candidates, *suggestion);
            if (suggested != nullptr) {
                correction = std::format("try '{}'", OptionInvocation(command, *suggested));
            }
        }
    }
    static_cast<void>(ReportUsageError(
        color, command, std::format("unknown option '{}' for command '{}'", option, command), {}, correction));
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
    std::vector<std::string_view> seenGlobalOptions;
    std::vector<std::string_view> operands;
    bool passthrough = false;

    auto consumeGlobal = [&](std::size_t &index) -> std::optional<int> {
        const auto argument = raw[index];
        const auto spelling = OptionSpelling(argument);
        const auto globals = CliContract::GlobalOptions();
        const auto *option = FindOption(globals, spelling);
        if (option == nullptr)
            return std::nullopt;

        const auto preferred = PreferredOptionName(*option);
        if (preferred == "--help" || preferred == "--version") {
            if (AttachedValue(argument)) {
                return ReportUsageError(options.color, {},
                                        std::format("option '{}' does not accept a value", preferred), {},
                                        std::format("try '{}'", OptionInvocation({}, *option)));
            }
            return std::nullopt;
        }
        if (Contains(seenGlobalOptions, preferred)) {
            return ReportUsageError(options.color, {},
                                    std::format("option '{}' was specified more than once", preferred), {},
                                    std::format("remove the repeated '{}' option", preferred));
        }
        seenGlobalOptions.push_back(preferred);

        auto value = AttachedValue(argument);
        if (OptionTakesValue(*option)) {
            if (!value) {
                if (index + 1 >= raw.size() || raw[index + 1].starts_with('-')) {
                    return ReportUsageError(
                        options.color, {},
                        std::format("option '{}' requires a {}", preferred, ValueDescription(*option)), {},
                        std::format("try '{}'", OptionInvocation({}, *option)));
                }
                value = raw[++index];
            }
            if (value->empty()) {
                return ReportUsageError(options.color, {},
                                        std::format("option '{}' requires a {}", preferred, ValueDescription(*option)),
                                        {}, std::format("try '{}'", OptionInvocation({}, *option)));
            }
        }
        else if (value) {
            return ReportUsageError(options.color, {}, std::format("option '{}' does not accept a value", preferred),
                                    {}, std::format("try '{}'", OptionInvocation({}, *option)));
        }

        if (preferred == "--manifest")
            options.manifest = *value;
        else if (preferred == "--color") {
            if (*value == "auto")
                options.color = ColorMode::Auto;
            else if (*value == "always")
                options.color = ColorMode::On;
            else if (*value == "never")
                options.color = ColorMode::Off;
            else
                return ReportUsageError(
                    options.color, {}, std::format("value '{}' is not valid for option '--color'", *value),
                    "accepted color modes are 'auto', 'always', and 'never'", "try 'rux --color auto'");
        }
        else if (preferred == "--quiet")
            options.quiet = true;
        else if (preferred == "--verbose")
            options.verbose = true;
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
                return ReportUsageError(options.color, {}, "argument separator '--' requires the 'run' command", {},
                                        "try 'rux run -- <arguments>'");
            if (argument.starts_with('-')) {
                PrintUnknownOption(OptionSpelling(argument), {}, options.color);
                return 2;
            }
            command = argument;
            commandSpec = FindCommand(command);
            if (!commandSpec) {
                PrintUnknownCommand(command, options.color);
                return 2;
            }
            continue;
        }

        if (!passthrough && argument == "--") {
            if (!PositionalsFor(command).acceptsPassthrough) {
                return ReportUsageError(
                    options.color, command,
                    std::format("argument separator '--' is not accepted by command '{}'", command));
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
                if (command == "run" && spelling == "--target") {
                    auto requested = AttachedValue(argument);
                    if (!requested && i + 1 < raw.size() && !raw[i + 1].starts_with('-')) {
                        requested = raw[i + 1];
                    }
                    if (requested && !requested->empty()) {
                        return ReportUsageError(
                            options.color, command, std::format("command 'run' cannot execute target '{}'", *requested),
                            std::format("'rux run' builds and executes only the host target '{}'",
                                        Rux::Driver::HostTargetTriple()),
                            std::format(
                                "build it with 'rux build --target {}', then run the output on a compatible host",
                                *requested));
                    }
                    return ReportUsageError(
                        options.color, command, "option '--target' is not available for command 'run'",
                        std::format("'rux run' builds and executes only the host target '{}'",
                                    Rux::Driver::HostTargetTriple()),
                        "use 'rux run' for the host or 'rux build --target linux-x86_64' for a cross build");
                }
                PrintUnknownOption(spelling, command, options.color);
                return 2;
            }
            const auto preferred = PreferredOptionName(*option);
            if (!IsRepeatable(preferred) && Contains(seenOptions, preferred)) {
                return ReportUsageError(options.color, command,
                                        std::format("option '{}' was specified more than once", preferred), {},
                                        std::format("remove the repeated '{}' option", preferred));
            }
            seenOptions.push_back(preferred);
            commandArgs.push_back(spelling);
            auto value = AttachedValue(argument);
            if (OptionTakesValue(*option)) {
                if (!value) {
                    if (i + 1 >= raw.size() || raw[i + 1].starts_with('-')) {
                        return ReportUsageError(
                            options.color, command,
                            std::format("option '{}' requires a {}", preferred, ValueDescription(*option)), {},
                            std::format("try '{}'", OptionInvocation(command, *option)));
                    }
                    value = raw[++i];
                }
                if (value->empty()) {
                    return ReportUsageError(
                        options.color, command,
                        std::format("option '{}' requires a {}", preferred, ValueDescription(*option)), {},
                        std::format("try '{}'", OptionInvocation(command, *option)));
                }
                commandArgs.push_back(*value);
            }
            else if (value) {
                return ReportUsageError(options.color, command,
                                        std::format("option '{}' does not accept a value", preferred), {},
                                        std::format("try '{}'", OptionInvocation(command, *option)));
            }
            continue;
        }
        operands.push_back(argument);
        commandArgs.push_back(argument);
    }

    // `command` and `commandSpec` are set together above, so testing the pointer
    // also covers the no-command case and keeps the uses below provably non-null.
    if (commandSpec == nullptr) {
        PrintHelp(options.color);
        return 0;
    }
    if (options.quiet && options.verbose)
        return ReportUsageError(options.color, command, "options '--quiet' and '--verbose' cannot be used together", {},
                                "use either '--quiet' or '--verbose', but not both");

    const auto &positionals = PositionalsFor(command);
    if (operands.size() < positionals.minimum) {
        const auto argument = positionals.arguments[operands.size()];
        return ReportUsageError(options.color, command,
                                std::format("command '{}' requires a {} argument", command, argument.name), {},
                                std::format("try '{}'", FirstCommandExample(*commandSpec)));
    }
    if (operands.size() > positionals.maximum) {
        return ReportUsageError(
            options.color, command,
            std::format("argument '{}' is not accepted by command '{}'", operands[positionals.maximum], command));
    }
    for (const auto &[left, right] : commandSpec->conflicts) {
        if (Contains(seenOptions, left) && Contains(seenOptions, right)) {
            return ReportUsageError(options.color, command,
                                    std::format("options '{}' and '{}' cannot be used together", left, right), {},
                                    std::format("use either '{}' or '{}', but not both", left, right));
        }
    }
    if (command == "uninstall" && !operands.empty() && Contains(seenOptions, "--global")) {
        return ReportUsageError(
            options.color, command,
            std::format("option '--global' cannot be used with package argument '{}'", operands.front()), {},
            std::format("try 'rux uninstall {}' or 'rux uninstall --global'", operands.front()));
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
