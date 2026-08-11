#include "System/Process.h"

#include "System/Os.h"
#include "System/WinApi.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

#if RUX_OS_WINDOWS
    #include <winhttp.h>
#else
    #include <atomic>
    #include <charconv>
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace Rux::System {
std::string UrlEncode(const std::string_view text, const bool preserveSlashes) {
    static constexpr std::string_view digits = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size());
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
                                byte == '~' || (preserveSlashes && byte == '/');
        if (unreserved) {
            encoded.push_back(raw);
        }
        else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 0x0F]);
        }
    }
    return encoded;
}

std::optional<MultipartBody> BuildMultipartBody(const std::span<const MultipartPart> parts) {
    // The boundary may not appear anywhere in the payload. Trying a few fixed
    // suffixes is enough: a part would have to contain every candidate to fail.
    std::string boundary;
    bool usable = false;
    for (int attempt = 0; attempt < 16 && !usable; ++attempt) {
        boundary = std::format("RuxBoundary{:016x}", 0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(attempt + 1));
        usable = std::ranges::none_of(
            parts, [&boundary](const MultipartPart &part) { return part.content.find(boundary) != std::string::npos; });
    }
    if (!usable) {
        return std::nullopt;
    }

    MultipartBody encoded{.contentType = "multipart/form-data; boundary=" + boundary, .body = {}};
    for (const auto &part : parts) {
        encoded.body += "--" + boundary + "\r\n";
        encoded.body += "Content-Disposition: form-data; name=\"" + part.name + "\"\r\n\r\n";
        encoded.body += part.content;
        encoded.body += "\r\n";
    }
    encoded.body += "--" + boundary + "--\r\n";
    return encoded;
}

std::optional<std::string> FetchUrl(const std::string &url) {
    auto response = HttpSend({.method = "GET", .url = url, .headers = {}, .body = {}});
    if (!response || response->status < 200 || response->status >= 300) {
        return std::nullopt;
    }
    return std::move(response->body);
}

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

namespace {
// True when `path` names a file this process could execute. On POSIX the
// permission bits alone do not answer that — the effective user does — so the
// question goes to the kernel.
bool IsExecutableFile(const std::filesystem::path &path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return false;
    }
#if RUX_OS_WINDOWS
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}

// The entries of a `separator`-delimited search list, skipping empty ones.
std::vector<std::string_view> SplitSearchList(const std::string_view list, const char separator) {
    std::vector<std::string_view> entries;
    std::string_view remaining = list;
    while (!remaining.empty()) {
        const auto end = remaining.find(separator);
        const auto entry = remaining.substr(0, end);
        if (!entry.empty()) {
            entries.push_back(entry);
        }
        if (end == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(end + 1);
    }
    return entries;
}

#if RUX_OS_WINDOWS
// The extensions a bare command name may be spelled with, from PATHEXT. The
// empty one comes first because a name may already carry its extension.
std::vector<std::string> ExecutableSuffixes() {
    std::vector<std::string> suffixes{std::string{}};
    const auto configured = GetEnv("PATHEXT");
    const std::string_view list =
        configured && !configured->empty() ? std::string_view(*configured) : ".COM;.EXE;.BAT;.CMD";
    for (const auto entry : SplitSearchList(list, ';')) {
        suffixes.emplace_back(entry);
    }
    return suffixes;
}
#endif
} // namespace

std::optional<std::filesystem::path> FindExecutable(const std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    // A name that already carries a directory names one file rather than
    // something to search for: `./qemu` and `/opt/qemu/bin/qemu-aarch64` are
    // both answers in themselves.
    if (const std::filesystem::path named(name); named.has_parent_path()) {
        return IsExecutableFile(named) ? std::optional(named) : std::nullopt;
    }
    const auto searchPath = GetEnv("PATH");
    if (!searchPath) {
        return std::nullopt;
    }
#if RUX_OS_WINDOWS
    constexpr char separator = ';';
    const std::vector<std::string> suffixes = ExecutableSuffixes();
#else
    constexpr char separator = ':';
    const std::array<std::string, 1> suffixes{std::string{}};
#endif
    for (const auto directory : SplitSearchList(*searchPath, separator)) {
        for (const auto &suffix : suffixes) {
            std::filesystem::path candidate = std::filesystem::path(directory) / name;
            candidate += suffix;
            if (IsExecutableFile(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

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

} // namespace

std::optional<HttpResponse> HttpSend(const HttpRequest &request) {
    const std::wstring wideUrl = Utf8ToWide(request.url);
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
    // The send and receive budgets cover a publication upload, which the
    // registry allows up to two minutes to complete.
    WinHttpSetTimeouts(session.Get(), 30'000, 30'000, 120'000, 120'000);

    WinHttpHandle connection(WinHttpConnect(session.Get(), host.c_str(), components.nPort, 0));
    if (!connection.Get()) {
        return std::nullopt;
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const wchar_t *acceptTypes[] = {L"application/json", L"application/problem+json", L"text/plain", L"*/*", nullptr};
    const std::wstring method = Utf8ToWide(request.method);
    WinHttpHandle handle(WinHttpOpenRequest(connection.Get(), method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            acceptTypes, flags));
    if (!handle.Get()) {
        return std::nullopt;
    }

    for (const auto &header : request.headers) {
        const std::wstring line = Utf8ToWide(header.name + ": " + header.value);
        if (line.empty() || !WinHttpAddRequestHeaders(handle.Get(), line.c_str(), static_cast<DWORD>(-1),
                                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            return std::nullopt;
        }
    }

    const auto bodySize = static_cast<DWORD>(request.body.size());
    void *bodyData = request.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                          : const_cast<void *>(static_cast<const void *>(request.body.data()));
    if (!WinHttpSendRequest(handle.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyData, bodySize, bodySize, 0) ||
        !WinHttpReceiveResponse(handle.Get(), nullptr)) {
        return std::nullopt;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(handle.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }

    constexpr std::size_t maxResponseSize = 128 * 1024 * 1024;
    std::string result;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle.Get(), &available)) {
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
        if (!WinHttpReadData(handle.Get(), result.data() + offset, available, &read)) {
            return std::nullopt;
        }
        result.resize(offset + read);
    }
    return HttpResponse{.status = static_cast<unsigned>(status), .body = std::move(result)};
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

std::optional<RunResult> RunCaptured(const std::filesystem::path &exe, const std::span<const std::string_view> args) {
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

// Quote a value for a curl configuration file, where an argument is wrapped in
// double quotes and backslashes and quotes are escaped.
std::string CurlConfigQuote(const std::string &value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted += '"';
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            quoted += '\\';
        }
        quoted += ch;
    }
    quoted += '"';
    return quoted;
}

// Write a file only the current user can read. The curl configuration carries
// the bearer credential, so it must never be world-readable, and open() with an
// explicit mode avoids the window a later permissions change would leave.
bool WritePrivateFile(const std::filesystem::path &path, const std::string_view data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0600);
    if (fd < 0) {
        return false;
    }
    std::size_t written = 0;
    bool ok = true;
    while (written < data.size()) {
        const ssize_t chunk = ::write(fd, data.data() + written, data.size() - written);
        if (chunk <= 0) {
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(chunk);
    }
    return ::close(fd) == 0 && ok;
}

std::optional<std::string> ReadWholeFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

// Removes its directory when the request finishes, however it finishes.
struct TemporaryDirectory {
    std::filesystem::path path;

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

std::optional<HttpResponse> HttpSend(const HttpRequest &request) {
    // curl carries the credential and the body in files rather than in argv,
    // so neither reaches the process list.
    std::error_code ec;
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::nullopt;
    }
    const TemporaryDirectory workspace{tempRoot / std::format("rux-http-{}-{}", ::getpid(), counter.fetch_add(1))};
    if (!std::filesystem::create_directories(workspace.path, ec) || ec) {
        return std::nullopt;
    }
    std::filesystem::permissions(workspace.path, std::filesystem::perms::owner_all, ec);

    const std::filesystem::path configPath = workspace.path / "request.conf";
    const std::filesystem::path bodyPath = workspace.path / "request.body";
    const std::filesystem::path responsePath = workspace.path / "response.body";

    std::string config;
    config += "--request " + CurlConfigQuote(request.method) + "\n";
    config += "--url " + CurlConfigQuote(request.url) + "\n";
    config += "--silent\n--show-error\n--location\n";
    config += "--max-time 120\n";
    config += "--output " + CurlConfigQuote(responsePath.string()) + "\n";
    config += "--write-out \"%{http_code}\"\n";
    for (const auto &header : request.headers) {
        config += "--header " + CurlConfigQuote(header.name + ": " + header.value) + "\n";
    }
    if (!request.body.empty()) {
        if (!WritePrivateFile(bodyPath, request.body)) {
            return std::nullopt;
        }
        config += "--data-binary " + CurlConfigQuote("@" + bodyPath.string()) + "\n";
    }
    if (!WritePrivateFile(configPath, config)) {
        return std::nullopt;
    }

    // curl exits 0 for a 4xx or 5xx unless --fail is given, so a non-zero exit
    // here means no response arrived at all. Its stderr is left attached so a
    // transport failure still explains itself, as it did before.
    auto status = RunCommandCapture("curl --config " + ShellQuote(configPath.string()));
    if (!status) {
        // A plain read can still succeed through wget where curl is absent.
        // Anything richer than that needs curl.
        if (request.method != "GET" || !request.headers.empty() || !request.body.empty()) {
            return std::nullopt;
        }
        auto body = RunCommandCapture("wget -qO- " + ShellQuote(request.url));
        if (!body) {
            return std::nullopt;
        }
        return HttpResponse{.status = 200, .body = std::move(*body)};
    }

    unsigned code = 0;
    const auto digits = std::string_view(*status);
    if (std::from_chars(digits.data(), digits.data() + digits.size(), code).ec != std::errc{}) {
        return std::nullopt;
    }
    return HttpResponse{.status = code, .body = ReadWholeFile(responsePath).value_or(std::string{})};
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

std::optional<RunResult> RunCaptured(const std::filesystem::path &exe, const std::span<const std::string_view> args) {
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
        execv(exeStr.c_str(), argv.data());
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
