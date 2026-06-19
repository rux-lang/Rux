// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#include "Rux/Cli/Cli.h"
#include "Rux/Cli/CliInternals.h"
#include "Rux/Hir.h"
#include "Rux/Manifest.h"
#include "Rux/Package.h"
#include "Rux/Platform/Defines.h"
#include "Rux/Platform/Host.h"
#include "Rux/Version.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>

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
#else
    #include <fcntl.h>
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace Rux;
using namespace Platform;
using namespace Misc;

int Cli::RunAdd(std::span<std::string_view const> args, GlobalOptions const &opts) {
    std::string_view spec;
    std::string_view pathArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("add");
            return 0;
        }
        if (arg == "--path") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--path' requires an argument\n");
                return 1;
            }
            pathArg = args[++i];
            continue;
        }
        if (!arg.starts_with('-') && spec.empty()) {
            spec = arg;
            continue;
        }
        PrintUnknownOption(arg, "add");
        return 1;
    }
    if (spec.empty()) {
        std::print(stderr, "error: missing package name\n\n");
        PrintHelpFor("add");
        return 1;
    }
    auto manifestPath = RequireManifest();
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    auto [pkgName, pkgVersion] = ParsePackageSpec(spec);

    if (!pathArg.empty()) {
        bool const changed = manifest->AddPathDependency(pkgName, std::string(pathArg));
        if (!manifest->Save(*manifestPath)) {
            std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
            return 1;
        }
        if (!opts.quiet) {
            if (changed) {
                std::print("Added {} @ path '{}'\n", pkgName, pathArg);
            }
            else {
                std::print("Up-to-date {} @ path '{}'\n", pkgName, pathArg);
            }
        }
        return 0;
    }

    if (!opts.quiet) {
        std::print("     Fetching registry...\n");
    }

    auto const jsonOpt = FetchUrl(std::string(kRegistryUrl));
    if (!jsonOpt) {
        std::print(stderr, "error: failed to fetch package registry\n");
        return 1;
    }

    if (JsonLookupString(*jsonOpt, pkgName).empty()) {
        std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
        return 1;
    }

    bool const changed = manifest->AddDependency(pkgName, pkgVersion);
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        std::string const ver = pkgVersion.empty() ? "latest" : pkgVersion;
        if (changed) {
            std::print("Added {} @ {}\n", pkgName, ver);
        }
        else {
            std::print("Up-to-date {} @ {}\n", pkgName, ver);
        }
    }
    return 0;
}

int Cli::RunRemove(std::span<std::string_view const> args, GlobalOptions const &opts) {
    std::string_view name;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("remove");
            return 0;
        }
        if (!arg.starts_with('-') && name.empty()) {
            name = arg;
            continue;
        }
        PrintUnknownOption(arg, "remove");
        return 1;
    }
    if (name.empty()) {
        std::print(stderr, "error: missing package name\n\n");
        PrintHelpFor("remove");
        return 1;
    }
    auto manifestPath = RequireManifest();
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    std::string pkgName(name);
    if (!manifest->RemoveDependency(pkgName)) {
        std::print(stderr, "error: package '{}' is not a dependency\n", pkgName);
        return 1;
    }
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        std::print("     Removed {}\n", pkgName);
    }
    return 0;
}

int Cli::RunTest(std::span<std::string_view const> args, GlobalOptions const &opts) {
    bool isRelease = false;
    for (auto &arg : args) {
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("test");
            return 0;
        }
        PrintUnknownOption(arg, "test");
        return 1;
    }
    auto manifestPath = Manifest::Find();
    std::filesystem::path projectRoot;
    std::filesystem::path testsDir;

    if (manifestPath) {
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            return 1;
        }
        if (!opts.quiet) {
            std::print("     Testing {} v{}\n", manifest->package.name, manifest->package.version);
        }
        projectRoot = manifestPath->parent_path();
        testsDir = projectRoot / "Tests";
    }
    else {
        projectRoot = std::filesystem::current_path();
        testsDir = projectRoot / "Tests";
        std::error_code ec;
        if (!std::filesystem::exists(testsDir, ec)) {
            RequireManifest(); // Prints standard "Rux.toml not found" error
            return 1;
        }
        if (!opts.quiet) {
            std::print("     Running workspace tests\n");
        }
    }

    std::string_view const profileName = isRelease ? "Release" : "Debug";

    // Collect test package directories: any subdirectory of Tests/ that
    // contains a Rux.toml with Type = "bin".
    std::vector<std::filesystem::path> testPackages;
    {
        std::error_code ec;
        if (!std::filesystem::exists(testsDir, ec)) {
            if (!opts.quiet) {
                std::print("  No Tests/ directory found — nothing to run.\n");
            }
            return 0;
        }
        for (auto const &entry : std::filesystem::directory_iterator(testsDir, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            auto const toml = entry.path() / "Rux.toml";
            if (!std::filesystem::exists(toml)) {
                continue;
            }
            auto pkgManifest = Manifest::Load(toml);
            if (!pkgManifest) {
                continue;
            }
            // Only run binary packages (not DLLs / shared libraries).
            auto const &type = pkgManifest->package.type;
            if (type != "bin" && type != "Bin") {
                continue;
            }
            testPackages.push_back(entry.path());
        }
        std::sort(testPackages.begin(), testPackages.end());
    }

    if (testPackages.empty()) {
        if (!opts.quiet) {
            std::print("  No test packages found in Tests/.\n");
        }
        return 0;
    }

    // Helper: run rux build inside a package directory, then execute the
    // resulting binary. Returns the process exit code, or -1 on build/launch
    // failure.
    auto runOne = [&](std::filesystem::path const &pkgDir) -> int {
        // Load the package manifest to derive the executable name and output path.
        auto pkgManifest = Manifest::Load(pkgDir / "Rux.toml");
        if (!pkgManifest) {
            std::print(stderr, "error: failed to parse '{}'\n", (pkgDir / "Rux.toml").string());
            return -1;
        }

        // Build: temporarily change the working directory into the package root
        // so that RequireManifest() and source paths resolve correctly.
        auto const savedCwd = std::filesystem::current_path();
        std::error_code ec;
        std::filesystem::current_path(pkgDir, ec);
        if (ec) {
            std::print(stderr, "error: cannot chdir into '{}': {}\n", pkgDir.string(),
                       ec.message());
            return -1;
        }

        GlobalOptions buildOpts = opts;
        buildOpts.quiet = true; // suppress per-file build output for tests
        std::vector<std::string_view> buildArgs;
        if (isRelease) {
            buildArgs.emplace_back("--release");
        }
        buildArgs.emplace_back("--quiet");

        int const buildRc = RunBuild(buildArgs, buildOpts);
        std::filesystem::current_path(savedCwd, ec); // always restore CWD

        if (buildRc != 0) {
            std::print(stderr, "error: build failed for test package '{}'\n",
                       pkgDir.filename().string());
            return -1;
        }

        // Locate the built executable.
        auto const binDir = ResolveBuildOutputDir(pkgDir, *pkgManifest, profileName);
        std::string exeName = pkgManifest->package.name;
#if RUX_OS_WINDOWS
        exeName += ".exe";
#endif
        auto const exePath = binDir / exeName;

        if (!std::filesystem::exists(exePath)) {
            std::print(stderr, "error: built executable not found at '{}'\n", exePath.string());
            return -1;
        }

        if (opts.verbose) std::print("     Running `{}`\n", exePath.string());

        // Execute the test binary and capture its exit code.
#if RUX_OS_WINDOWS
        HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        std::string cmdLine = "\"" + exePath.string() + "\"";
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.hStdInput = hNul != INVALID_HANDLE_VALUE ? hNul : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.dwFlags = STARTF_USESTDHANDLES;
        if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                            &si, &pi)) {
            std::print(stderr, "error: failed to launch '{}' (code {})\n", exePath.string(),
                       GetLastError());
            if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
            return -1;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        return static_cast<int>(exitCode);
#else
        std::string const exeStr = exePath.string();
        char const *argv[] = {exeStr.c_str(), nullptr};
        pid_t const pid = fork();
        if (pid < 0) {
            std::print(stderr, "error: fork failed\n");
            return -1;
        }
        if (pid == 0) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd >= 0) {
                dup2(fd, 0);
                close(fd);
            }
            execv(exeStr.c_str(), const_cast<char *const *>(argv));
            std::print(stderr, "error: failed to launch '{}'\n", exeStr);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    };

    // Run every discovered test package and tally results.
    int passed = 0;
    int failed = 0;

    for (auto const &pkgDir : testPackages) {
        std::string const label = pkgDir.filename().string();
        if (!opts.quiet) {
            std::print("      Running test package: {}\n", label);
        }

        int const rc = runOne(pkgDir);
        if (rc == 0) {
            ++passed;
            if (!opts.quiet) {
                std::print("    PASS: {}\n", label);
            }
        }
        else {
            ++failed;
            std::print(stderr, "    FAIL: {} (exit {})\n", label,
                       rc == -1 ? std::string("build/launch error") : std::to_string(rc));
        }
    }

    // Summary line.
    int const total = passed + failed;
    if (!opts.quiet || failed > 0) {
        std::print("{}: {} passed, {} failed, {} total\n", failed == 0 ? "ok" : "FAILED", passed,
                   failed, total);
    }
    return failed == 0 ? 0 : 1;
}

int Cli::RunInit(std::span<std::string_view const> args, GlobalOptions const &opts) {
    bool bin = false;
    bool lib = false;
    for (auto &arg : args) {
        if (arg == "--bin") {
            bin = true;
            continue;
        }
        if (arg == "--lib") {
            lib = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("init");
            return 0;
        }
        PrintUnknownOption(arg, "init");
        return 1;
    }
    auto const type = (lib && !bin) ? PackageType::SharedLibrary : PackageType::Executable;
    auto const root = std::filesystem::current_path();
    auto name = root.filename().string();
    if (!opts.quiet) {
        std::print("  Initializing {} package '{}'\n",
                   type == PackageType::Executable ? "binary" : "library", name);
    }
    if (!ScaffoldPackage(root, name, type, /*initMode=*/true)) {
        return 1;
    }
    if (!opts.quiet) {
        std::print("   Initialized package '{}'\n", name);
    }
    return 0;
}
