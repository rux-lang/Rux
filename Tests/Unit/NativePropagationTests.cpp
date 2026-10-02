// `?` on a native fallible: the success continues, inner levels intact, and the error leaves as the enclosing
// function's outer failure by identity, sum injection, or subset widening only. Legacy and native outcomes never mix,
// a borrowed operand is refused, and the operand is consumed like any other by-value use.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// The errors in `source` other than the pending rejection of the native forms still awaiting their lowering.
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

/// Every error diagnostic in `source` with its help, for the cases that check one.
std::vector<SemanticDiagnostic> ErrorDiagnostics(const std::string &source) {
    std::vector<SemanticDiagnostic> errors;
    for (auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error &&
            !diagnostic.message.contains("is not supported yet")) {
            errors.push_back(std::move(diagnostic));
        }
    }
    return errors;
}

bool AnyContains(const std::vector<std::string> &errors, const std::string_view text) {
    return std::ranges::any_of(errors, [&](const std::string &error) { return error.contains(text); });
}

constexpr std::string_view kDeclarations = R"(
    struct ParseError {}
    struct IoError {}
    struct Timeout {}
    struct Owned { handle: int32; }
    extend Owned {
        func =(self: &var Owned, other: &Owned);
    }
    struct Pinned { handle: int32; }
    extend Pinned {
        func =(self: &var Pinned, other: &Pinned);
        func <-(self: &var Pinned, other: Pinned);
    }
    func UseValue(value: int32) {}
)";

std::vector<std::string> ErrorsIn(const std::string_view body) {
    return Errors(std::string(kDeclarations) + std::string(body));
}
} // namespace

TEST_CASE("'?' continues with the success and keeps every inner level") {
    CHECK(ErrorsIn(R"(
        func Nested(outcome: (int32 ! ParseError) ! IoError) -> int32 ! IoError {
            let inner: int32 ! ParseError = outcome?;
            return match inner {
                .Success(value) => value,
                .Failure(_) => 0i32
            };
        }
        func Optional(outcome: int32? ! IoError) -> int32 ! IoError {
            let present: int32? = outcome?;
            return 1i32;
        }
        func Sum(outcome: (int32 | bool) ! IoError) -> int32 ! IoError {
            let member: int32 | bool = outcome?;
            return 1i32;
        }
        func Save() -> ! IoError {}
        func Unit() -> int32 ! IoError {
            let done: () = Save()?;
            Save()?;
            return 1i32;
        }
        func SameType(outcome: int32 ! int32) -> int32 ! int32 {
            let value = outcome?;
            return value;
        }
    )")
              .empty());
}

TEST_CASE("an error enters the outer failure only by identity, injection, or widening") {
    CHECK(ErrorsIn(R"(
        func Read() -> int32 ! ParseError { return 1i32; }
        func Both() -> int32 ! (ParseError | IoError) { return 1i32; }
        func Injected() -> int32 ! (ParseError | IoError) {
            return Read()?;
        }
        func Widened() -> int32 ! (ParseError | IoError | Timeout) {
            return Both()?;
        }
    )")
              .empty());

    const auto errors = ErrorDiagnostics(std::string(kDeclarations) + R"(
        func Read() -> int32 ! ParseError { return 1i32; }
        func Converted() -> int32 ! IoError {
            return Read()?;
        }
    )");
    REQUIRE_EQ(errors.size(), 1);
    CHECK_EQ(errors.front().message,
             "'?' propagates error type 'ParseError', but the enclosing function fails with 'IoError'");
    REQUIRE(errors.front().help.has_value());
    CHECK(errors.front().help->contains("'? else (e => ...)'"));
}

TEST_CASE("explicit propagation is the forwarding alternative of the ambiguous return") {
    CHECK(ErrorsIn(R"(
        type R = int32 ! ParseError;
        func Forward(value: R) -> (int32 | R) ! ParseError {
            return value?;
        }
        func KeepAsData(value: R) -> (int32 | R) ! ParseError {
            return .Success(value);
        }
    )")
              .empty());
}

TEST_CASE("legacy and native outcomes never propagate into each other") {
    const auto errors = ErrorDiagnostics(std::string(kDeclarations) + R"(
        variant Result<T, E> {
            Success(T),
            Error(E)
        }
        func Native() -> int32 ! ParseError { return 1i32; }
        func Legacy() -> Result<int32, ParseError> { return Result::Success<int32, ParseError>(1i32); }
        func NativeIntoLegacy() -> Result<int32, ParseError> {
            let value = Native()?;
            return Result::Success<int32, ParseError>(value);
        }
        func LegacyIntoNative() -> int32 ! ParseError {
            return Legacy()?;
        }
    )");
    const auto native = std::ranges::find_if(errors, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message.starts_with("'?' propagates native fallible 'int32 ! ParseError'");
    });
    REQUIRE(native != errors.end());
    REQUIRE(native->help.has_value());
    CHECK(native->help->contains("'Result::Success(...)'"));
    const auto legacy = std::ranges::find_if(errors, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message.starts_with("'?' propagates a Result, but the enclosing function returns");
    });
    REQUIRE(legacy != errors.end());
    REQUIRE(legacy->help.has_value());
    CHECK(legacy->help->contains("'.Failure(...)'"));
}

TEST_CASE("'?' needs a fallible enclosing function at the outer level") {
    const auto errors = ErrorsIn(R"(
        func Read() -> int32 ! ParseError { return 1i32; }
        func Wrapped() -> (int32 ! ParseError)? {
            let value = Read()?;
            return .Some(.Success(value));
        }
        func Plain() -> int32 {
            return Read()?;
        }
    )");
    CHECK(AnyContains(errors, "'?' propagates native fallible 'int32 ! ParseError', but the enclosing function "
                              "returns '(int32 ! ParseError)?'"));
    CHECK(AnyContains(errors, "but the enclosing function returns 'int32'"));
}

TEST_CASE("'?' consumes its operand and moves both payloads") {
    CHECK(ErrorsIn(R"(
        func Transfer(outcome: Owned ! ParseError) -> int32 ! ParseError {
            let owned = (<-outcome)?;
            return owned.handle;
        }
        func Copies(outcome: int32 ! ParseError) -> int32 ! ParseError {
            let first = outcome?;
            let second = outcome?;
            return first + second;
        }
    )")
              .empty());

    const auto implicit = ErrorsIn(R"(
        func Implicit(outcome: Owned ! ParseError) -> int32 ! ParseError {
            let owned = outcome?;
            return owned.handle;
        }
    )");
    CHECK(AnyContains(implicit, "requires an explicit '<-' in propagation operand"));

    const auto borrowed = ErrorsIn(R"(
        func Borrowed(outcome: &(int32 ! ParseError)) -> int32 ! ParseError {
            return outcome?;
        }
    )");
    CHECK(AnyContains(borrowed, "'?' cannot consume the borrowed fallible '&(int32 ! ParseError)'"));
}

TEST_CASE("a payload that prohibits moving cannot be propagated, even through a generic") {
    const auto direct = ErrorsIn(R"(
        func Direct(outcome: Pinned ! ParseError) -> int32 ! ParseError {
            let pinned = (<-outcome)?;
            return 1i32;
        }
    )");
    CHECK(AnyContains(direct, "'?' cannot extract payload type 'Pinned' because moving it is prohibited"));

    const auto generic = ErrorsIn(R"(
        func Unwrap<T>(outcome: T ! ParseError) -> T ! ParseError {
            let value = (<-outcome)?;
            return <-value;
        }
        func Use(outcome: Pinned ! ParseError) {
            let result = Unwrap<Pinned>(<-outcome);
        }
    )");
    CHECK(AnyContains(generic, "'?' cannot extract payload type 'Pinned' because moving it is prohibited"));
}
