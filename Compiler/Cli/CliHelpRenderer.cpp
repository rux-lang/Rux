// Pure text, ANSI, and JSON rendering for the immutable CLI contract.

#include "Cli/CliHelpRenderer.h"

#include "Diagnostics/Diagnostics.h"
#include "Reporting/Reporting.h"

#include <algorithm>
#include <concepts>
#include <format>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>

using namespace Rux::CliContract;
using namespace std::string_view_literals;

namespace {
template <typename T>
concept Sizable = requires(const T &value) {
    { std::size(value) } -> std::convertible_to<std::size_t>;
};

template <std::ranges::input_range Range, typename Projection = std::identity>
    requires Sizable<std::indirect_result_t<Projection, std::ranges::iterator_t<Range>>>
/// The widest element in a range, which is how each help block sizes its own label column. Widths are per block, not
/// shared, so one long flag never pads an unrelated section.
std::size_t MaxSize(const Range &range, Projection projection = {}) {
    std::size_t result = 0;
    for (const auto &item : range) {
        result = std::max(result, static_cast<std::size_t>(std::size(std::invoke(projection, item))));
    }
    return result;
}

namespace Layout {
constexpr std::size_t defaultWidth = 80;
constexpr std::size_t minimumTerminalWidth = 48;
constexpr std::size_t minimumDescriptionWidth = 24;
constexpr std::size_t blockIndent = 4;
constexpr std::size_t alignedPadding = 2;
constexpr auto whitespace = " \t"sv;
constexpr auto cliName = "rux"sv;
} // namespace Layout

using Rux::Reporting::Style;

std::size_t UsableWidth(const std::size_t terminalWidth, const std::size_t indent) {
    if (terminalWidth <= indent) {
        return Layout::minimumDescriptionWidth;
    }
    return std::max(terminalWidth - indent, Layout::minimumDescriptionWidth);
}

template <typename Callback>
void ProcessLine(std::string_view line, const std::size_t width, Callback callback) {
    while (!line.empty()) {
        const auto firstNonSpace = line.find_first_not_of(Layout::whitespace);
        if (firstNonSpace == std::string_view::npos) {
            break;
        }
        line.remove_prefix(firstNonSpace);
        std::size_t cutLength = line.size();
        if (width < line.size()) {
            cutLength = line.find_last_of(Layout::whitespace, width);
            if (cutLength == std::string_view::npos) {
                cutLength = line.find_first_of(Layout::whitespace, width);
                if (cutLength == std::string_view::npos) {
                    cutLength = line.size();
                }
            }
        }
        std::string_view piece = line.substr(0, cutLength);
        const auto lastNonSpace = piece.find_last_not_of(Layout::whitespace);
        piece.remove_suffix(piece.size() - lastNonSpace - 1);
        callback(piece);
        line.remove_prefix(cutLength);
    }
}

template <typename Callback>
void Wrap(std::string_view text, const std::size_t width, Callback callback) {
    while (!text.empty()) {
        const auto newLine = text.find('\n');
        if (const std::string_view line = text.substr(0, newLine); line.empty()) {
            callback(""sv);
        }
        else {
            ProcessLine(line, width, callback);
        }
        if (newLine == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newLine + 1);
    }
}

void AppendCommandLine(std::string &output, const std::string_view command, const std::string_view suffix,
                       const Style &style) {
    output += std::string(Layout::blockIndent, ' ');
    output += style.Cyan();
    output += style.Bold();
    if (suffix.starts_with(Layout::cliName)) {
        output += suffix;
    }
    else {
        output += Layout::cliName;
        if (!command.empty()) {
            output += std::format(" {}", command);
        }
        if (!suffix.empty()) {
            output += std::format(" {}", suffix);
        }
    }
    output += style.Reset();
    output += '\n';
}

void AppendBlock(std::string &output, const std::string_view text, const std::size_t terminalWidth,
                 const std::size_t indent = Layout::blockIndent) {
    if (text.empty()) {
        return;
    }
    Wrap(text, UsableWidth(terminalWidth, indent), [&](const std::string_view line) {
        output += std::string(line.empty() ? 0 : indent, ' ');
        output += line;
        output += '\n';
    });
    output += '\n';
}

void AppendAligned(std::string &output, const std::string_view left, const std::string_view right,
                   const std::size_t leftWidth, const std::size_t terminalWidth, const Style &style) {
    const std::size_t indent = Layout::blockIndent + leftWidth + Layout::alignedPadding;
    bool first = true;
    Wrap(right, UsableWidth(terminalWidth, indent), [&](const std::string_view line) {
        if (first) {
            std::string leftColumn(left);
            leftColumn.resize(leftWidth + Layout::alignedPadding, ' ');
            output += std::string(Layout::blockIndent, ' ');
            output += style.Cyan();
            output += leftColumn;
            output += style.Reset();
            output += line;
            output += '\n';
            first = false;
        }
        else {
            output += std::string(line.empty() ? 0 : indent, ' ');
            output += line;
            output += '\n';
        }
    });
    if (first) {
        output += std::string(Layout::blockIndent, ' ');
        output += style.Cyan();
        output += left;
        output += style.Reset();
        output += '\n';
    }
}

void AppendJsonString(std::string &output, const std::string_view value) {
    output += std::format("\"{}\"", Rux::EscapeJson(value));
}

void AppendOptionJson(std::string &output, const OptionSpec &option) {
    output += "{\"flags\":";
    AppendJsonString(output, option.flags);
    output += ",\"description\":";
    AppendJsonString(output, option.desc);
    output += std::format(",\"takesValue\":{},\"repeatable\":{}}}", OptionTakesValue(option),
                          option.flags.starts_with("--define") || option.flags.starts_with("--emit"));
}

void AppendCommandJson(std::string &output, const CommandSpec &command) {
    const auto &positionals = PositionalsFor(command.name);
    output += "{\"name\":";
    AppendJsonString(output, command.name);
    output += ",\"summary\":";
    AppendJsonString(output, command.shortDesc);
    output += ",\"description\":";
    AppendJsonString(output, command.description.empty() ? command.shortDesc : command.description);
    output += ",\"usages\":[";
    for (std::size_t i = 0; i < command.usage.size(); ++i) {
        if (i != 0) {
            output += ',';
        }
        AppendJsonString(
            output, std::format("rux {}{}{}", command.name, command.usage[i].empty() ? "" : " ", command.usage[i]));
    }
    if (command.usage.empty()) {
        AppendJsonString(output, std::format("rux {}", command.name));
    }
    output += "],\"arguments\":[";
    for (std::size_t i = 0; i < positionals.arguments.size(); ++i) {
        if (i != 0) {
            output += ',';
        }
        const auto &argument = positionals.arguments[i];
        output += "{\"name\":";
        AppendJsonString(output, argument.name);
        output += ",\"description\":";
        AppendJsonString(output, argument.description);
        output += std::format(",\"required\":{},\"multiple\":{}}}", argument.required, argument.multiple);
    }
    output += "],\"options\":[";
    for (std::size_t i = 0; i < command.options.size(); ++i) {
        if (i != 0) {
            output += ',';
        }
        AppendOptionJson(output, command.options[i]);
    }
    output += "],\"examples\":[";
    for (std::size_t i = 0; i < command.examples.size(); ++i) {
        if (i != 0) {
            output += ',';
        }
        const auto example = command.examples[i];
        AppendJsonString(output, example.starts_with("rux")
                                     ? example
                                     : std::format("rux {}{}{}", command.name, example.empty() ? "" : " ", example));
    }
    output += "],\"documentationUrl\":";
    AppendJsonString(output, DocumentationUrl(command.name));
    output += '}';
}
} // namespace

namespace Rux::CliHelp {
std::size_t NormalizeTerminalWidth(const std::size_t width) {
    return width == 0 ? Layout::defaultWidth : std::max(width, Layout::minimumTerminalWidth);
}

std::string RenderGeneral(const std::size_t terminalWidth, const bool color) {
    const auto width = NormalizeTerminalWidth(terminalWidth);
    const Style style{color};
    std::string output =
        std::format("The {}{}Rux{} compiler and package manager\n\n", style.Bold(), style.Cyan(), style.Reset());
    output += std::format("{}Usage:{} {}{}{} [global-options] <command> [command-options] [operands]\n\n", style.Bold(),
                          style.Reset(), style.Cyan(), Layout::cliName, style.Reset());
    output += std::format("{}Commands:{}\n", style.Bold(), style.Reset());
    const auto commands = Commands();
    const auto commandWidth = MaxSize(commands, &CommandSpec::name);
    for (const auto &command : commands) {
        AppendAligned(output, command.name, command.shortDesc, commandWidth, width, style);
    }
    output += std::format("\n{}Global options:{}\n", style.Bold(), style.Reset());
    const auto options = GlobalOptions();
    const auto optionWidth = MaxSize(options, &OptionSpec::flags);
    for (const auto &option : options) {
        AppendAligned(output, option.flags, option.desc, optionWidth, width, style);
    }
    output += std::format("\nUse '{}rux help <command>{}' for more information about a command.\n", style.Cyan(),
                          style.Reset());
    return output;
}

std::string RenderCommand(const CommandSpec &command, const std::size_t terminalWidth, const bool color) {
    const auto width = NormalizeTerminalWidth(terminalWidth);
    const Style style{color};
    std::string output;
    Wrap(command.description.empty() ? command.shortDesc : command.description, width,
         [&](const std::string_view line) { output += std::format("{}\n", line); });
    output += std::format("\n{}Usage:{}\n", style.Bold(), style.Reset());
    if (command.usage.empty()) {
        AppendCommandLine(output, command.name, ""sv, style);
    }
    else {
        for (const auto usage : command.usage) {
            AppendCommandLine(output, command.name, usage, style);
        }
    }
    output += '\n';
    AppendBlock(output, command.postUsage, width);
    if (!command.options.empty()) {
        output += std::format("{}Options:{}\n", style.Bold(), style.Reset());
        const auto optionWidth = MaxSize(command.options, &OptionSpec::flags);
        for (const auto &option : command.options) {
            AppendAligned(output, option.flags, option.desc, optionWidth, width, style);
        }
        output += '\n';
    }
    AppendBlock(output, command.footer, width, 0);
    if (!command.examples.empty()) {
        output += std::format("{}Examples:{}\n", style.Bold(), style.Reset());
        for (const auto example : command.examples) {
            AppendCommandLine(output, command.name, example, style);
        }
        output += '\n';
    }
    output += std::format("{}Documentation:{}\n", style.Bold(), style.Reset());
    output += std::format("    {}{}{}\n", style.Cyan(), DocumentationUrl(command.name), style.Reset());
    return output;
}

std::string RenderJson(const std::string_view version, const std::string_view command) {
    std::string output = "{\"schemaVersion\":1,\"program\":{\"name\":\"rux\",\"version\":";
    AppendJsonString(output, version);
    output += "},\"globalOptions\":[";
    const auto globals = GlobalOptions();
    for (std::size_t i = 0; i < globals.size(); ++i) {
        if (i != 0) {
            output += ',';
        }
        AppendOptionJson(output, globals[i]);
    }
    output += "],\"commands\":[";
    if (command.empty()) {
        const auto commands = Commands();
        for (std::size_t i = 0; i < commands.size(); ++i) {
            if (i != 0) {
                output += ',';
            }
            AppendCommandJson(output, commands[i]);
        }
    }
    else if (const auto *spec = FindCommand(command)) {
        AppendCommandJson(output, *spec);
    }
    output += "]}\n";
    return output;
}
} // namespace Rux::CliHelp
