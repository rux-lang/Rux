#pragma once

#include "Cli/CliSpec.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace Rux::CliHelp {
[[nodiscard]] std::size_t NormalizeTerminalWidth(std::size_t width);
[[nodiscard]] std::string RenderGeneral(std::size_t terminalWidth, bool color);
[[nodiscard]] std::string RenderCommand(const CliContract::CommandSpec &command, std::size_t terminalWidth, bool color);
[[nodiscard]] std::string RenderJson(std::string_view version, std::string_view command = {});
} // namespace Rux::CliHelp
