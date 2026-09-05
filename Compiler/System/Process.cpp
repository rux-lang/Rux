#include "System/Process.h"

#include "System/Os.h"
#include "System/WinApi.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

#if RUX_OS_WINDOWS
#else
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace Rux::System {
#if RUX_OS_WINDOWS
std::optional<int> RunInherited(const std::filesystem::path &exe, std::span<const std::string_view> args,
                                std::error_code *launchError) {
    std::string cmdLine = "\"" + exe.string() + "\"";
    for (const auto &a : args) {
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
        if (launchError != nullptr) {
            *launchError = {static_cast<int>(GetLastError()), std::system_category()};
        }
        return std::nullopt;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}

std::optional<RunResult> RunCaptured(const std::filesystem::path &exe, const std::span<const std::string_view> args,
                                     std::error_code *launchError) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        if (launchError != nullptr) {
            *launchError = {static_cast<int>(GetLastError()), std::system_category()};
        }
        return std::nullopt;
    }
    // The read end must stay in this process only.
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string cmdLine = "\"" + exe.string() + "\"";
    for (const auto argument : args) {
        cmdLine += " \"";
        cmdLine += argument;
        cmdLine += '"';
    }
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.hStdInput = hNul != INVALID_HANDLE_VALUE ? hNul : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        const DWORD error = GetLastError();
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        if (hNul != INVALID_HANDLE_VALUE) {
            CloseHandle(hNul);
        }
        if (launchError != nullptr) {
            *launchError = {static_cast<int>(error), std::system_category()};
        }
        return std::nullopt;
    }
    // Close our copy of the write end so ReadFile returns EOF once the child
    // exits and no writable handle remains.
    CloseHandle(writePipe);
    RunResult result;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        result.output.append(buf, n);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hNul != INVALID_HANDLE_VALUE) {
        CloseHandle(hNul);
    }
    result.exitCode = static_cast<int>(exitCode);
    return result;
}

#else
std::optional<int> RunInherited(const std::filesystem::path &exe, std::span<const std::string_view> args,
                                std::error_code *launchError) {
    std::vector<std::string> argStrings;
    argStrings.push_back(exe.string());
    for (const auto &a : args) {
        argStrings.emplace_back(a);
    }

    std::vector<char *> argv;
    argv.reserve(argStrings.size() + 1);
    for (auto &s : argStrings) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        if (launchError != nullptr) {
            *launchError = {errno, std::generic_category()};
        }
        return std::nullopt;
    }
    if (pid == 0) {
        execv(argStrings.front().c_str(), argv.data());
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

std::optional<RunResult> RunCaptured(const std::filesystem::path &exe, const std::span<const std::string_view> args,
                                     std::error_code *launchError) {
    const std::string exeStr = exe.string();
    std::vector<std::string> argStrings;
    argStrings.reserve(args.size() + 1);
    argStrings.push_back(exeStr);
    for (const auto argument : args)
        argStrings.emplace_back(argument);
    std::vector<char *> argv;
    argv.reserve(argStrings.size() + 1);
    for (auto &argument : argStrings)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    int fds[2];
    if (pipe(fds) != 0) {
        if (launchError != nullptr) {
            *launchError = {errno, std::generic_category()};
        }
        return std::nullopt;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        const int error = errno;
        close(fds[0]);
        close(fds[1]);
        if (launchError != nullptr) {
            *launchError = {error, std::generic_category()};
        }
        return std::nullopt;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, 0);
            close(devnull);
        }
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execv(exeStr.c_str(), argv.data());
        _exit(127);
    }
    close(fds[1]);
    RunResult result;
    char buf[4096];
    // Drain until EOF; a signal delivered to this process must not truncate
    // the captured output, so an EINTR-interrupted read is retried.
    for (;;) {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            result.output.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return result;
}

#endif
} // namespace Rux::System
