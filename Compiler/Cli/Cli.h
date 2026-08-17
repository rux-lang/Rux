#pragma once

#include <filesystem>
#include <span>
#include <string_view>

namespace Rux {
/// What the user asked for, before the terminal is consulted. `Auto` is the only value that still depends on where
/// output is going; the reporter resolves it once and everything below works from the answer.
enum class ColorMode {
    Auto,
    On,
    Off,
};

/// Options every command accepts, parsed before the subcommand is dispatched.
struct GlobalOptions {
    ColorMode color = ColorMode::Auto;
    bool quiet = false;
    bool verbose = false;
    std::filesystem::path manifest; ///< Custom manifest path; empty = find automatically
};

/**
 * @brief The `rux` command-line front end.
 *
 * Owns argument parsing, user-facing wording, and exit codes; the driver and the reusable compiler components below it
 * own none of those and never print. That split is enforced by a CI guard, not just by convention.
 */
class Cli {
public:
    /// Borrows `argv`, which must outlive this object.
    Cli(int argc, char *argv[]);

    /// Dispatch the subcommand.
    ///
    /// @return The process exit code: 0 on success, non-zero when the command failed or the arguments were rejected
    [[nodiscard]] int Run() const;

private:
    std::span<char *const> args;

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
    static int RunLogin(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunLogout(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunNew(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunPack(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunPublish(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunAdd(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunRemove(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunRun(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunTest(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunUpdate(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunInfo(std::span<const std::string_view> args, const GlobalOptions &opts);
    static int RunCheck(std::span<const std::string_view> args, const GlobalOptions &opts);

    // Help printers
    static void PrintHelp(ColorMode color = ColorMode::Auto);
    static void PrintHelpFor(std::string_view command, ColorMode color = ColorMode::Auto);
    static void PrintHelpJson(std::string_view command = {});
    static void PrintVersion();
    static void PrintUnknownCommand(std::string_view command, ColorMode color = ColorMode::Auto);
    static void PrintUnknownOption(std::string_view option, std::string_view command = {},
                                   ColorMode color = ColorMode::Auto);
};
} // namespace Rux
