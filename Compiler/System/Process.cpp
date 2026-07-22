#include "System/Process.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

#if RUX_OS_WINDOWS
    #include <winhttp.h>
#else
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace Rux::System {
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

std::string JsonFindPackageField(std::string_view json, std::string_view name, std::string_view field) {
    // Walk each top-level object in the array and return the requested field of
    // the one whose "name" matches. Braces and quotes inside string values are
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
            return JsonLookupString(object, field);
        }
        i = j + 1;
    }
    return {};
}

std::string JsonFindPackageRepository(std::string_view json, std::string_view name) {
    return JsonFindPackageField(json, name, "repository");
}

std::vector<std::string> JsonFindGitBlobPaths(const std::string_view json) {
    std::vector<std::string> paths;
    std::size_t pos = 0;
    while ((pos = json.find('{', pos)) != std::string_view::npos) {
        const std::size_t objectStart = pos++;
        std::size_t depth = 0;
        bool inString = false;
        bool escaped = false;
        std::size_t end = objectStart;
        for (; end < json.size(); ++end) {
            const char c = json[end];
            if (escaped) {
                escaped = false;
            }
            else if (c == '\\' && inString) {
                escaped = true;
            }
            else if (c == '"') {
                inString = !inString;
            }
            else if (!inString && c == '{') {
                ++depth;
            }
            else if (!inString && c == '}' && --depth == 0) {
                break;
            }
        }
        if (end >= json.size()) {
            break;
        }

        const std::string_view object = json.substr(objectStart, end - objectStart + 1);
        // Git tree entries are flat objects. Ignoring enclosing objects avoids
        // treating a nested entry as a field of the response object itself.
        if (object.find('{', 1) == std::string_view::npos && JsonLookupString(object, "type") == "blob") {
            if (std::string path = JsonLookupString(object, "path"); !path.empty()) {
                paths.push_back(std::move(path));
            }
        }
    }
    return paths;
}

namespace {
bool CommitDownloadedPackage(const std::filesystem::path &staging, const std::filesystem::path &dest) {
    std::error_code ec;
    std::filesystem::path backup = dest;
    backup += ".previous";
    std::filesystem::remove_all(backup, ec);
    ec.clear();

    const bool hadExisting = std::filesystem::exists(dest, ec);
    if (ec) {
        return false;
    }
    if (hadExisting) {
        std::filesystem::rename(dest, backup, ec);
        if (ec) {
            return false;
        }
    }

    std::filesystem::rename(staging, dest, ec);
    if (ec) {
        if (hadExisting) {
            std::error_code restoreError;
            std::filesystem::rename(backup, dest, restoreError);
        }
        return false;
    }
    std::filesystem::remove_all(backup, ec);
    return true;
}
} // namespace

#if RUX_OS_WINDOWS

namespace {
class WinHttpHandle {
public:
    explicit WinHttpHandle(HINTERNET value = nullptr)
        : value_(value) {
    }

    ~WinHttpHandle() {
        if (value_) {
            WinHttpCloseHandle(value_);
        }
    }

    WinHttpHandle(const WinHttpHandle &) = delete;
    WinHttpHandle &operator=(const WinHttpHandle &) = delete;

    [[nodiscard]] HINTERNET Get() const noexcept {
        return value_;
    }

private:
    HINTERNET value_;
};

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                            size) != size) {
        return {};
    }
    return result;
}

std::string UrlEncode(const std::string_view text, const bool preserveSlashes = false) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size());
    for (const unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || (preserveSlashes && c == '/')) {
            encoded += static_cast<char>(c);
        }
        else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0f];
        }
    }
    return encoded;
}

struct GitHubRepository {
    std::string owner;
    std::string name;
};

std::optional<GitHubRepository> ParseGitHubRepository(std::string_view url) {
    constexpr std::string_view prefix = "https://github.com/";
    if (!url.starts_with(prefix)) {
        return std::nullopt;
    }
    url.remove_prefix(prefix.size());
    while (url.ends_with('/')) {
        url.remove_suffix(1);
    }
    const auto slash = url.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == url.size() ||
        url.find('/', slash + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view name = url.substr(slash + 1);
    if (name.ends_with(".git")) {
        name.remove_suffix(4);
    }
    if (name.empty()) {
        return std::nullopt;
    }
    return GitHubRepository{std::string(url.substr(0, slash)), std::string(name)};
}

bool IsTrueJsonField(const std::string_view json, const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return false;
    }
    do {
        ++pos;
    }
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'));
    return json.substr(pos).starts_with("true");
}

bool IsSafePackagePath(const std::string_view path) {
    if (path.empty() || path.starts_with('/') || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool DownloadPackageWithGit(const std::string &repoUrl, const std::string &folder, const std::filesystem::path &dest,
                            const bool devBranch) {
    const auto repositoryInfo = ParseGitHubRepository(repoUrl);
    if (!repositoryInfo) {
        return false;
    }

    std::string packageRoot = folder;
    std::replace(packageRoot.begin(), packageRoot.end(), '\\', '/');
    while (packageRoot.starts_with('/')) {
        packageRoot.erase(packageRoot.begin());
    }
    while (packageRoot.ends_with('/')) {
        packageRoot.pop_back();
    }
    if (!packageRoot.empty() && !IsSafePackagePath(packageRoot)) {
        return false;
    }

    std::filesystem::path repository = dest;
    repository += ".repository";
    std::filesystem::path staging = dest;
    staging += ".download";
    std::error_code ec;
    std::filesystem::remove_all(repository, ec);
    std::filesystem::remove_all(staging, ec);

    const std::string cloneUrl =
        "https://github.com/" + UrlEncode(repositoryInfo->owner) + "/" + UrlEncode(repositoryInfo->name) + ".git";
    std::vector<std::string> argStorage = {"clone", "--quiet", "--depth", "1"};
    if (devBranch) {
        argStorage.emplace_back("--branch");
        argStorage.emplace_back("dev");
    }
    argStorage.push_back(cloneUrl);
    argStorage.push_back(repository.string());
    std::vector<std::string_view> args;
    args.reserve(argStorage.size());
    for (const auto &arg : argStorage) {
        args.push_back(arg);
    }
    if (const auto exitCode = RunInherited("git", args); !exitCode || *exitCode != 0) {
        std::filesystem::remove_all(repository, ec);
        return false;
    }

    const std::filesystem::path source = packageRoot.empty() ? repository : repository / packageRoot;
    std::filesystem::create_directories(staging, ec);
    if (!ec && std::filesystem::exists(source / "Rux.toml", ec)) {
        std::filesystem::copy_file(source / "Rux.toml", staging / "Rux.toml",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (!ec && std::filesystem::exists(source / "Src", ec)) {
        std::filesystem::copy(
            source / "Src", staging / "Src",
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }
    const bool packageReady = !ec && std::filesystem::exists(staging / "Rux.toml", ec) && !ec;
    std::error_code cleanupError;
    std::filesystem::remove_all(repository, cleanupError);
    if (!packageReady || !CommitDownloadedPackage(staging, dest)) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    return true;
}
} // namespace

std::optional<std::string> FetchUrl(const std::string &url) {
    const std::wstring wideUrl = Utf8ToWide(url);
    if (wideUrl.empty()) {
        return std::nullopt;
    }

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components) ||
        (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS)) {
        return std::nullopt;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    WinHttpHandle session(WinHttpOpen(L"Rux package manager", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.Get()) {
        return std::nullopt;
    }
    WinHttpSetTimeouts(session.Get(), 30'000, 30'000, 30'000, 30'000);

    WinHttpHandle connection(WinHttpConnect(session.Get(), host.c_str(), components.nPort, 0));
    if (!connection.Get()) {
        return std::nullopt;
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const wchar_t *acceptTypes[] = {L"application/json", L"text/plain", L"*/*", nullptr};
    WinHttpHandle request(
        WinHttpOpenRequest(connection.Get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, acceptTypes, flags));
    if (!request.Get() ||
        !WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.Get(), nullptr)) {
        return std::nullopt;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        return std::nullopt;
    }

    constexpr std::size_t maxResponseSize = 128 * 1024 * 1024;
    std::string result;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &available)) {
            return std::nullopt;
        }
        if (available == 0) {
            break;
        }
        if (available > maxResponseSize - result.size()) {
            return std::nullopt;
        }
        const std::size_t offset = result.size();
        result.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.Get(), result.data() + offset, available, &read)) {
            return std::nullopt;
        }
        result.resize(offset + read);
    }
    return result;
}

bool DownloadPackage(const std::string &repoUrl, const std::string &folder, const std::filesystem::path &dest,
                     const bool devBranch) {
    const auto repository = ParseGitHubRepository(repoUrl);
    if (!repository) {
        return false;
    }

    const std::string apiBase =
        "https://api.github.com/repos/" + UrlEncode(repository->owner) + "/" + UrlEncode(repository->name);
    std::string branch = "dev";
    if (!devBranch) {
        const auto metadata = FetchUrl(apiBase);
        if (!metadata || (branch = JsonLookupString(*metadata, "default_branch")).empty()) {
            return DownloadPackageWithGit(repoUrl, folder, dest, devBranch);
        }
    }
    const auto tree = FetchUrl(apiBase + "/git/trees/" + UrlEncode(branch) + "?recursive=1");
    if (!tree || IsTrueJsonField(*tree, "truncated")) {
        return DownloadPackageWithGit(repoUrl, folder, dest, devBranch);
    }

    std::string packageRoot = folder;
    std::replace(packageRoot.begin(), packageRoot.end(), '\\', '/');
    while (packageRoot.starts_with('/')) {
        packageRoot.erase(packageRoot.begin());
    }
    while (packageRoot.ends_with('/')) {
        packageRoot.pop_back();
    }
    const std::string prefix = packageRoot.empty() ? std::string{} : packageRoot + "/";

    std::filesystem::path staging = dest;
    staging += ".download";
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        return false;
    }

    bool foundManifest = false;
    for (const std::string &remotePath : JsonFindGitBlobPaths(*tree)) {
        if (!prefix.empty() && !std::string_view(remotePath).starts_with(prefix)) {
            continue;
        }
        const std::string_view relative =
            prefix.empty() ? std::string_view(remotePath) : std::string_view(remotePath).substr(prefix.size());
        if (relative != "Rux.toml" && !relative.starts_with("Src/")) {
            continue;
        }
        if (!IsSafePackagePath(relative)) {
            std::filesystem::remove_all(staging, ec);
            return false;
        }

        const std::string rawUrl = "https://raw.githubusercontent.com/" + UrlEncode(repository->owner) + "/" +
                                   UrlEncode(repository->name) + "/" + UrlEncode(branch) + "/" +
                                   UrlEncode(remotePath, true);
        const auto contents = FetchUrl(rawUrl);
        if (!contents) {
            std::filesystem::remove_all(staging, ec);
            return DownloadPackageWithGit(repoUrl, folder, dest, devBranch);
        }

        const std::filesystem::path output = staging / std::filesystem::path(relative);
        std::filesystem::create_directories(output.parent_path(), ec);
        if (ec) {
            std::filesystem::remove_all(staging, ec);
            return false;
        }
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        file.write(contents->data(), static_cast<std::streamsize>(contents->size()));
        if (!file) {
            file.close();
            std::filesystem::remove_all(staging, ec);
            return false;
        }
        foundManifest |= relative == "Rux.toml";
    }

    if (!foundManifest) {
        std::filesystem::remove_all(staging, ec);
        return DownloadPackageWithGit(repoUrl, folder, dest, devBranch);
    }
    if (!CommitDownloadedPackage(staging, dest)) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    return true;
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

bool DownloadPackage(const std::string &repoUrl, const std::string &folder, const std::filesystem::path &dest,
                     const bool devBranch) {
    std::filesystem::path repository = dest;
    repository += ".repository";
    std::filesystem::path staging = dest;
    staging += ".download";
    std::error_code ec;
    std::filesystem::remove_all(repository, ec);
    std::filesystem::remove_all(staging, ec);

    const std::string branch = devBranch ? " -b dev" : "";
    const std::string command =
        "git clone" + branch + " " + ShellQuote(repoUrl) + " " + ShellQuote(repository.string());
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(repository, ec);
        return false;
    }

    const std::filesystem::path source = folder.empty() ? repository : repository / folder;
    std::filesystem::create_directories(staging, ec);
    if (!ec && std::filesystem::exists(source / "Rux.toml", ec)) {
        std::filesystem::copy_file(source / "Rux.toml", staging / "Rux.toml",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (!ec && std::filesystem::exists(source / "Src", ec)) {
        std::filesystem::copy(
            source / "Src", staging / "Src",
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }
    const bool packageReady = !ec && std::filesystem::exists(staging / "Rux.toml", ec) && !ec;
    std::error_code cleanupError;
    std::filesystem::remove_all(repository, cleanupError);
    if (!packageReady || !CommitDownloadedPackage(staging, dest)) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    return true;
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
} // namespace Rux::System
