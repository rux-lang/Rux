// `rux run` — build the package, then execute the resulting binary.

#include "Cli/Cli.h"
#include "Driver/BuildReport.h"
#include "Driver/BuildTarget.h"
#include "Driver/Driver.h"
#include "Platform/Platform.h"

#if RUX_OS_WINDOWS
    #include "Platform/WinApi.h"
#else
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include <cstdio>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Misc;

int Cli::RunRun(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool isRelease = false;
    std::vector<std::string_view> runArgs;
    bool passThroughMode = false;
    for (auto arg : args) {
        if (passThroughMode) {
            runArgs.push_back(arg);
            continue;
        }
        if (arg == "--") {
            passThroughMode = true;
            continue;
        }
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("run");
            return 0;
        }
        PrintUnknownOption(arg, "run");
        return 1;
    }
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return 1;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return 1;
    }
    // Build first (quiet unless verbose)
    const std::string_view profileName = isRelease ? "Release" : "Debug";
    std::string targetName = HostTargetTriple();
    if (!IsSupportedTargetTriple(targetName)) {
        std::print(stderr,
                   "error: unsupported target '{}'; supported targets are "
                   "linux-x64, windows-x64, macos-x64, macos-arm64, "
                   "freebsd-x64, openbsd-x64, netbsd-x64, illumos-x64\n",
                   targetName);
        return 1;
    }
    const bool buildQuiet = !opts.verbose || opts.quiet;
    if (!buildQuiet) {
        std::print("Compiling {} v{} [{}]\n", manifest->package.name, manifest->package.version,
                   manifestPath->parent_path().string());
    }
    CompileOptions copts;
    copts.manifestPath = *manifestPath;
    copts.manifest = *manifest;
    copts.targetName = std::move(targetName);
    copts.profileName = std::string(profileName);
    copts.quiet = buildQuiet;
    copts.verbose = opts.verbose;
    Driver driver(std::move(copts));
    const CompileResult result = driver.Compile();
    if (!result.ok) {
        return 1;
    }
    if (!buildQuiet) {
        PrintBuildSummary(result.executablePath, profileName, result.stats);
    }
    const bool runDll = (manifest->package.type == "Dll" || manifest->package.type == "dll");
    if (runDll) {
        std::print(stderr, "error: cannot run a DLL package directly\n");
        return 1;
    }
    auto exePath = result.executablePath;
    if (!std::filesystem::exists(exePath)) {
        std::print(stderr, "error: executable not found: '{}'\n", exePath.string());
        return 1;
    }
    if (opts.verbose && !opts.quiet) {
        std::print("     Running `{}`\n", exePath.string());
    }
#if RUX_OS_WINDOWS
    std::string cmdLine = "\"" + exePath.string() + "\"";
    for (const auto &a : runArgs) {
        cmdLine += " \"";
        cmdLine += std::string(a);
        cmdLine += '"';
    }
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        std::print(stderr, "error: failed to launch '{}' (code {})\n", exePath.string(), GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
#else
    std::vector<std::string> argStrings;
    argStrings.push_back(exePath.string());
    for (const auto &a : runArgs) {
        argStrings.emplace_back(a);
    }

    std::vector<char *> argv;
    for (auto &s : argStrings) {
        argv.push_back(s.data());
    }

    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        std::print(stderr, "error: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        execv(exePath.c_str(), argv.data());
        std::print(stderr, "error: failed to launch '{}'\n", exePath.string());
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
