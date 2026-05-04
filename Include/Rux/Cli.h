/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include <span>
#include <string_view>

namespace Rux
{
    enum class ColorMode { Auto, On, Off };

    struct GlobalOptions
    {
        ColorMode color = ColorMode::Auto;
        bool quiet = false;
        bool verbose = false;
    };

    class Cli
    {
    public:
        Cli(int argc, char* argv[]);
        [[nodiscard]] int Run() const;

    private:
        std::span<char* const> args;

        // Argument parsing
        static GlobalOptions ParseGlobalOptions(std::span<const std::string_view> args);

        // Command dispatch
        static int RunHelp(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunVersion(const GlobalOptions& opts);
        static int RunBuild(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunClean(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunDoc(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunFmt(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunInit(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunInstall(const GlobalOptions& opts);
        static int RunNew(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunAdd(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunRemove(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunRun(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunTest(std::span<const std::string_view> args, const GlobalOptions& opts);
        static int RunUp(std::span<const std::string_view> args, const GlobalOptions& opts);

        // Help printers
        static void PrintHelp();
        static void PrintHelpFor(std::string_view command);
        static void PrintHelpAdd();
        static void PrintHelpBuild();
        static void PrintHelpClean();
        static void PrintHelpDoc();
        static void PrintHelpFmt();
        static void PrintHelpInit();
        static void PrintHelpInstall();
        static void PrintHelpNew();
        static void PrintHelpRemove();
        static void PrintHelpRun();
        static void PrintHelpTest();
        static void PrintHelpUp();
        static void PrintHelpVersion();
        static void PrintVersion();
        static void PrintUnknownCommand(std::string_view command);
        static void PrintUnknownOption(std::string_view option, std::string_view command = {});
    };
}
