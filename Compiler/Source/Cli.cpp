#include "Rux/Cli.h"

#include <span>
#include <string_view>
#include <vector>

namespace Rux {

Cli::Cli(const int argc, char *argv[])
    : args(argv, argc) {
}

// Parse global options from the command line arguments.
GlobalOptions Cli::ParseGlobalOptions(std::span<const std::string_view> args) {
    GlobalOptions opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-q" || arg == "--quiet") {
            opts.quiet = true;
            continue;
        }
        if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
            continue;
        }
        if (arg == "--color") {
            if (i + 1 < args.size()) {
                if (const std::string_view val = args[++i]; val == "on") {
                    opts.color = ColorMode::On;
                }
                else if (val == "off") {
                    opts.color = ColorMode::Off;
                }
                else {
                    opts.color = ColorMode::Auto;
                }
            }
            continue;
        }
        if (arg.starts_with("--color=")) {
            if (const std::string_view val = arg.substr(8); val == "on") {
                opts.color = ColorMode::On;
            }
            else if (val == "off") {
                opts.color = ColorMode::Off;
            }
            else {
                opts.color = ColorMode::Auto;
            }
        }
    }
    return opts;
}

int Cli::Run() const {
    // Collect all arguments as string_views (skip argv[0])
    std::vector<std::string_view> sv;
    sv.reserve(static_cast<std::size_t>(args.size()));
    for (auto *a : args.subspan(1)) {
        sv.emplace_back(a);
    }

    if (sv.empty()) {
        PrintHelp();
        return 0;
    }

    // Walk through arguments collecting global flags and finding the command.
    // Global flags may appear before OR after the command name.
    std::string_view command;
    bool foundCommand = false;
    std::vector<std::string_view> preCommandGlobals;
    std::vector<std::string_view> cmdArgs;

    for (std::size_t i = 0; i < sv.size(); ++i) {
        std::string_view arg = sv[i];

        if (!foundCommand) {
            if (arg == "-h" || arg == "--help") {
                PrintHelp();
                return 0;
            }
            if (arg == "-V" || arg == "--version") {
                PrintVersion();
                return 0;
            }
            if (arg == "-q" || arg == "--quiet" || arg == "-v" || arg == "--verbose") {
                preCommandGlobals.push_back(arg);
                continue;
            }
            if (arg == "--color") {
                preCommandGlobals.push_back(arg);
                if (i + 1 < sv.size()) {
                    preCommandGlobals.push_back(sv[++i]);
                }
                continue;
            }
            if (arg.starts_with("--color=")) {
                preCommandGlobals.push_back(arg);
                continue;
            }
            command = arg;
            foundCommand = true;
        }
        else {
            cmdArgs.push_back(arg);
        }
    }

    if (!foundCommand) {
        PrintHelp();
        return 0;
    }

    // Merge pre-command globals with command args for option parsing
    std::vector<std::string_view> allArgs;
    allArgs.insert(allArgs.end(), preCommandGlobals.begin(), preCommandGlobals.end());
    allArgs.insert(allArgs.end(), cmdArgs.begin(), cmdArgs.end());

    GlobalOptions opts = ParseGlobalOptions(allArgs);
    std::span<const std::string_view> rest(cmdArgs);

    if (command == "help") {
        return RunHelp(rest, opts);
    }
    if (command == "version") {
        return RunVersion(opts);
    }
    if (command == "build") {
        return RunBuild(rest, opts);
    }
    if (command == "clean") {
        return RunClean(rest, opts);
    }
    if (command == "doc") {
        return RunDoc(rest, opts);
    }
    if (command == "fmt") {
        return RunFmt(rest, opts);
    }
    if (command == "init") {
        return RunInit(rest, opts);
    }
    if (command == "install") {
        return RunInstall(rest, opts);
    }
    if (command == "uninstall") {
        return RunUninstall(rest, opts);
    }
    if (command == "list") {
        return RunList(rest, opts);
    }
    if (command == "new") {
        return RunNew(rest, opts);
    }
    if (command == "add") {
        return RunAdd(rest, opts);
    }
    if (command == "remove") {
        return RunRemove(rest, opts);
    }
    if (command == "run") {
        return RunRun(rest, opts);
    }
    if (command == "test") {
        return RunTest(rest, opts);
    }
    if (command == "update") {
        return RunUpdate(rest, opts);
    }
    if (command == "info") {
        return RunInfo(rest, opts);
    }
    if (command == "check") {
        return RunCheck(rest, opts);
    }

    PrintUnknownCommand(command);
    return 1;
}

} // namespace Rux
