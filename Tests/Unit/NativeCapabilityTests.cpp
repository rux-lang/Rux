// Copy, move, and equality capabilities through native sums, optionals, and fallibles: each holds whichever payload
// is active, so it supports an operation only when every payload it can hold does.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// The errors in `source` other than the pending native-type and native-value rejections.
std::vector<std::string> Errors(const std::string &source) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error &&
            !diagnostic.message.contains("is not supported in compiled code yet") &&
            !diagnostic.message.contains("is not supported yet")) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

bool AnyContains(const std::vector<std::string> &errors, const std::string_view text) {
    return std::ranges::any_of(errors, [&](const std::string &error) { return error.contains(text); });
}

constexpr std::string_view kOwned = R"(
    struct Owned { handle: int32; }
    extend Owned {
        func =(self: &var Owned, other: &Owned);
    }
)";
} // namespace

TEST_CASE("a native value of copyable payloads is copyable") {
    CHECK(Errors(R"(
        struct E {}
        func Use(optional: int32?, outcome: int32 ! E, sum: int32 | bool, nested: (int32 ! E)?) {
            let first = optional;
            let second = optional;
            let channel = outcome;
            let channelAgain = outcome;
            let member = sum;
            let memberAgain = sum;
            let wrapped = nested;
            let wrappedAgain = nested;
        }
    )")
              .empty());
}

TEST_CASE("a native value holding a move-only payload is move-only") {
    const std::string source = std::string(kOwned) + R"(
        struct E {}
        func Use(optional: Owned?, outcome: int32 ! Owned, sum: Owned | int32) {
            let copied = optional;
            let failure = outcome;
            let member = sum;
        }
        func Moves(optional: Owned?, outcome: int32 ! Owned) {
            let moved <- optional;
            let failure <- outcome;
        }
    )";
    const auto errors = Errors(source);
    CHECK_EQ(std::ranges::count_if(errors, [](const std::string &error) { return error.contains("<-"); }), 3);
}

TEST_CASE("unit is copyable wherever a type parameter can be") {
    CHECK(Errors(R"(
        func Twice<T>(value: T) -> (T, T) { return (value, value); }
        func Use() {
            let pair = Twice<()>(());
            let optional: ()? = ();
            let again = optional;
            let third = optional;
        }
    )")
              .empty());
}

TEST_CASE("native values compare structurally with one type on both sides") {
    CHECK(Errors(R"(
        struct E {}
        func Compare(count: int32?, other: int32?, outcome: int32 ! E, error: E, same: int32 ! int32) -> bool {
            let absent = count == none;
            let equal = count == other;
            let different = count != other;
            let failed = outcome == .Failure(error);
            let channels = same == same;
            return absent && equal && !different && failed && channels;
        }
    )")
              .empty());
}

TEST_CASE("a comparison never injects, widens, or orders a native value") {
    const auto errors = Errors(R"(
        struct A {}
        struct B {}
        func Compare(sum: A | B, member: A, optional: int32?, wider: A | B | int32) -> bool {
            let injected = sum == member;
            let wrapped = optional == 5i32;
            let widened = sum == wider;
            let ordered = optional < optional;
            return false;
        }
    )");
    CHECK(AnyContains(errors, "operator '==' cannot compare 'A | B' with 'A'"));
    CHECK(AnyContains(errors, "operator '==' cannot compare 'int32?' with 'int32'"));
    CHECK(AnyContains(errors, "operator '==' cannot compare 'A | B' with 'A | B | int32'"));
    CHECK(AnyContains(errors, "operator '<' is not defined for 'int32?'"));
}

TEST_CASE("equality needs every payload a native value can hold to compare") {
    const auto errors = Errors(R"(
        struct View { items: int32[..]; }
        func Compare(left: View?, right: View?) -> bool {
            return left == right;
        }
    )");
    CHECK_FALSE(errors.empty());
}
