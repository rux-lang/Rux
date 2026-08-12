#include "System/Process.h"
#include "Target/Platform.h"

#include <array>
#include <doctest.h>
#include <filesystem>
#include <string>
#include <system_error>

using namespace Rux::System;

TEST_CASE("UrlEncode leaves unreserved characters alone") {
    CHECK(UrlEncode("rux") == "rux");
    CHECK(UrlEncode("my-pkg_2.0~x") == "my-pkg_2.0~x");
}

TEST_CASE("UrlEncode escapes everything a path segment must not carry") {
    CHECK(UrlEncode("a/b") == "a%2Fb");
    CHECK(UrlEncode("1.0.0+build 1") == "1.0.0%2Bbuild%201");
    CHECK(UrlEncode("?#&=") == "%3F%23%26%3D");
}

TEST_CASE("UrlEncode can preserve slashes for a whole path") {
    CHECK(UrlEncode("Src/Main.rux", true) == "Src/Main.rux");
    CHECK(UrlEncode("Src/A B.rux", true) == "Src/A%20B.rux");
}

TEST_CASE("UrlEncode escapes non-ASCII bytes") {
    CHECK(UrlEncode("\xC3\xA9") == "%C3%A9");
}

TEST_CASE("BuildMultipartBody frames every part with the same boundary") {
    const std::array<MultipartPart, 2> parts{MultipartPart{.name = "manifest", .content = "[Manifest]\n"},
                                             MultipartPart{.name = "package", .content = "PK\x03\x04"}};
    const auto encoded = BuildMultipartBody(parts);
    REQUIRE(encoded.has_value());

    const std::string marker = "multipart/form-data; boundary=";
    REQUIRE(encoded->contentType.starts_with(marker));
    const std::string boundary = encoded->contentType.substr(marker.size());

    CHECK(encoded->body.starts_with("--" + boundary + "\r\n"));
    CHECK(encoded->body.ends_with("--" + boundary + "--\r\n"));
    CHECK(encoded->body.contains("Content-Disposition: form-data; name=\"manifest\"\r\n\r\n[Manifest]\n"));
    CHECK(encoded->body.contains("Content-Disposition: form-data; name=\"package\"\r\n\r\nPK\x03\x04"));
}

#if RUX_OS_WINDOWS
TEST_CASE("failed Windows subprocess launch preserves the system error") {
    const auto missing = std::filesystem::temp_directory_path() / "rux-missing-subprocess-for-launch-error.exe";
    std::error_code filesystemError;
    std::filesystem::remove(missing, filesystemError);

    std::error_code launchError;
    CHECK_FALSE(RunCaptured(missing, {}, &launchError).has_value());
    CHECK(launchError);
}
#endif
