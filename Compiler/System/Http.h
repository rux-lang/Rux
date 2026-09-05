#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::System {
/// Percent-encode `text` for one URL path segment. With `preserveSlashes`, `/` is left alone so a whole path can be
/// encoded in one call.
[[nodiscard]] std::string UrlEncode(std::string_view text, bool preserveSlashes = false);

[[nodiscard]] std::string SanitizeUrl(std::string_view url);

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

/// An answered request. A 4xx or 5xx is a response, not a failure: callers such as publish need the status and the
/// error body to explain what went wrong.
struct HttpResponse {
    unsigned status = 0;
    std::string body;
};

/// Send one request over http or https. Redirects are followed, so a registry route that answers 307 with a CDN
/// location returns the final body. Returns nullopt only when no response arrived at all, for example a DNS, connection
/// or TLS failure.
[[nodiscard]] std::optional<HttpResponse> HttpSend(const HttpRequest &request, std::string *failureDetail = nullptr);

/// Fetch the body of a URL. Returns nullopt on failure or a non-2xx status.
[[nodiscard]] std::optional<std::string> FetchUrl(const std::string &url);

/// One named part of a multipart/form-data body.
struct MultipartPart {
    std::string name;
    std::string content;
};

struct MultipartBody {
    std::string contentType; ///< Value for the Content-Type request header.
    std::string body;
};

/// Encode parts as multipart/form-data. Returns nullopt when no boundary that is absent from every part could be found,
/// which no real payload triggers.
[[nodiscard]] std::optional<MultipartBody> BuildMultipartBody(std::span<const MultipartPart> parts);

} // namespace Rux::System
