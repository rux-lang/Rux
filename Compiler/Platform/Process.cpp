#include "Platform/Process.h"

#include "Platform/Platform.h"
#include "Platform/WinApi.h"

#include <array>
#include <cstdio>
#include <vector>

#if !RUX_OS_WINDOWS
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace Rux::Misc {

std::string JsonLookupString(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string_view::npos) {
        std::size_t i = pos + needle.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
            ++i;
        }
        if (i >= json.size() || json[i] != ':') {
            pos = i;
            continue;
        }
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
            ++i;
        }
        if (i >= json.size() || json[i] != '"') {
            pos = i;
            continue;
        }
        ++i;
        const auto end = json.find('"', i);
        if (end == std::string_view::npos) {
            break;
        }
        return std::string(json.substr(i, end - i));
    }
    return {};
}

std::string JsonFindPackageRepository(std::string_view json, std::string_view name) {
    // Walk each top-level object in the array and return the "repository" of the
    // one whose "name" matches. Braces and quotes inside string values are
    // skipped so they don't confuse the object boundaries.
    std::size_t i = 0;
    while ((i = json.find('{', i)) != std::string_view::npos) {
        std::size_t depth = 0;
        bool inString = false;
        bool escaped = false;
        std::size_t j = i;
        for (; j < json.size(); ++j) {
            const char c = json[j];
            if (escaped) {
                escaped = false;
            }
            else if (c == '\\') {
                escaped = inString;
            }
            else if (c == '"') {
                inString = !inString;
            }
            else if (!inString && c == '{') {
                ++depth;
            }
            else if (!inString && c == '}') {
                if (--depth == 0) {
                    break;
                }
            }
        }

        const std::string_view object = json.substr(i, j - i + 1);
        if (JsonLookupString(object, "name") == name) {
            return JsonLookupString(object, "repository");
        }
        i = j + 1;
    }
    return {};
}

#if RUX_OS_WINDOWS

std::optional<std::string> FetchUrl(const std::string &url) {
    const std::string cmd = "curl -s " + url;
    std::array<char, 4096> buffer{};
    std::string result;

    FILE *pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::nullopt;
    }

    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }

    _pclose(pipe);
    return result;
}

bool GitClone(const std::string &repoUrl, const std::filesystem::path &dest, bool devBranch) {
    std::wstring cmd = L"git clone ";
    if (devBranch) {
        cmd += L"--branch dev ";
    }
    cmd += std::wstring(repoUrl.begin(), repoUrl.end()) + L" \"" + dest.wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

bool GitPull(const std::filesystem::path &repoDir) {
    std::wstring cmd = L"git -C \"" + repoDir.wstring() + L"\" pull";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

std::optional<int> RunInherited(const std::filesystem::path &exe, std::span<const std::string_view> args) {
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
        return std::nullopt;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}

std::optional<RunResult> RunCaptured(const std::filesystem::path &exe) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return std::nullopt;
    }
    // The read end must stay in this process only.
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string cmdLine = "\"" + exe.string() + "\"";
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.hStdInput = hNul != INVALID_HANDLE_VALUE ? hNul : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        if (hNul != INVALID_HANDLE_VALUE) {
            CloseHandle(hNul);
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

namespace {

// Wrap a value in single quotes, escaping embedded single quotes, so it can be
// passed safely as one shell argument.
std::string ShellQuote(const std::string &value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted += '\'';
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        }
        else {
            quoted += ch;
        }
    }
    quoted += '\'';
    return quoted;
}

std::optional<std::string> RunCommandCapture(const std::string &command) {
    FILE *pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return std::nullopt;
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output.append(buffer.data());
    }

    const int status = ::pclose(pipe);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    return output;
}

} // namespace

std::optional<std::string> FetchUrl(const std::string &url) {
    const std::string quotedUrl = ShellQuote(url);
    if (auto body = RunCommandCapture("curl -fsSL " + quotedUrl)) {
        return body;
    }
    return RunCommandCapture("wget -qO- " + quotedUrl);
}

bool GitClone(const std::string &repoUrl, const std::filesystem::path &dest, bool devBranch) {
    const std::string cmd = devBranch ? "git clone -b dev " + repoUrl + " \"" + dest.string() + "\""
                                      : "git clone " + repoUrl + " \"" + dest.string() + "\"";
    return std::system(cmd.c_str()) == 0;
}

bool GitPull(const std::filesystem::path &repoDir) {
    const std::string cmd = "git -C \"" + repoDir.string() + "\" pull";
    return std::system(cmd.c_str()) == 0;
}

std::optional<int> RunInherited(const std::filesystem::path &exe, std::span<const std::string_view> args) {
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

std::optional<RunResult> RunCaptured(const std::filesystem::path &exe) {
    const std::string exeStr = exe.string();
    const char *argv[] = {exeStr.c_str(), nullptr};
    int fds[2];
    if (pipe(fds) != 0) {
        return std::nullopt;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
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
        execv(exeStr.c_str(), const_cast<char *const *>(argv));
        _exit(127);
    }
    close(fds[1]);
    RunResult result;
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        result.output.append(buf, static_cast<std::size_t>(n));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return result;
}

#endif

} // namespace Rux::Misc
