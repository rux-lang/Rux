// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#include "Rux/Cli/Cli.h"

#include "Rux/Asm.h"
#include "Rux/Ast.h"
#include "Rux/Cli/CliInternals.h"
#include "Rux/Hir.h"
#include "Rux/Lexer.h"
#include "Rux/Linker.h"
#include "Rux/Lir.h"
#include "Rux/Manifest.h"
#include "Rux/Package.h"
#include "Rux/Parser.h"
#include "Rux/Platform/Defines.h"
#include "Rux/Platform/Host.h"
#include "Rux/Rcu.h"
#include "Rux/Sema.h"
#include "Rux/Version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iomanip>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * This is separate from the other ifdef because otherwise clang-format attempts
 * to change the order, which makes MSVC cry.
 */

#if RUX_OS_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <windows.h>
#endif

#if RUX_OS_WINDOWS
    #include <psapi.h>
    #include <winhttp.h>
#else
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include "Rux/SourceLoader.h"

using namespace Rux;
using namespace Platform;
using namespace Misc;

Cli::Cli(int const argc, char *argv[])
    : args(argv, argc) {
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
    std::span<std::string_view const> rest(cmdArgs);

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

// you can see the implementation in .\Include\Rux\Cli\CliInternals.h
// all previous functions here are in Rux::Misc
