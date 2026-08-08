// Decoding and version selection for the registry read contract. Everything
// here is pure: no request is made, so the cases pin the shape of the documents
// the API promises rather than the behavior of a live server.

#include "Driver/Registry.h"

#include <doctest.h>
#include <string>

using namespace Rux;
using namespace Rux::Driver;

namespace {
constexpr const char *kIndexBody = R"({
  "data": {
    "namespace": "Rux",
    "package": "Io",
    "versions": [
      { "version": "0.1.0", "min_rux": "0.4.0", "yanked": false, "dependencies": [] },
      {
        "version": "0.2.0",
        "min_rux": "0.4.0",
        "yanked": false,
        "dependencies": [
          { "alias": "Json", "target_namespace": "Rux", "target_package": "Json", "version_range": "^1" },
          { "alias": "Mem", "target_namespace": "Rux", "target_package": "Memory", "version_range": ">=0.1.0, <0.3.0" }
        ]
      },
      { "version": "0.3.0", "min_rux": "9.0.0", "yanked": false, "dependencies": [] },
      { "version": "0.4.0", "min_rux": "0.4.0", "yanked": true, "dependencies": [] }
    ]
  }
})";

SemanticVersion Version(const std::string_view text) {
    auto parsed = SemanticVersion::Parse(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

VersionRange Range(const std::string_view text) {
    auto parsed = VersionRange::Parse(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

IdentitySegment Segment(const std::string_view text) {
    auto parsed = IdentitySegment::Parse(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}
} // namespace

TEST_CASE("DecodePackageIndex reads identity, versions and dependency edges") {
    const auto entry = DecodePackageIndex(kIndexBody);
    REQUIRE(entry.has_value());

    CHECK(entry->ns.Text() == "Rux");
    CHECK(entry->package.Text() == "Io");
    REQUIRE(entry->versions.size() == 4);

    CHECK(entry->versions[0].version.Text() == "0.1.0");
    REQUIRE(entry->versions[0].minRux.has_value());
    CHECK(entry->versions[0].minRux->Text() == "0.4.0");
    CHECK_FALSE(entry->versions[0].yanked);
    CHECK(entry->versions[0].dependencies.empty());

    REQUIRE(entry->versions[1].dependencies.size() == 2);
    CHECK(entry->versions[1].dependencies[0].alias.Text() == "Json");
    CHECK(entry->versions[1].dependencies[0].ns.Text() == "Rux");
    CHECK(entry->versions[1].dependencies[0].package.Text() == "Json");
    CHECK(entry->versions[1].dependencies[0].range.Text() == "^1");
    CHECK(entry->versions[1].dependencies[1].package.Text() == "Memory");
    CHECK(entry->versions[1].dependencies[1].range.Text() == ">=0.1.0, <0.3.0");

    CHECK(entry->versions[3].yanked);
}

TEST_CASE("DecodePackageIndex keeps the registry's display spelling") {
    const auto entry = DecodePackageIndex(R"({
      "data": { "namespace": "Acme", "package": "Fast_Json", "versions": [] }
    })");
    REQUIRE(entry.has_value());
    CHECK(entry->package.Text() == "Fast_Json");
    CHECK(entry->package.Normalized() == "fast-json");
    CHECK(entry->versions.empty());
}

TEST_CASE("DecodePackageIndex accepts a version without a declared minimum") {
    const auto entry = DecodePackageIndex(R"({
      "data": {
        "namespace": "Rux", "package": "Io",
        "versions": [ { "version": "1.0.0", "min_rux": null, "yanked": false } ]
      }
    })");
    REQUIRE(entry.has_value());
    REQUIRE(entry->versions.size() == 1);
    CHECK_FALSE(entry->versions[0].minRux.has_value());
}

TEST_CASE("DecodePackageIndex rejects documents it cannot trust") {
    const auto notJson = DecodePackageIndex("<html>");
    REQUIRE_FALSE(notJson.has_value());
    CHECK(notJson.error().kind == RegistryErrorKind::Malformed);

    CHECK_FALSE(DecodePackageIndex(R"({ "versions": [] })").has_value());
    CHECK_FALSE(DecodePackageIndex(R"({ "data": { "namespace": "Rux", "package": "Io" } })").has_value());
    CHECK_FALSE(
        DecodePackageIndex(R"({ "data": { "namespace": "-bad", "package": "Io", "versions": [] } })").has_value());
    CHECK_FALSE(DecodePackageIndex(R"({
      "data": { "namespace": "Rux", "package": "Io", "versions": [ { "version": "not-a-version" } ] }
    })")
                    .has_value());
}

TEST_CASE("DecodeArtifactChecksum reads a sha256 digest") {
    const auto digest = DecodeArtifactChecksum(R"({
      "data": {
        "version": "1.0.0",
        "checksum": { "algorithm": "sha256", "digest": "0123456789abcdef" }
      }
    })");
    REQUIRE(digest.has_value());
    CHECK(*digest == "0123456789abcdef");
}

TEST_CASE("DecodeArtifactChecksum refuses anything but sha256") {
    CHECK_FALSE(
        DecodeArtifactChecksum(R"({ "data": { "checksum": { "algorithm": "md5", "digest": "x" } } })").has_value());
    CHECK_FALSE(DecodeArtifactChecksum(R"({ "data": { "checksum": { "algorithm": "sha256" } } })").has_value());
    CHECK_FALSE(DecodeArtifactChecksum(R"({ "data": {} })").has_value());
    CHECK_FALSE(DecodeArtifactChecksum("{}").has_value());
}

TEST_CASE("DecodeProblem reads the RFC 9457 members") {
    const RegistryError error = DecodeProblem(R"({
      "type": "https://api.rux-lang.dev/problems/rate_limited",
      "title": "Too Many Requests",
      "status": 429,
      "code": "rate_limited",
      "detail": "Retry shortly."
    })",
                                              429);
    CHECK(error.kind == RegistryErrorKind::Rejected);
    CHECK(error.status == 429);
    CHECK(error.code == "rate_limited");
    CHECK(error.detail == "Retry shortly.");
}

TEST_CASE("SelectVersion takes the highest match") {
    const auto entry = DecodePackageIndex(kIndexBody);
    REQUIRE(entry.has_value());
    const SemanticVersion compiler = Version("0.4.0");

    const RegistryVersion *any = SelectVersion(*entry, Range("*"), compiler);
    REQUIRE(any != nullptr);
    CHECK(any->version.Text() == "0.2.0"); // 0.3.0 needs a newer compiler; 0.4.0 is yanked.

    const RegistryVersion *caret = SelectVersion(*entry, Range("^0.1.0"), compiler);
    REQUIRE(caret != nullptr);
    CHECK(caret->version.Text() == "0.1.0");

    const RegistryVersion *exact = SelectVersion(*entry, Range("=0.2.0"), compiler);
    REQUIRE(exact != nullptr);
    CHECK(exact->version.Text() == "0.2.0");
}

TEST_CASE("SelectVersion never picks a yanked release") {
    const auto entry = DecodePackageIndex(kIndexBody);
    REQUIRE(entry.has_value());
    CHECK(SelectVersion(*entry, Range("=0.4.0"), Version("0.4.0")) == nullptr);
}

TEST_CASE("SelectVersion skips a release the running compiler is too old for") {
    const auto entry = DecodePackageIndex(kIndexBody);
    REQUIRE(entry.has_value());
    CHECK(SelectVersion(*entry, Range("=0.3.0"), Version("0.4.0")) == nullptr);

    const RegistryVersion *newer = SelectVersion(*entry, Range("=0.3.0"), Version("9.1.0"));
    REQUIRE(newer != nullptr);
    CHECK(newer->version.Text() == "0.3.0");
}

TEST_CASE("SelectVersion breaks a precedence tie with build metadata") {
    const auto entry = DecodePackageIndex(R"({
      "data": {
        "namespace": "Rux", "package": "Io",
        "versions": [
          { "version": "1.0.0+1", "yanked": false },
          { "version": "1.0.0+2", "yanked": false }
        ]
      }
    })");
    REQUIRE(entry.has_value());
    const RegistryVersion *chosen = SelectVersion(*entry, Range("*"), Version("0.4.0"));
    REQUIRE(chosen != nullptr);
    CHECK(chosen->version.Text() == "1.0.0+2"); // The index is ascending, so the later entry wins.
}

TEST_CASE("SelectVersion reports nothing when no release qualifies") {
    const auto entry = DecodePackageIndex(kIndexBody);
    REQUIRE(entry.has_value());
    CHECK(SelectVersion(*entry, Range("^2.0.0"), Version("0.4.0")) == nullptr);
}

TEST_CASE("DescribeAvailableVersions names every release and marks the yanked ones") {
    const auto entry = DecodePackageIndex(kIndexBody);
    REQUIRE(entry.has_value());
    CHECK(DescribeAvailableVersions(*entry) == "0.1.0, 0.2.0, 0.3.0, 0.4.0 (yanked)");

    const auto empty = DecodePackageIndex(R"({ "data": { "namespace": "Rux", "package": "Io", "versions": [] } })");
    REQUIRE(empty.has_value());
    CHECK(DescribeAvailableVersions(*empty) == "none");
}

TEST_CASE("QualifiedIdentity uses display spelling") {
    CHECK(QualifiedIdentity(Segment("Rux"), Segment("My_Pkg")) == "Rux/My_Pkg");
}

TEST_CASE("Describe explains each failure against the registry it came from") {
    const std::string base = "https://api.rux-lang.dev";
    CHECK(Describe(RegistryError{.kind = RegistryErrorKind::Unreachable, .status = 0, .code = {}, .detail = {}}, base,
                   "Rux/Io") == "failed to reach the registry at https://api.rux-lang.dev");
    CHECK(Describe(
              RegistryError{
                  .kind = RegistryErrorKind::NotFound, .status = 404, .code = "package_not_found", .detail = {}},
              base, "Rux/Io") == "Rux/Io is not published on https://api.rux-lang.dev");
    CHECK(Describe(RegistryError{.kind = RegistryErrorKind::Rejected,
                                 .status = 429,
                                 .code = "rate_limited",
                                 .detail = "Retry shortly."},
                   base, "Rux/Io") == "https://api.rux-lang.dev is rate-limiting requests; retry shortly");
    CHECK(Describe(RegistryError{.kind = RegistryErrorKind::Rejected,
                                 .status = 503,
                                 .code = "resolver_index_unavailable",
                                 .detail = "The index is unavailable."},
                   base, "Rux/Io") == "The index is unavailable.");
}

TEST_CASE("CompilerVersion reports a usable release") {
    CHECK_FALSE(CompilerVersion().Empty());
}
