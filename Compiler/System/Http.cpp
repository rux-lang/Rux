#include "System/Http.h"

#include "System/Process.h"
#include "System/WinApi.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <cstdint>
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
    #include <unistd.h>
#endif

namespace Rux::System {
std::string SanitizeUrl(const std::string_view url) {
    std::string sanitized(url.substr(0, url.find_first_of("?#")));
    const std::size_t scheme = sanitized.find("://");
    if (scheme != std::string::npos) {
        const std::size_t authority = scheme + 3;
        const std::size_t end = sanitized.find('/', authority);
        const std::size_t at = sanitized.find('@', authority);
        if (at != std::string::npos && (end == std::string::npos || at < end)) {
            sanitized.erase(authority, at - authority + 1);
        }
    }
    return sanitized;
}

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

#if RUX_OS_WINDOWS

namespace {
class WinHttpHandle {
public:
    explicit WinHttpHandle(HINTERNET inputValue = nullptr)
        : value(inputValue) {
    }

    ~WinHttpHandle() {
        if (value) {
            WinHttpCloseHandle(value);
        }
    }

    WinHttpHandle(const WinHttpHandle &) = delete;
    WinHttpHandle &operator=(const WinHttpHandle &) = delete;

    [[nodiscard]] HINTERNET Get() const noexcept {
        return value;
    }

private:
    HINTERNET value;
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

std::optional<HttpResponse> HttpSend(const HttpRequest &request, std::string *failureDetail) {
    const auto Fail = [failureDetail](const DWORD error) -> std::optional<HttpResponse> {
        if (failureDetail != nullptr) {
            const std::error_code code(static_cast<int>(error), std::system_category());
            *failureDetail = std::format("system error {}: {}", code.value(), code.message());
        }
        return std::nullopt;
    };
    const std::wstring wideUrl = Utf8ToWide(request.url);
    if (wideUrl.empty()) {
        return Fail(ERROR_INVALID_PARAMETER);
    }

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components) ||
        (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS)) {
        return Fail(ERROR_INVALID_PARAMETER);
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
        return Fail(GetLastError());
    }
    // The send and receive budgets cover a publication upload, which the
    // registry allows up to two minutes to complete.
    WinHttpSetTimeouts(session.Get(), 30'000, 30'000, 120'000, 120'000);

    WinHttpHandle connection(WinHttpConnect(session.Get(), host.c_str(), components.nPort, 0));
    if (!connection.Get()) {
        return Fail(GetLastError());
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const wchar_t *acceptTypes[] = {L"application/json", L"application/problem+json", L"text/plain", L"*/*", nullptr};
    const std::wstring method = Utf8ToWide(request.method);
    WinHttpHandle handle(WinHttpOpenRequest(connection.Get(), method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            acceptTypes, flags));
    if (!handle.Get()) {
        return Fail(GetLastError());
    }

    for (const auto &header : request.headers) {
        const std::wstring line = Utf8ToWide(header.name + ": " + header.value);
        if (line.empty() || !WinHttpAddRequestHeaders(handle.Get(), line.c_str(), static_cast<DWORD>(-1),
                                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            return Fail(GetLastError());
        }
    }

    const auto bodySize = static_cast<DWORD>(request.body.size());
    void *bodyData = request.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                          : const_cast<void *>(static_cast<const void *>(request.body.data()));
    if (!WinHttpSendRequest(handle.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyData, bodySize, bodySize, 0) ||
        !WinHttpReceiveResponse(handle.Get(), nullptr)) {
        return Fail(GetLastError());
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(handle.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return Fail(GetLastError());
    }

    constexpr std::size_t maxResponseSize = 128 * 1024 * 1024;
    std::string result;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle.Get(), &available)) {
            return Fail(GetLastError());
        }
        if (available == 0) {
            break;
        }
        if (available > maxResponseSize - result.size()) {
            return Fail(ERROR_INSUFFICIENT_BUFFER);
        }
        const std::size_t offset = result.size();
        result.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(handle.Get(), result.data() + offset, available, &read)) {
            return Fail(GetLastError());
        }
        result.resize(offset + read);
    }
    return HttpResponse{.status = static_cast<unsigned>(status), .body = std::move(result)};
}

#else

namespace {
std::optional<std::string> RunClient(const std::filesystem::path &executable,
                                     const std::span<const std::string_view> arguments) {
    auto result = RunCaptured(executable, arguments);
    if (!result || result->exitCode != 0)
        return std::nullopt;
    return std::move(result->output);
}

/// Quote a value for a curl configuration file, where an argument is wrapped in double quotes and backslashes and
/// quotes are escaped.
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

/// Write a file only the current user can read. The curl configuration carries the bearer credential, so it must never
/// be world-readable, and open() with an explicit mode avoids the window a later permissions change would leave.
bool WritePrivateFile(const std::filesystem::path &path, const std::string_view data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_EXCL | O_CLOEXEC, 0600);
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

/// Removes its directory when the request finishes, however it finishes.
struct TemporaryDirectory {
    std::filesystem::path path;

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

std::optional<HttpResponse> HttpSend(const HttpRequest &request, std::string *failureDetail) {
    const auto Fail = [failureDetail](std::string detail) -> std::optional<HttpResponse> {
        if (failureDetail != nullptr) {
            *failureDetail = std::move(detail);
        }
        return std::nullopt;
    };
    // curl carries the credential and the body in files rather than in argv,
    // so neither reaches the process list.
    std::error_code ec;
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return Fail(std::format("temporary-directory error: {}", ec.message()));
    }
    const TemporaryDirectory workspace{tempRoot / std::format("rux-http-{}-{}", ::getpid(), counter.fetch_add(1))};
    if (!std::filesystem::create_directories(workspace.path, ec) || ec) {
        return Fail(std::format("temporary-directory error: {}", ec.message()));
    }
    std::filesystem::permissions(workspace.path, std::filesystem::perms::owner_all, ec);

    const std::filesystem::path configPath = workspace.path / "request.conf";
    const std::filesystem::path bodyPath = workspace.path / "request.body";
    const std::filesystem::path responsePath = workspace.path / "response.body";

    std::string config;
    config += "--silent\n";
    config += "--request " + CurlConfigQuote(request.method) + "\n";
    config += "--url " + CurlConfigQuote(request.url) + "\n";
    config += "--location\n";
    config += "--max-time 120\n";
    config += "--output " + CurlConfigQuote(responsePath.string()) + "\n";
    config += "--write-out \"%{http_code}\"\n";
    for (const auto &header : request.headers) {
        config += "--header " + CurlConfigQuote(header.name + ": " + header.value) + "\n";
    }
    if (!request.body.empty()) {
        if (!WritePrivateFile(bodyPath, request.body)) {
            return Fail("could not prepare the private HTTP request body");
        }
        config += "--data-binary " + CurlConfigQuote("@" + bodyPath.string()) + "\n";
    }
    if (!WritePrivateFile(configPath, config)) {
        return Fail("could not prepare the private HTTP request configuration");
    }

    // curl exits 0 for a 4xx or 5xx unless --fail is given. Suppress its
    // unstructured stderr; callers receive the transport detail below.
    const std::string configName = configPath.string();
    auto status = RunClient("curl", std::array<std::string_view, 2>{"--config", configName});
    if (!status) {
        // A plain read can still succeed through wget where curl is absent.
        // Anything richer than that needs curl.
        if (request.method != "GET" || !request.headers.empty() || !request.body.empty()) {
            return Fail("the HTTP client did not receive a response");
        }
        auto body = RunClient("wget", std::array<std::string_view, 3>{"-qO-", "--", request.url});
        if (!body) {
            return Fail("the HTTP client did not receive a response");
        }
        return HttpResponse{.status = 200, .body = std::move(*body)};
    }

    unsigned code = 0;
    const auto digits = std::string_view(*status);
    if (std::from_chars(digits.data(), digits.data() + digits.size(), code).ec != std::errc{}) {
        return Fail("the HTTP client returned an invalid status code");
    }
    return HttpResponse{.status = code, .body = ReadWholeFile(responsePath).value_or(std::string{})};
}

#endif
} // namespace Rux::System
