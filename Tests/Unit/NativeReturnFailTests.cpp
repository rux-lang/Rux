// `return`, `fail`, and unit completion against native return types, and diverging bodies that never decide the type
// of the expression they stand in.

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

TEST_CASE("a unit success completes without a written value") {
    CHECK(Errors(R"(
        struct E {}
        func Shorthand() throws E {}
        func Spelled() -> () throws E {}
        func Early(stop: bool) throws E {
            if stop {
                return;
            }
        }
        func Unit() -> () {}
        func UnitValue() -> () { return (); }
    )")
              .empty());

    // A type that merely contains the unit still needs a value.
    const auto contained = Errors(R"(
        struct E {}
        func OptionalUnit() -> ()? {}
        func SumUnit() -> () | int32 {}
        func EarlyOptional() -> ()? { return; }
    )");
    CHECK(AnyContains(contained, "function 'OptionalUnit' must return a value of type '()?'"));
    CHECK(AnyContains(contained, "function 'SumUnit' must return a value of type '() | int32'"));
    CHECK(AnyContains(contained, "'return' requires a value of type '()?'"));
}

TEST_CASE("a non-unit success still needs a value, and Core::Unit is not the unit") {
    const auto errors = Errors(R"(
        struct E {}
        struct Unit {}
        func Missing() -> int32 throws E {}
        func Named() -> Unit throws E {}
    )");
    CHECK(AnyContains(errors, "function 'Missing' must return a value of type 'int32 ! E'"));
    CHECK(AnyContains(errors, "function 'Named' must return a value of type 'Unit ! E'"));
}

TEST_CASE("a void function is not a unit-returning value") {
    const auto errors = Errors(R"(
        func Void() {}
        func Unit() -> () {}
        func Use() {
            let unit: func() -> () = Unit;
            let mismatched: func() -> () = Void;
        }
    )");
    REQUIRE_EQ(errors.size(), 1);
    CHECK(errors.front().contains("'func()'"));
}

TEST_CASE("diverging bodies never decide the type of the expression they stand in") {
    CHECK(Errors(R"(
        #NoReturn
        func Stop() {
            loop {}
        }
        func Pick(value: int32) -> int32 {
            let chosen: int32 = match value {
                1 => 10i32,
                2 => return 20i32,
                3 => Stop(),
                _ => 30i32
            };
            loop {
                let next: int32 = match chosen {
                    10 => break,
                    _ => chosen
                };
            }
            return chosen;
        }
    )")
              .empty());
}

TEST_CASE("a fallible entry point is accepted") {
    CHECK(Errors(R"(
        struct E {}
        func Main() throws E {}
    )")
              .empty());
    CHECK(Errors(R"(
        struct E {}
        func Main() -> int throws E { return 0; }
    )")
              .empty());
}
