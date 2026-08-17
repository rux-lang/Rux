#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace Rux::CliContract {
/// One option, as both the argument parser and the help renderer see it. Holding a single declaration means a flag
/// cannot be accepted but undocumented, or documented but unaccepted.
struct OptionSpec {
    /// Every accepted spelling, comma-separated and shortest first, with a `<value>` placeholder on any option that
    /// takes one: `-r, --release` or `--target <triple>`. The placeholder is what `OptionTakesValue` reads.
    std::string_view flags;

    /// One-line help text, sentence-cased and without a trailing period.
    std::string_view desc;
};

/// Two options that cannot be given together. The parser reports the pair rather than whichever it happened to see
/// second.
struct OptionConflict {
    std::string_view left;
    std::string_view right;
};

/// The full declaration of one `rux` subcommand: what it accepts and everything `rux help` prints about it.
struct CommandSpec {
    std::string_view name;
    std::string_view shortDesc;
    std::string_view description;
    std::span<const std::string_view> usage;
    std::string_view postUsage;
    std::string_view footer;
    std::span<const std::string_view> examples;
    std::span<const OptionSpec> options;
    std::span<const OptionConflict> conflicts;
};

/// One positional argument of a command, named so diagnostics and help can refer to it the same way.
struct ArgumentSpec {
    std::string_view name;
    std::string_view description;
    bool required = false;
    bool multiple = false;
};

/// How many positional arguments one command takes, and what they mean.
struct PositionalSpec {
    std::string_view command;
    std::span<const ArgumentSpec> arguments;
    std::size_t minimum = 0;
    std::size_t maximum = 0;

    /// Whether everything after `--` is forwarded to the program being run rather than parsed as this command's own
    /// arguments.
    bool acceptsPassthrough = false;
};

/// Options every command accepts, declared once instead of repeated per command.
[[nodiscard]] std::span<const OptionSpec> GlobalOptions();

/// Every declared subcommand, in the order `rux help` lists them.
[[nodiscard]] std::span<const CommandSpec> Commands();

/// @return The declaration of `name`, or nullptr when no such command exists
[[nodiscard]] const CommandSpec *FindCommand(std::string_view name);

/// The positional contract for `command`. Returns an empty spec for a command that takes none, so callers need no
/// special case.
[[nodiscard]] const PositionalSpec &PositionalsFor(std::string_view command);

/// Whether `spelling` names this option. Accepts any of the comma-separated spellings, and tolerates an attached
/// `--flag=value`, so matching can happen before the value is split off.
[[nodiscard]] bool OptionMatches(const OptionSpec &option, std::string_view spelling);

/// Whether the option carries a value, which the `<placeholder>` in `flags` is what declares.
[[nodiscard]] bool OptionTakesValue(const OptionSpec &option);

/// The spelling to name this option by in a diagnostic: the long form when there is one, otherwise the first listed.
[[nodiscard]] std::string_view PreferredOptionName(const OptionSpec &option);

/// Where to send a reader for more about `command`. Commands without a dedicated page fall back to the CLI index, so
/// this always returns something usable.
[[nodiscard]] std::string_view DocumentationUrl(std::string_view command);
} // namespace Rux::CliContract
