// `return`, `fail`, and unit completion against native return types, and diverging bodies that never decide the type
// of the expression they stand in.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// The errors in `source` other than the pending rejection of the native forms still awaiting their semantics.
std::vector<std::string> Errors(const std::string &source) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error &&
            !diagnostic.message.contains("is not supported yet")) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

bool AnyContains(const std::vector<std::string> &errors, const std::string_view text) {
    return std::ranges::any_of(errors, [&](const std::string &error) { return error.contains(text); });
}
} // namespace

TEST_CASE("fail selects the outer failure channel of the enclosing function") {
    CHECK(Errors(R"(
        struct E {}
        func Fails(error: E) -> int32 ! E { fail error; }
        func UnitError() -> int32 ! () { fail (); }
        func NestedError(error: E) -> int32 ! (int32 ! E) { fail .Failure(error); }
        func OptionalError() -> int32 ! (E?) { fail none; }
    )")
              .empty());

    const auto wrongPayload = Errors(R"(
        struct E {}
        struct Other {}
        func Fails() -> int32 ! E { fail Other {}; }
    )");
    CHECK(AnyContains(wrongPayload, "'fail' value must have type 'E', but found 'Other'"));

    const auto outside = Errors(R"(
        struct E {}
        func Plain() -> int32 { fail E {}; }
        func Nothing() { fail E {}; }
    )");
    CHECK(AnyContains(outside, "'fail' needs an enclosing fallible function, but this function returns 'int32'"));
    CHECK(AnyContains(outside, "'fail' needs an enclosing fallible function, but this function returns no value"));
}

TEST_CASE("a unit success completes without a written value") {
    CHECK(Errors(R"(
        struct E {}
        func Shorthand() -> ! E {}
        func Spelled() -> () ! E {}
        func Early(stop: bool) -> ! E {
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
        func OptionalCompletion() -> (() ! E)? {}
        func EarlyOptional() -> ()? { return; }
    )");
    CHECK(AnyContains(contained, "function 'OptionalUnit' must return a value of type '()?'"));
    CHECK(AnyContains(contained, "function 'SumUnit' must return a value of type '() | int32'"));
    CHECK(AnyContains(contained, "function 'OptionalCompletion' must return a value of type '(! E)?'"));
    CHECK(AnyContains(contained, "'return' requires a value of type '()?'"));
}

TEST_CASE("a non-unit success still needs a value, and Core::Unit is not the unit") {
    const auto errors = Errors(R"(
        struct E {}
        struct Unit {}
        func Missing() -> int32 ! E {}
        func Named() -> Unit ! E {}
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
    CHECK(errors.front().contains("func() -> opaque"));
}

TEST_CASE("return converts through the native rules and refuses an ambiguous meaning") {
    const auto errors = Errors(R"(
        struct E {}
        type R = int32 ! E;
        func Ambiguous(value: R) -> (int32 | R) ! E { return value; }
        func Explicit(value: R) -> (int32 | R) ! E { return .Success(value); }
        func Plain(value: int32) -> int32? ! E { return value; }
    )");
    REQUIRE_EQ(errors.size(), 1);
    CHECK(errors.front().contains("is ambiguous"));
}

TEST_CASE("diverging bodies never decide the type of the expression they stand in") {
    CHECK(Errors(R"(
        #NoReturn()
        func Stop() {
            loop {}
        }
        func Pick(value: int32) -> int32 {
            let chosen: int32 = match value {
                1 => 10i32,
                2 => return 20i32,
                3 => Stop(),
                else => 30i32
            };
            loop {
                let next: int32 = match chosen {
                    10 => break,
                    else => chosen
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
        func Main() -> ! E {}
    )")
              .empty());
    CHECK(Errors(R"(
        struct E {}
        func Main() -> int ! E { return 0; }
    )")
              .empty());
}
