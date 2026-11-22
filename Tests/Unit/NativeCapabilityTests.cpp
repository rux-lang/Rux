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
        intrinsic interface Copyable;
        func Twice<T: Copyable>(value: T) -> (T, T) { return (value, value); }
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
    // A value compared with an optional of its type is made present, as wherever an optional is expected.
    CHECK_FALSE(AnyContains(errors, "operator '==' cannot compare 'int32?' with 'int32'"));
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

TEST_CASE("a member compared with its sum is bound out of it first") {
    const auto diagnostics = AnalyzeSource(R"(
        func Compare(value: bool8 | int32) -> bool {
            return value == true;
        }
    )");
    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "operator '==' cannot compare 'bool8 | int32' with 'bool8'");
    CHECK_EQ(diagnostics[0].help, "bind the member with a typed pattern and compare that, as in 'if let value: bool8 = "
                                  "...', or match the value");
}
