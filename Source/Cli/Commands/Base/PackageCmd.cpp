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
    #include <fcntl.h>
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include "Rux/SourceLoader.h"

using namespace Rux;
using namespace Platform;
using namespace Misc;


int Cli::RunAdd(std::span<const std::string_view> args, const GlobalOptions& opts) {
    std::string_view spec;
    std::string_view pathArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpAdd();
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
        PrintHelpAdd();
        return 1;
    }
    auto manifestPath = RequireManifest();
    if (!manifestPath) return 1;
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) return 1;
    auto [pkgName, pkgVersion] = ParsePackageSpec(spec);

    if (!pathArg.empty()) {
        const bool changed = manifest->AddPathDependency(pkgName, std::string(pathArg));
        if (!manifest->Save(*manifestPath)) {
            std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
            return 1;
        }
        if (!opts.quiet) {
            if (changed)
                std::print("Added {} @ path '{}'\n", pkgName, pathArg);
            else
                std::print("Up-to-date {} @ path '{}'\n", pkgName, pathArg);
        }
        return 0;
    }

    if (!opts.quiet) std::print("     Fetching registry...\n");

    const auto jsonOpt = FetchUrl(std::string(kRegistryUrl));
    if (!jsonOpt) {
        std::print(stderr, "error: failed to fetch package registry\n");
        return 1;
    }

    if (JsonLookupString(*jsonOpt, pkgName).empty()) {
        std::print(stderr, "error: package '{}' not found in registry\n", pkgName);
        return 1;
    }

    const bool changed = manifest->AddDependency(pkgName, pkgVersion);
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) {
        const std::string ver = pkgVersion.empty() ? "latest" : pkgVersion;
        if (changed)
            std::print("Added {} @ {}\n", pkgName, ver);
        else
            std::print("Up-to-date {} @ {}\n", pkgName, ver);
    }
    return 0;
}

int Cli::RunRemove(std::span<const std::string_view> args, const GlobalOptions& opts) {
    std::string_view name;
    for (auto arg : args) {
        if (arg == "-h" || arg == "--help") {
            PrintHelpRemove();
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
        PrintHelpRemove();
        return 1;
    }
    auto manifestPath = RequireManifest();
    if (!manifestPath) return 1;
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) return 1;
    std::string pkgName(name);
    if (!manifest->RemoveDependency(pkgName)) {
        std::print(stderr, "error: package '{}' is not a dependency\n", pkgName);
        return 1;
    }
    if (!manifest->Save(*manifestPath)) {
        std::print(stderr, "error: failed to write '{}'\n", manifestPath->string());
        return 1;
    }
    if (!opts.quiet) std::print("     Removed {}\n", pkgName);
    return 0;
}

int Cli::RunTest(std::span<const std::string_view> args, const GlobalOptions& opts) {
    bool isRelease = false;
    for (auto& arg : args) {
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpTest();
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
        if (!manifest) return 1;
        if (!opts.quiet)
            std::print("     Testing {} v{}\n", manifest->package.name, manifest->package.version);
        projectRoot = manifestPath->parent_path();
        testsDir = projectRoot / "Tests";
    } else {
        projectRoot = std::filesystem::current_path();
        testsDir = projectRoot / "Tests";
        std::error_code ec;
        if (!std::filesystem::exists(testsDir, ec)) {
            RequireManifest(); // Prints standard "Rux.toml not found" error
            return 1;
        }
        if (!opts.quiet)
            std::print("     Running workspace tests\n");
    }

    const std::string_view profileName = isRelease ? "Release" : "Debug";

    // Collect test package directories: any subdirectory of Tests/ that
    // contains a Rux.toml with Type = "bin".
    std::vector<std::filesystem::path> testPackages;
    {
        std::error_code ec;
        if (!std::filesystem::exists(testsDir, ec)) {
            if (!opts.quiet) std::print("  No Tests/ directory found — nothing to run.\n");
            return 0;
        }
        for (const auto& entry : std::filesystem::directory_iterator(testsDir, ec)) {
            if (!entry.is_directory()) continue;
            const auto toml = entry.path() / "Rux.toml";
            if (!std::filesystem::exists(toml)) continue;
            auto pkgManifest = Manifest::Load(toml);
            if (!pkgManifest) continue;
            // Only run binary packages (not DLLs / shared libraries).
            const auto& type = pkgManifest->package.type;
            if (type != "bin" && type != "Bin") continue;
            testPackages.push_back(entry.path());
        }
        std::sort(testPackages.begin(), testPackages.end());
    }

    if (testPackages.empty()) {
        if (!opts.quiet) std::print("  No test packages found in Tests/.\n");
        return 0;
    }

    // Helper: run rux build inside a package directory, then execute the
    // resulting binary. Returns the process exit code, or -1 on build/launch
    // failure.
    auto runOne = [&](const std::filesystem::path& pkgDir) -> int {
        // Load the package manifest to derive the executable name and output path.
        auto pkgManifest = Manifest::Load(pkgDir / "Rux.toml");
        if (!pkgManifest) {
            std::print(stderr, "error: failed to parse '{}'\n", (pkgDir / "Rux.toml").string());
            return -1;
        }

        // Build: temporarily change the working directory into the package root
        // so that RequireManifest() and source paths resolve correctly.
        const auto savedCwd = std::filesystem::current_path();
        std::error_code ec;
        std::filesystem::current_path(pkgDir, ec);
        if (ec) {
            std::print(stderr, "error: cannot chdir into '{}': {}\n", pkgDir.string(), ec.message());
            return -1;
        }

        GlobalOptions buildOpts = opts;
        buildOpts.quiet = true; // suppress per-file build output for tests
        std::vector<std::string_view> buildArgs;
        if (isRelease) buildArgs.emplace_back("--release");
        buildArgs.emplace_back("--quiet");

        const int buildRc = RunBuild(buildArgs, buildOpts);
        std::filesystem::current_path(savedCwd, ec); // always restore CWD

        if (buildRc != 0) {
            std::print(stderr, "error: build failed for test package '{}'\n", pkgDir.filename().string());
            return -1;
        }

        // Locate the built executable.
        const auto binDir = ResolveBuildOutputDir(pkgDir, *pkgManifest, profileName);
        std::string exeName = pkgManifest->package.name;
#if RUX_OS_WINDOWS
        exeName += ".exe";
#endif
        const auto exePath = binDir / exeName;

        if (!std::filesystem::exists(exePath)) {
            std::print(stderr,
                       "error: built executable not found at '{}'\n",
                       exePath.string());
            return -1;
        }

        if (opts.verbose)
            std::print("     Running `{}`\n", exePath.string());

        // Execute the test binary and capture its exit code.
#if RUX_OS_WINDOWS
        HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::string cmdLine = "\"" + exePath.string() + "\"";
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.hStdInput  = hNul != INVALID_HANDLE_VALUE ? hNul : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si.dwFlags    = STARTF_USESTDHANDLES;
        if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0,
                            nullptr, nullptr, &si, &pi)) {
            std::print(stderr,
                       "error: failed to launch '{}' (code {})\n",
                       exePath.string(), GetLastError());
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
        const std::string exeStr = exePath.string();
        const char* argv[] = {exeStr.c_str(), nullptr};
        const pid_t pid = fork();
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
            execv(exeStr.c_str(), const_cast<char* const*>(argv));
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

    for (const auto& pkgDir : testPackages) {
        const std::string label = pkgDir.filename().string();
        if (!opts.quiet) std::print("      Running test package: {}\n", label);

        const int rc = runOne(pkgDir);
        if (rc == 0) {
            ++passed;
            if (!opts.quiet) std::print("    PASS: {}\n", label);
        } else {
            ++failed;
            std::print(stderr,
                       "    FAIL: {} (exit {})\n",
                       label,
                       rc == -1 ? std::string("build/launch error") : std::to_string(rc));
        }
    }

    // Summary line.
    const int total = passed + failed;
    if (!opts.quiet || failed > 0) {
        std::print("{}: {} passed, {} failed, {} total\n",
                   failed == 0 ? "ok" : "FAILED",
                   passed, failed, total);
    }
    return failed == 0 ? 0 : 1;
}

int Cli::RunInit(std::span<const std::string_view> args, const GlobalOptions& opts) {
    bool bin = false;
    bool lib = false;
    for (auto& arg : args) {
        if (arg == "--bin") {
            bin = true;
            continue;
        }
        if (arg == "--lib") {
            lib = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpInit();
            return 0;
        }
        PrintUnknownOption(arg, "init");
        return 1;
    }
    const auto type = (lib && !bin) ? PackageType::SharedLibrary : PackageType::Executable;
    const auto root = std::filesystem::current_path();
    auto name = root.filename().string();
    if (!opts.quiet)
        std::print("  Initializing {} package '{}'\n", type == PackageType::Executable ? "binary" : "library", name);
    if (!ScaffoldPackage(root, name, type, /*initMode=*/true)) return 1;
    if (!opts.quiet) std::print("   Initialized package '{}'\n", name);
    return 0;
}