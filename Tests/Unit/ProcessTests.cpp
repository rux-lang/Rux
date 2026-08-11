#include "System/Os.h"
#include "System/Process.h"

#include <array>
#include <doctest.h>
#include <filesystem>
#include <string>

using namespace Rux::System;

namespace {
// The compiler this build produced: a file that is certainly executable, and
// the only one a test can name on every host.
std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / ExecutableFileName("rux");
}
} // namespace

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

TEST_CASE("FindExecutable takes a name carrying a directory as the answer itself") {
    const auto rux = RuxExecutable();
    REQUIRE(std::filesystem::is_regular_file(rux));
    const auto found = FindExecutable(rux.string());
    REQUIRE(found.has_value());
    CHECK(*found == rux);
}

TEST_CASE("FindExecutable answers nothing for what cannot be run") {
    CHECK(!FindExecutable("").has_value());
    // A directory is not a program, however executable its permission bits are.
    CHECK(!FindExecutable(std::filesystem::path(RUX_ROOT_DIR).string()).has_value());
    CHECK(!FindExecutable((RuxExecutable().parent_path() / "rux-that-was-never-built").string()).has_value());
    CHECK(!FindExecutable("rux-emulator-that-is-not-installed").has_value());
}

TEST_CASE("FindExecutable searches PATH for a bare name") {
    const auto rux = RuxExecutable();
    const auto previousPath = GetEnv("PATH");
    REQUIRE(SetEnv("PATH", rux.parent_path().string()));

    // Windows spells the name without its extension, which PATHEXT supplies.
    const auto found = FindExecutable("rux");
    CHECK(found.has_value());
    if (found) {
        CHECK(*found == rux);
    }
    CHECK(!FindExecutable("rux-emulator-that-is-not-installed").has_value());

    CHECK(previousPath.has_value());
    if (previousPath) {
        CHECK(SetEnv("PATH", *previousPath));
    }
}
