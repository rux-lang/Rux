#include "Package/Version.h"

#include <doctest.h>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
SemanticVersion Version(const std::string_view value) {
    auto version = SemanticVersion::Parse(value);
    REQUIRE_MESSAGE(version.has_value(), "expected a valid version: ", std::string(value));
    return *version;
}

VersionRange Range(const std::string_view value) {
    auto range = VersionRange::Parse(value);
    REQUIRE_MESSAGE(range.has_value(), "expected a valid requirement: ", std::string(value));
    return *range;
}

// Assert that `requirement` accepts every version in `accepted` and rejects
// every version in `rejected`.
void CheckMatches(const std::string_view requirement, const std::vector<std::string_view> &accepted,
                  const std::vector<std::string_view> &rejected) {
    const auto range = Range(requirement);
    for (const auto &value : accepted) {
        CHECK_MESSAGE(range.Matches(Version(value)), std::string(requirement), " should match ", std::string(value));
    }
    for (const auto &value : rejected) {
        CHECK_MESSAGE(!range.Matches(Version(value)), std::string(requirement), " should not match ",
                      std::string(value));
    }
}
} // namespace

TEST_CASE("Strict versions expose every component and keep their source text") {
    const auto version = Version("12.34.56-alpha.1+linux.001");

    CHECK(version.Text() == "12.34.56-alpha.1+linux.001");
    CHECK(version.Major() == 12);
    CHECK(version.Minor() == 34);
    CHECK(version.Patch() == 56);
    REQUIRE(version.Prerelease().has_value());
    CHECK(*version.Prerelease() == "alpha.1");
    REQUIRE(version.Build().has_value());
    CHECK(*version.Build() == "linux.001");
    CHECK(version.IsPrerelease());
}

TEST_CASE("Strict versions accept numeric boundaries and identifier characters") {
    for (const std::string_view value :
         {"0.0.0", "0.1.0", "18446744073709551615.18446744073709551615.18446744073709551615", "1.2.3-0",
          "1.2.3-alpha-beta.9", "1.2.3+001.A-Z"}) {
        CHECK_MESSAGE(SemanticVersion::Parse(value).has_value(), "expected valid: ", std::string(value));
    }
}

TEST_CASE("Strict versions reject incomplete, leading-zero and overflowing values") {
    const auto empty = SemanticVersion::Parse("");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().kind == VersionErrorKind::Empty);

    const auto missingPatch = SemanticVersion::Parse("1.2");
    REQUIRE_FALSE(missingPatch.has_value());
    CHECK(missingPatch.error().kind == VersionErrorKind::MissingComponent);
    CHECK(missingPatch.error().section == "patch");
    CHECK(missingPatch.error().offset == 3);

    const auto leadingZero = SemanticVersion::Parse("1.02.3");
    REQUIRE_FALSE(leadingZero.has_value());
    CHECK(leadingZero.error().kind == VersionErrorKind::LeadingZero);
    CHECK(leadingZero.error().section == "minor");
    CHECK(leadingZero.error().offset == 2);

    const auto overflow = SemanticVersion::Parse("18446744073709551616.0.0");
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().kind == VersionErrorKind::NumericOverflow);
    CHECK(overflow.error().section == "major");
}

TEST_CASE("Strict versions reject invalid core and suffix syntax") {
    for (const std::string_view value : {"v1.2.3", "1.2.3 ", "1.2.3.4", "1.2.3-", "1.2.3+", "1.2.3-alpha..1",
                                         "1.2.3-alpha_1", "1.2.3-01", "1.2.3-alpha+build+extra", "1", "*"}) {
        CHECK_MESSAGE(!SemanticVersion::Parse(value).has_value(), "expected invalid: ", std::string(value));
    }
}

TEST_CASE("Version precedence follows the Semantic Versioning specification") {
    const std::vector<std::string_view> ordered = {"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                                                   "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};

    for (std::size_t i = 0; i + 1 < ordered.size(); ++i) {
        const auto lower = Version(ordered[i]);
        const auto higher = Version(ordered[i + 1]);
        CHECK_MESSAGE(SemanticVersion::ComparePrecedence(lower, higher) < 0, std::string(ordered[i]),
                      " should precede ", std::string(ordered[i + 1]));
        CHECK(SemanticVersion::ComparePrecedence(higher, lower) > 0);
    }

    CHECK(SemanticVersion::ComparePrecedence(Version("1.2.3"), Version("1.2.3")) == 0);
    CHECK(SemanticVersion::ComparePrecedence(Version("1.2.3"), Version("1.3.0")) < 0);
    CHECK(SemanticVersion::ComparePrecedence(Version("2.0.0"), Version("1.9.9")) > 0);
}

TEST_CASE("Build metadata is part of identity but not of precedence") {
    const auto linuxBuild = Version("1.2.3+linux");
    const auto windowsBuild = Version("1.2.3+windows");
    const auto plain = Version("1.2.3");

    CHECK_FALSE(linuxBuild == windowsBuild);
    CHECK_FALSE(linuxBuild == plain);
    CHECK(linuxBuild == Version("1.2.3+linux"));

    CHECK(SemanticVersion::ComparePrecedence(linuxBuild, windowsBuild) == 0);
    CHECK(SemanticVersion::ComparePrecedence(linuxBuild, plain) == 0);
}

TEST_CASE("Requirements preserve their source text and accept the documented grammar") {
    for (const std::string_view value :
         {"*", "x", "X", "1", "1.2", "1.2.3", "^1.2.3", "~1.2", "=1.2.3", ">= 1.2.3, < 2.0.0", ">=1.2.0,<2.0.0", "1.*",
          "1.x.*", "1.2.X", ">=1.2.3-alpha.1+ignored"}) {
        const auto range = VersionRange::Parse(value);
        REQUIRE_MESSAGE(range.has_value(), "expected valid: ", std::string(value));
        CHECK(range->Text() == std::string(value));
    }

    CHECK(Range("*").IsWildcard());
    CHECK(Range("x").IsWildcard());
    CHECK_FALSE(Range("1.*").IsWildcard());
    CHECK(Range(">=1.2.0, <2.0.0").Comparators().size() == 2);
}

TEST_CASE("Requirements reject unsupported and malformed syntax") {
    for (const std::string_view value : {"", "   ", "*.*", "*, >=1", "1 || 2", "1.2 - 2.0", ">=1 <2", ">=", "1..2",
                                         "1.*.3", "1.2.3,", "1.2.3-alpha_1", "1.2-alpha", "01.2.3", "1.2.3.4"}) {
        CHECK_MESSAGE(!VersionRange::Parse(value).has_value(), "expected invalid: ", std::string(value));
    }
}

TEST_CASE("Requirement errors expose stable kinds and byte offsets") {
    const auto wildcard = VersionRange::Parse("*, >=1");
    REQUIRE_FALSE(wildcard.has_value());
    CHECK(wildcard.error().kind == VersionErrorKind::WildcardMustStandAlone);
    CHECK(wildcard.error().offset == 0);

    const auto spaced = VersionRange::Parse(">=1.0 <2.0");
    REQUIRE_FALSE(spaced.has_value());
    CHECK(spaced.error().kind == VersionErrorKind::InvalidCharacter);
    CHECK(spaced.error().offset == 5);

    const auto trailingComma = VersionRange::Parse("1.2.3,  ");
    REQUIRE_FALSE(trailingComma.has_value());
    CHECK(trailingComma.error().kind == VersionErrorKind::EmptyComparator);
    CHECK(trailingComma.error().offset == 6);

    const auto afterWildcard = VersionRange::Parse("1.*.3");
    REQUIRE_FALSE(afterWildcard.has_value());
    CHECK(afterWildcard.error().kind == VersionErrorKind::ComponentAfterWildcard);

    const auto partialSuffix = VersionRange::Parse("1.2-alpha");
    REQUIRE_FALSE(partialSuffix.has_value());
    CHECK(partialSuffix.error().kind == VersionErrorKind::SuffixRequiresCompleteVersion);
}

TEST_CASE("Requirements bound the comparator count") {
    std::string maximum;
    for (std::size_t i = 0; i < versionRangeMaxComparators; ++i) {
        maximum += i == 0 ? ">=1" : ",>=1";
    }
    const auto accepted = VersionRange::Parse(maximum);
    REQUIRE(accepted.has_value());
    CHECK(accepted->Comparators().size() == versionRangeMaxComparators);

    const auto excessive = VersionRange::Parse(maximum + ",>=1");
    REQUIRE_FALSE(excessive.has_value());
    CHECK(excessive.error().kind == VersionErrorKind::TooManyComparators);
}

TEST_CASE("Caret requirements follow leftmost-nonzero compatibility") {
    CheckMatches("^1.2.3", {"1.2.3", "1.9.9"}, {"1.2.2", "2.0.0", "0.9.9"});
    CheckMatches("^0.2.3", {"0.2.3", "0.2.99"}, {"0.2.2", "0.3.0"});
    CheckMatches("^0.0.3", {"0.0.3"}, {"0.0.2", "0.0.4"});
    CheckMatches("^1.2", {"1.2.0", "1.9.0"}, {"1.1.9", "2.0.0"});
    CheckMatches("^0.0", {"0.0.0", "0.0.9"}, {"0.1.0"});
}

TEST_CASE("An operand with no operator is a caret requirement") {
    CheckMatches("1.2.3", {"1.2.3", "1.9.9"}, {"1.2.2", "2.0.0"});
    // The exact spelling still pins.
    CheckMatches("=1.2.3", {"1.2.3"}, {"1.2.4", "1.9.9"});
}

TEST_CASE("Tilde, exact, wildcard and comparison requirements match their boundaries") {
    CheckMatches("~1.2.3", {"1.2.3", "1.2.99"}, {"1.2.2", "1.3.0"});
    CheckMatches("=1.2", {"1.2.0", "1.2.99"}, {"1.1.9", "1.3.0"});
    CheckMatches("1.2.*", {"1.2.0", "1.2.99"}, {"1.1.9", "1.3.0"});
    CheckMatches(">=1.2.3, <2.0.0", {"1.2.3", "1.9.9"}, {"1.2.2", "2.0.0"});
    CheckMatches(">= 1.2.3, < 2.0.0", {"1.2.3", "1.9.9"}, {"1.2.2", "2.0.0"});
    CheckMatches(">1.2", {"1.3.0", "2.0.0"}, {"1.2.99"});
    CheckMatches("<=1.2", {"1.2.99", "0.9.0"}, {"1.3.0"});
    CheckMatches("<2.0.0", {"1.9.9", "0.0.1"}, {"2.0.0", "2.0.1"});
}

TEST_CASE("A wildcard requirement matches any stable version") {
    CheckMatches("*", {"0.0.1", "1.2.3", "99.99.99", "1.2.3+build"}, {"1.2.3-alpha"});
}

TEST_CASE("Prereleases need an explicit same-core prerelease operand") {
    CheckMatches(">=1.2.3", {"1.2.3", "2.0.0"}, {"2.0.0-alpha", "1.2.3-alpha"});
    CheckMatches(">=1.2.3-alpha", {"1.2.3-alpha", "1.2.3-beta", "1.2.3", "1.3.0"}, {"1.2.2", "1.3.0-alpha"});
    CheckMatches("^1.2.3-alpha", {"1.2.3-alpha", "1.2.3", "1.9.0"}, {"1.2.3-a", "1.4.0-beta"});
}

TEST_CASE("Build metadata does not affect requirement matching") {
    CheckMatches("=1.2.3+request-build", {"1.2.3", "1.2.3+candidate-build"}, {"1.2.4"});
    CheckMatches("^1.2.3", {"1.2.3+linux", "1.4.0+windows"}, {"1.2.2+linux"});
}

TEST_CASE("Version errors explain themselves") {
    CHECK(Describe(VersionError{VersionErrorKind::TooManyComponents, 0, {}}).find("three") != std::string::npos);
    CHECK(Describe(VersionError{VersionErrorKind::LeadingZero, 0, "minor"}).find("minor") != std::string::npos);
    CHECK(Describe(VersionError{VersionErrorKind::WildcardMustStandAlone, 0, {}}).find('*') != std::string::npos);
}
