#pragma once

// Subprocess and network helpers used by the package commands.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::System {
// Outcome of a captured subprocess run.
struct RunResult {
    int exitCode = 0;
    std::string output; // combined stdout + stderr
};

// Run `exe` with `args`, inheriting this process's stdin/stdout/stderr (used
// by `rux run`). Returns the child's exit code, or nullopt when the process
// could not be launched.
[[nodiscard]] std::optional<int> RunInherited(const std::filesystem::path &exe,
                                              std::span<const std::string_view> args = {});

// Run `exe`, stdin redirected from the null device, and its combined
// stdout+stderr captured (used by tests and `rux test`). Returns nullopt when
// the process could not be launched.
[[nodiscard]] std::optional<RunResult> RunCaptured(const std::filesystem::path &exe,
                                                   std::span<const std::string_view> args = {});

// Base URL of the versioned registry API, whose routes live below /v1. The
// package commands override it with --registry or RUX_REGISTRY_URL to reach a
// local registry.
inline constexpr std::string_view kRegistryApiBase = "https://api.rux-lang.dev";

// Percent-encode `text` for one URL path segment. With `preserveSlashes`, `/`
// is left alone so a whole path can be encoded in one call.
[[nodiscard]] std::string UrlEncode(std::string_view text, bool preserveSlashes = false);

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
};

// An answered request. A 4xx or 5xx is a response, not a failure: callers such
// as publish need the status and the error body to explain what went wrong.
struct HttpResponse {
    unsigned status = 0;
    std::string body;
};

// Send one request over http or https. Redirects are followed, so a registry
// route that answers 307 with a CDN location returns the final body. Returns
// nullopt only when no response arrived at all, for example a DNS, connection
// or TLS failure.
[[nodiscard]] std::optional<HttpResponse> HttpSend(const HttpRequest &request);

// Fetch the body of a URL. Returns nullopt on failure or a non-2xx status.
[[nodiscard]] std::optional<std::string> FetchUrl(const std::string &url);

// One named part of a multipart/form-data body.
struct MultipartPart {
    std::string name;
    std::string content;
};

struct MultipartBody {
    std::string contentType; ///< Value for the Content-Type request header.
    std::string body;
};

// Encode parts as multipart/form-data. Returns nullopt when no boundary that is
// absent from every part could be found, which no real payload triggers.
[[nodiscard]] std::optional<MultipartBody> BuildMultipartBody(std::span<const MultipartPart> parts);

// Replace `dest` with the fully prepared directory `staging`, keeping the
// previous contents until the swap succeeds. A half-written package therefore
// never becomes the installed one.
[[nodiscard]] bool CommitDownloadedPackage(const std::filesystem::path &staging, const std::filesystem::path &dest);
} // namespace Rux::System
