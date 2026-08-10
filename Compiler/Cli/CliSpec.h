#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace Rux::CliContract {
struct OptionSpec {
    std::string_view flags;
    std::string_view desc;
};

struct CommandSpec {
    std::string_view name;
    std::string_view shortDesc;
    std::string_view description;
    std::span<const std::string_view> usage;
    std::string_view postUsage;
    std::string_view footer;
    std::span<const std::string_view> examples;
    std::span<const OptionSpec> options;
};

struct ArgumentSpec {
    std::string_view name;
    std::string_view description;
    bool required = false;
    bool multiple = false;
};

struct PositionalSpec {
    std::string_view command;
    std::span<const ArgumentSpec> arguments;
    std::size_t minimum = 0;
    std::size_t maximum = 0;
    bool acceptsPassthrough = false;
};

[[nodiscard]] std::span<const OptionSpec> GlobalOptions();
[[nodiscard]] std::span<const CommandSpec> Commands();
[[nodiscard]] const CommandSpec *FindCommand(std::string_view name);
[[nodiscard]] const PositionalSpec &PositionalsFor(std::string_view command);
[[nodiscard]] bool OptionMatches(const OptionSpec &option, std::string_view spelling);
[[nodiscard]] bool OptionTakesValue(const OptionSpec &option);
[[nodiscard]] std::string_view PreferredOptionName(const OptionSpec &option);
} // namespace Rux::CliContract
