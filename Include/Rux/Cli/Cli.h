// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <span>
#include <string_view>

namespace Rux {
enum class ColorMode {
    Auto,
    On,
    Off,
};

struct GlobalOptions {
    ColorMode color = ColorMode::Auto;
    bool quiet = false;
    bool verbose = false;
};

class Cli {
public:
    Cli(int argc, char *argv[]);
    [[nodiscard]] int Run() const;

private:
    std::span<char *const> args;

    // Argument parsing
    static GlobalOptions ParseGlobalOptions(std::span<std::string_view const> args);

    // Command dispatch
    static int RunHelp(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunVersion(GlobalOptions const &opts);
    static int RunBuild(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunClean(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunDoc(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunFmt(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunInit(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunInstall(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunUninstall(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunList(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunNew(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunAdd(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunRemove(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunRun(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunTest(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunUpdate(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunInfo(std::span<std::string_view const> args, GlobalOptions const &opts);
    static int RunCheck(std::span<std::string_view const> args, GlobalOptions const &opts);

    // Help printers
    static void PrintHelp();
    static void PrintHelpFor(std::string_view command);
    static void PrintVersion();
    static void PrintUnknownCommand(std::string_view command);
    static void PrintUnknownOption(std::string_view option, std::string_view command = {});
};
} // namespace Rux
