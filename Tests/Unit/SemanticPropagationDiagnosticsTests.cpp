#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(source, "propagation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "propagation.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

const SemanticDiagnostic &RequireDiagnostic(const std::vector<SemanticDiagnostic> &diagnostics,
                                            const std::string_view message) {
    const auto found = std::ranges::find(diagnostics, message, &SemanticDiagnostic::message);
    REQUIRE_MESSAGE(found != diagnostics.end(), message);
    if (found == diagnostics.end()) {
        throw std::runtime_error("missing semantic diagnostic");
    }
    return *found;
}

/// `Result` and `Option` are ordinary variants rather than built-in types, so every case declares the ones it needs.
const std::string kPropagationPrelude = R"(
    enum ParseError: int32 { Empty, Bad }
    enum IoError: int32 { Closed }
    variant Result<T, E> { Success(T), Error(E) }
    variant Option<T> { Some(T), None }
    func Read(flag: bool) -> Result<int32, ParseError> {
        return Result::Success<int32, ParseError>(7i32);
    }
    func Lookup(flag: bool) -> Option<int32> { return Option::Some<int32>(1i32); }
)";
} // namespace

TEST_CASE("a propagated Result evaluates to its success payload") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Doubled(flag: bool) -> Result<int32, ParseError> {
            let value: int32 = Read(flag)?;
            return Result::Success<int32, ParseError>(value * 2i32);
        }
        func Found(flag: bool) -> Option<int32> {
            let value: int32 = Lookup(flag)?;
            return Option::Some<int32>(value);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("the conditional operator keeps its own parse beside the propagation operator") {
    const auto diagnostics = AnalyzeSource(R"(
        func Pick(flag: bool) -> int32 { return flag ? 1i32 : 2i32; }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("propagation rejects a value that is neither a Result nor an Option") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Bad(flag: bool) -> Result<int32, ParseError> {
            let value: int32 = 3i32?;
            return Result::Success<int32, ParseError>(value);
        }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    CHECK_EQ(diagnostics[0].message,
             "'int32' cannot be propagated with '?' because it is neither a Result nor an Option");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "'?' propagates a variant shaped as 'Result<T, E>' or 'Option<T>'");
}

TEST_CASE("propagation conventions accept custom variant type names and non-generic cases") {
    const auto diagnostics = AnalyzeSource(R"(
        enum ParseError: int32 { Bad }
        variant Attempt<T, E> { Success(T), Error(E) }
        variant Maybe<T> { Some(T), None }
        variant LocalAttempt { Success(int32), Error(ParseError) }
        variant LocalMaybe { Some(int32), None }

        func Read() -> Attempt<int32, ParseError> {
            return Attempt::Success<int32, ParseError>(7i32);
        }
        func Lookup() -> Maybe<int32> { return Maybe::Some<int32>(8i32); }
        func ReadLocal(value: LocalAttempt) -> LocalAttempt {
            let item = value?;
            return LocalAttempt::Success(item);
        }
        func LookupLocal(value: LocalMaybe) -> LocalMaybe {
            let item = value?;
            return LocalMaybe::Some(item);
        }
        func Generic() -> Attempt<int32, ParseError> {
            let value = Read()?;
            return Attempt::Success<int32, ParseError>(value);
        }
        func Optional() -> Maybe<int32> {
            let value = Lookup()?;
            return Maybe::Some<int32>(value);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("scalar enums cannot impersonate Result or Option propagation variants") {
    const auto diagnostics = AnalyzeSource(R"(
        enum ResultLookalike: uint8 { Error = 1, Success = 2 }
        enum OptionLookalike: uint8 { None = 1, Some = 2 }

        func ResultValue(input: ResultLookalike) -> ResultLookalike {
            let value = input?;
            return ResultLookalike::Success;
        }
        func OptionValue(input: OptionLookalike) -> OptionLookalike {
            let value = input?;
            return OptionLookalike::Some;
        }
    )");

    const SemanticDiagnostic &result = RequireDiagnostic(
        diagnostics, "'ResultLookalike' cannot be propagated with '?' because it is neither a Result nor an Option");
    REQUIRE_EQ(result.notes.size(), 1);
    CHECK_EQ(result.notes[0],
             "type 'ResultLookalike' uses a scalar enum for the Result protocol; declare it with 'variant'");
    const SemanticDiagnostic &option = RequireDiagnostic(
        diagnostics, "'OptionLookalike' cannot be propagated with '?' because it is neither a Result nor an Option");
    REQUIRE_EQ(option.notes.size(), 1);
    CHECK_EQ(option.notes[0],
             "type 'OptionLookalike' uses a scalar enum for the Option protocol; declare it with 'variant'");
}

TEST_CASE("a scalar enum cannot carry a propagated failure from a valid variant") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        enum ReturnLookalike: uint8 { Error = 1, Success = 2 }
        func Bad(flag: bool) -> ReturnLookalike {
            let value = Read(flag)?;
            return ReturnLookalike::Success;
        }
    )");

    const SemanticDiagnostic &diagnostic =
        RequireDiagnostic(diagnostics, "'?' propagates a Result, but the enclosing function returns 'ReturnLookalike'");
    REQUIRE_EQ(diagnostic.notes.size(), 1);
    CHECK_EQ(diagnostic.notes[0],
             "type 'ReturnLookalike' uses a scalar enum for the Result protocol; declare it with 'variant'");
}

TEST_CASE("propagation variants require the complete two-case protocol shape") {
    const auto diagnostics = AnalyzeSource(R"(
        variant MissingError { Success(int32), Error }
        variant PayloadNone { Some(int32), None(int32) }
        variant ExtraResult { Success(int32), Error(int32), Pending }
        variant NamedOption { Some { value: int32; }, None }

        func First(input: MissingError) -> MissingError {
            let value = input?;
            return MissingError::Success(value);
        }
        func Second(input: PayloadNone) -> PayloadNone {
            let value = input?;
            return PayloadNone::Some(value);
        }
        func Third(input: ExtraResult) -> ExtraResult {
            let value = input?;
            return ExtraResult::Success(value);
        }
        func Fourth(input: NamedOption) -> NamedOption {
            let value = input?;
            return NamedOption::Some { value: value };
        }
    )");

    const SemanticDiagnostic &missingError = RequireDiagnostic(
        diagnostics, "'MissingError' cannot be propagated with '?' because it is neither a Result nor an Option");
    REQUIRE_EQ(missingError.notes.size(), 1);
    CHECK_EQ(missingError.notes[0],
             "type 'MissingError' is not a valid Result variant; expected exactly 'Success(T)' and 'Error(E)' cases");
    const SemanticDiagnostic &payloadNone = RequireDiagnostic(
        diagnostics, "'PayloadNone' cannot be propagated with '?' because it is neither a Result nor an Option");
    REQUIRE_EQ(payloadNone.notes.size(), 1);
    CHECK_EQ(payloadNone.notes[0],
             "type 'PayloadNone' is not a valid Option variant; expected exactly 'Some(T)' and payload-less 'None' "
             "cases");
    const SemanticDiagnostic &extraResult = RequireDiagnostic(
        diagnostics, "'ExtraResult' cannot be propagated with '?' because it is neither a Result nor an Option");
    REQUIRE_EQ(extraResult.notes.size(), 1);
    CHECK_EQ(extraResult.notes[0],
             "type 'ExtraResult' is not a valid Result variant; expected exactly 'Success(T)' and 'Error(E)' cases");
    const SemanticDiagnostic &namedOption = RequireDiagnostic(
        diagnostics, "'NamedOption' cannot be propagated with '?' because it is neither a Result nor an Option");
    REQUIRE_EQ(namedOption.notes.size(), 1);
    CHECK_EQ(namedOption.notes[0],
             "type 'NamedOption' is not a valid Option variant; expected exactly 'Some(T)' and payload-less 'None' "
             "cases");
}

TEST_CASE("propagation requires an enclosing return type that can carry the failure") {
    const auto noReturn = AnalyzeSource(kPropagationPrelude + R"(
        func Discard(flag: bool) { let value: int32 = Read(flag)?; }
    )");

    REQUIRE_FALSE(noReturn.empty());
    CHECK_EQ(noReturn[0].message, "'?' propagates a Result, but the enclosing function returns nothing");
    REQUIRE(noReturn[0].help.has_value());
    CHECK_EQ(*noReturn[0].help, "give the function a 'Result' return type, or handle the failure with 'match'");

    const auto plainReturn = AnalyzeSource(kPropagationPrelude + R"(
        func Counted(flag: bool) -> int32 { return Read(flag)?; }
    )");

    REQUIRE_FALSE(plainReturn.empty());
    CHECK_EQ(plainReturn[0].message, "'?' propagates a Result, but the enclosing function returns 'int32'");
}

TEST_CASE("propagation does not cross between Result and Option") {
    const auto optionInResult = AnalyzeSource(kPropagationPrelude + R"(
        func Mixed(flag: bool) -> Result<int32, ParseError> {
            let value: int32 = Lookup(flag)?;
            return Result::Success<int32, ParseError>(value);
        }
    )");

    REQUIRE_FALSE(optionInResult.empty());
    CHECK_EQ(optionInResult[0].message,
             "'?' propagates an Option, but the enclosing function returns 'Result<int32, ParseError>'");
    REQUIRE(optionInResult[0].help.has_value());
    CHECK_EQ(*optionInResult[0].help, "convert the Option to a Result before propagating it");

    const auto resultInOption = AnalyzeSource(kPropagationPrelude + R"(
        func Mixed(flag: bool) -> Option<int32> {
            let value: int32 = Read(flag)?;
            return Option::Some<int32>(value);
        }
    )");

    REQUIRE_FALSE(resultInOption.empty());
    CHECK_EQ(resultInOption[0].message, "'?' propagates a Result, but the enclosing function returns 'Option<int32>'");
}

TEST_CASE("propagation requires an identical error type") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Rethrown(flag: bool) -> Result<int32, IoError> {
            let value: int32 = Read(flag)?;
            return Result::Success<int32, IoError>(value);
        }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    CHECK_EQ(diagnostics[0].message,
             "'?' propagates error type 'ParseError', but the enclosing function returns error type 'IoError'");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0], "'?' does not convert between error types");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "map the error to 'IoError' before propagating it");
}

TEST_CASE("a propagated payload keeps its own type in a chained expression") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Sum(flag: bool) -> Result<int32, ParseError> {
            return Result::Success<int32, ParseError>(Read(flag)? + Read(flag)?);
        }
        func Mistyped(flag: bool) -> Result<int32, ParseError> {
            let value: bool = Read(flag)?;
            return Result::Success<int32, ParseError>(1i32);
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "cannot assign 'int32' to 'bool8'");
}
