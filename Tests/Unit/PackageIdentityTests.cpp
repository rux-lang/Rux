#include "Package/Identity.h"

#include <doctest.h>
#include <set>
#include <string>
#include <string_view>

using namespace Rux;

namespace {
IdentitySegment Valid(const std::string_view value) {
    auto segment = IdentitySegment::Parse(value);
    REQUIRE_MESSAGE(segment.has_value(), "expected a valid identity segment: ", std::string(value));
    return *segment;
}

IdentityError Invalid(const std::string_view value) {
    auto segment = IdentitySegment::Parse(value);
    REQUIRE_MESSAGE(!segment.has_value(), "expected an invalid identity segment: ", std::string(value));
    return segment.error();
}
} // namespace

TEST_CASE("Identity segments preserve display spelling and expose normalization") {
    const auto segment = Valid("Foo_Bar-9");

    CHECK(segment.Text() == "Foo_Bar-9");
    CHECK(segment.Normalized() == "foo-bar-9");
    CHECK_FALSE(segment.Empty());
}

TEST_CASE("Identity segments accept the documented grammar and its boundaries") {
    for (const std::string_view value : {"a", "9", "9zip", "Rux", "My_Pkg", "alpha-beta_gamma"}) {
        CHECK_MESSAGE(IdentitySegment::Parse(value).has_value(), "expected valid: ", std::string(value));
    }

    const std::string longest(identitySegmentMaxLength, 'a');
    CHECK(IdentitySegment::Parse(longest).has_value());
}

TEST_CASE("Identity segments reject empty and oversized values") {
    CHECK(Invalid("").kind == IdentityErrorKind::Empty);

    const std::string tooLong(identitySegmentMaxLength + 1, 'a');
    CHECK(Invalid(tooLong).kind == IdentityErrorKind::TooLong);
}

TEST_CASE("Identity segments reject separators at the boundaries") {
    CHECK(Invalid("-alpha").kind == IdentityErrorKind::LeadingSeparator);
    CHECK(Invalid("_alpha").kind == IdentityErrorKind::LeadingSeparator);

    const auto trailing = Invalid("alpha-");
    CHECK(trailing.kind == IdentityErrorKind::TrailingSeparator);
    CHECK(trailing.offset == 5);
    CHECK(Invalid("alpha_").kind == IdentityErrorKind::TrailingSeparator);
}

TEST_CASE("Identity segments reject every adjacent separator pair") {
    for (const std::string_view value : {"alpha--beta", "alpha__beta", "alpha-_beta", "alpha_-beta"}) {
        const auto error = Invalid(value);
        CHECK_MESSAGE(error.kind == IdentityErrorKind::AdjacentSeparators, "value: ", std::string(value));
        CHECK(error.offset == 6);
    }
}

TEST_CASE("Identity segments reject characters outside the ASCII grammar") {
    // "café" in UTF-8: the first non-ASCII byte is at offset 3.
    const std::string nonAscii = std::string("caf") + static_cast<char>(0xC3) + static_cast<char>(0xA9);

    struct Case {
        std::string value;
        std::size_t offset;
    };

    const Case cases[] = {{"alpha beta", 5}, {"alpha.beta", 5}, {"alpha/beta", 5},
                          {"alpha:beta", 5}, {"alpha+beta", 5}, {nonAscii, 3}};

    for (const auto &[value, offset] : cases) {
        const auto error = Invalid(value);
        CHECK_MESSAGE(error.kind == IdentityErrorKind::InvalidCharacter, "value: ", value);
        CHECK(error.offset == offset);
    }
}

TEST_CASE("Normalized identity collisions compare and order as one package") {
    const auto reference = Valid("Foo_Bar");
    std::set<IdentitySegment> unique;
    unique.insert(reference);

    for (const std::string_view spelling : {"foo-bar", "FOO-BAR", "foo_bar"}) {
        const auto segment = Valid(spelling);
        CHECK_MESSAGE(segment == reference, "spelling: ", std::string(spelling));
        // Display spelling survives normalization.
        CHECK(segment.Text() == std::string(spelling));
        unique.insert(segment);
    }

    CHECK(unique.size() == 1);
    CHECK(unique.begin()->Text() == "Foo_Bar");
}

TEST_CASE("Identity segments that normalize differently stay distinct") {
    CHECK_FALSE(Valid("foo-bar") == Valid("foobar"));
    CHECK(NormalizeIdentity("Rux/My_Pkg") == "rux/my-pkg");
}

TEST_CASE("Identity errors explain themselves") {
    CHECK(Describe(IdentityError{IdentityErrorKind::LeadingSeparator, 0}).find("cannot start") != std::string::npos);
    CHECK(Describe(IdentityError{IdentityErrorKind::TooLong, 64}).find("64") != std::string::npos);

    CHECK(DescribeIdentity("package name", "bad/name", IdentityError{IdentityErrorKind::InvalidCharacter, 3}) ==
          "package name 'bad/name' contains '/' at byte 4");
    CHECK(DescribeIdentity("namespace", "bad-", IdentityError{IdentityErrorKind::TrailingSeparator, 3}) ==
          "namespace 'bad-' ends with separator '-'");
    CHECK(identitySegmentConstraint.contains("ASCII letters or digits"));
}
