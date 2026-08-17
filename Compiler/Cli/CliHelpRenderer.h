#pragma once

#include "Cli/CliSpec.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace Rux::CliHelp {
/// Clamp a measured terminal width into the range help text is laid out for, so an unset or absurd width still produces
/// a readable page.
[[nodiscard]] std::size_t NormalizeTerminalWidth(std::size_t width);

/// Render the top-level `rux help` page listing every command.
///
/// @param color Resolved policy from the caller; this component detects nothing about the terminal itself
[[nodiscard]] std::string RenderGeneral(std::size_t terminalWidth, bool color);

/// Render the help page for one command.
[[nodiscard]] std::string RenderCommand(const CliContract::CommandSpec &command, std::size_t terminalWidth, bool color);

/// Render the whole command surface as JSON, for tooling that needs to read the CLI contract rather than display it.
///
/// @param command Limits the output to one command; empty describes them all
[[nodiscard]] std::string RenderJson(std::string_view version, std::string_view command = {});
} // namespace Rux::CliHelp
