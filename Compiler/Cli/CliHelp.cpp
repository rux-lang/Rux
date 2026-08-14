// CLI help/version composition over the pure contract renderer.

#include "BuildInfo/CompilerMetadata.h"
#include "Cli/Cli.h"
#include "Cli/CliHelpRenderer.h"
#include "Cli/CliSpec.h"
#include "Cli/TerminalStyle.h"
#include "System/Os.h"

#include <cstdio>
#include <print>
#include <span>
#include <string_view>

using namespace Rux;
using namespace Rux::CliSupport;

namespace {
std::size_t TerminalWidth() {
    return CliHelp::NormalizeTerminalWidth(System::TerminalWidth());
}
} // namespace

void Cli::PrintHelp(const ColorMode color) {
    std::print("{}", CliHelp::RenderGeneral(TerminalWidth(), ColorEnabled(color, OutputStream::Stdout)));
}

void Cli::PrintHelpFor(const std::string_view command, const ColorMode color) {
    if (command == "help") {
        PrintHelp(color);
        return;
    }
    if (const auto *spec = CliContract::FindCommand(command)) {
        std::print("{}", CliHelp::RenderCommand(*spec, TerminalWidth(), ColorEnabled(color, OutputStream::Stdout)));
        return;
    }
    PrintUnknownCommand(command);
}

void Cli::PrintVersion() {
    std::println("Rux {} ({} {})", CompilerBuild::compilerVersion, CompilerBuild::compilerBuildDate,
                 CompilerBuild::compilerBuildTime);
}

void Cli::PrintHelpJson(const std::string_view command) {
    std::print("{}", CliHelp::RenderJson(CompilerBuild::compilerVersion, command));
}

int Cli::RunHelp(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool json = false;
    std::string_view command;
    for (const auto arg : args) {
        if (arg == "--json") {
            json = true;
        }
        else if (command.empty()) {
            command = arg;
        }
        else {
            std::println(stderr, "error: too many arguments for command 'help'");
            return 2;
        }
    }
    if (!command.empty() && !CliContract::FindCommand(command)) {
        PrintUnknownCommand(command);
        return 2;
    }
    if (json) {
        PrintHelpJson(command);
    }
    else if (!command.empty()) {
        PrintHelpFor(command, opts.color);
    }
    else {
        PrintHelp(opts.color);
    }
    return 0;
}

int Cli::RunVersion(const GlobalOptions &) {
    PrintVersion();
    return 0;
}
