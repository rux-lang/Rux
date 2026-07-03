#pragma once

#include <filesystem>
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
    std::filesystem::path manifest; // Custom manifest path; empty = find automatically
};

class Cli {
public:
    Cli(int argc, char *argv[]);
    [[nodiscard]] int Run() const;

private:
    std::span<char *const> args;

    // Argument parsing
    static GlobalOptions ParseGlobalOptions(std::span<const std::string_view> args);

    // Command dispatch
    static int RunHelp(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunVersion(const GlobalOptions &opts);
    static int RunBuild(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunClean(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunDoc(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunFmt(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunLint(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunInit(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunInstall(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunUninstall(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunList(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunNew(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunAdd(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunRemove(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunRun(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunTest(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunUpdate(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunInfo(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunCheck(std::span<const std::string_view> args, const GlobalOptions &opts);

    // Help printers
    static void PrintHelp();
    static void PrintHelpFor(std::string_view command);
    static void PrintVersion();
    static void PrintUnknownCommand(std::string_view command);
    static void PrintUnknownOption(std::string_view option, std::string_view command = {});
};
} // namespace Rux
