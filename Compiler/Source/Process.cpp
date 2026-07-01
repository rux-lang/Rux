#include "Rux/Process.h"

#include "Rux/Platform.h"
#include "Rux/WinApi.h"

#include <array>
#include <cstdio>

#if !RUX_OS_WINDOWS
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

#endif

} // namespace Rux::Misc
