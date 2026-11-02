// Copy, move, and equality capabilities through native sums, optionals, and fallibles: each holds whichever payload
// is active, so it supports an operation only when every payload it can hold does.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// The errors in `source`.
std::vector<std::string> Errors(const std::string &source) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

bool AnyContains(const std::vector<std::string> &errors, const std::string_view text) {
    return std::ranges::any_of(errors, [&](const std::string &error) { return error.contains(text); });
}

} // namespace

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
