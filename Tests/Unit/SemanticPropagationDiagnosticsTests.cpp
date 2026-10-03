#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <format>
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

/// A failure propagates only from a native fallible, and `Option` is an ordinary variant the legacy protocol recognizes
/// by its cases, so every case declares the outcomes it needs.
const std::string kPropagationPrelude = R"(
    enum ParseError: int32 { Empty, Bad }
    enum IoError: int32 { Closed }
    variant Option<T> { Some(T), None }
    func Read(flag: bool) -> int32 ! ParseError { return 7i32; }
    func Lookup(flag: bool) -> Option<int32> { return Option::Some<int32>(1i32); }
)";
} // namespace

TEST_CASE("a propagated outcome evaluates to its success payload") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Doubled(flag: bool) -> int32 ! ParseError {
            let value: int32 = Read(flag)?;
            return value * 2i32;
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

TEST_CASE("propagation rejects a value that is neither a native fallible nor an optional") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Bad(flag: bool) -> int32 ! ParseError {
            let value: int32 = 3i32?;
            return value;
        }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    CHECK_EQ(diagnostics[0].message,
             "'int32' cannot be propagated with '?' because it is neither a native fallible nor an optional");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "'?' propagates a native fallible 'T ! E' or an optional 'T?'");
}

TEST_CASE("propagation conventions accept custom Option names and non-generic cases") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Maybe<T> { Some(T), None }
        variant LocalMaybe { Some(int32), None }

        func Lookup() -> Maybe<int32> { return Maybe::Some<int32>(8i32); }
        func LookupLocal(value: LocalMaybe) -> LocalMaybe {
            let item = value?;
            return LocalMaybe::Some(item);
        }
        func Optional() -> Maybe<int32> {
            let value = Lookup()?;
            return Maybe::Some<int32>(value);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("a Result-shaped variant is an ordinary variant that cannot be propagated") {
    const auto diagnostics = AnalyzeSource(R"(
        enum ParseError: int32 { Bad }
        variant Attempt<T, E> { Success(T), Error(E) }
        variant LocalAttempt { Success(int32), Error(ParseError) }

        func Read() -> Attempt<int32, ParseError> {
            return Attempt::Success<int32, ParseError>(7i32);
        }
        func Generic() -> Attempt<int32, ParseError> {
            let value = Read()?;
            return Attempt::Success<int32, ParseError>(value);
        }
        func ReadLocal(value: LocalAttempt) -> LocalAttempt {
            let item = value?;
            return LocalAttempt::Success(item);
        }
    )");

    for (const std::string_view type : {"Attempt<int32, ParseError>", "LocalAttempt"}) {
        const SemanticDiagnostic &rejected = RequireDiagnostic(
            diagnostics,
            std::format("'{}' cannot be propagated with '?' because it is neither a native fallible nor an optional",
                        type));
        REQUIRE_EQ(rejected.notes.size(), 1);
        CHECK_EQ(rejected.notes[0], "a variant with 'Success' and 'Error' cases is an ordinary variant; only a native "
                                    "fallible propagates a failure");
        REQUIRE(rejected.help.has_value());
        CHECK_EQ(*rejected.help, "return 'T ! E' and propagate a native fallible, or match the variant");
    }
}

TEST_CASE("scalar enums cannot impersonate an Option propagation variant") {
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
        diagnostics,
        "'ResultLookalike' cannot be propagated with '?' because it is neither a native fallible nor an optional");
    CHECK(result.notes.empty());
    const SemanticDiagnostic &option = RequireDiagnostic(
        diagnostics,
        "'OptionLookalike' cannot be propagated with '?' because it is neither a native fallible nor an optional");
    REQUIRE_EQ(option.notes.size(), 1);
    CHECK_EQ(option.notes[0],
             "type 'OptionLookalike' uses a scalar enum for the Option protocol; declare it with 'variant'");
}

TEST_CASE("a scalar enum cannot carry a propagated absence from a valid variant") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        enum ReturnLookalike: uint8 { None = 1, Some = 2 }
        func Bad(flag: bool) -> ReturnLookalike {
            let value = Lookup(flag)?;
            return ReturnLookalike::Some;
        }
    )");

    const SemanticDiagnostic &diagnostic = RequireDiagnostic(
        diagnostics, "'?' propagates an Option, but the enclosing function returns 'ReturnLookalike'");
    REQUIRE_EQ(diagnostic.notes.size(), 1);
    CHECK_EQ(diagnostic.notes[0],
             "type 'ReturnLookalike' uses a scalar enum for the Option protocol; declare it with 'variant'");
}

TEST_CASE("propagation variants require the complete two-case Option shape") {
    const auto diagnostics = AnalyzeSource(R"(
        variant MissingError { Success(int32), Error }
        variant PayloadNone { Some(int32), None(int32) }
        variant NamedOption { Some { value: int32; }, None }

        func First(input: MissingError) -> MissingError {
            let value = input?;
            return MissingError::Success(value);
        }
        func Second(input: PayloadNone) -> PayloadNone {
            let value = input?;
            return PayloadNone::Some(value);
        }
        func Fourth(input: NamedOption) -> NamedOption {
            let value = input?;
            return NamedOption::Some { value: value };
        }
    )");

    const SemanticDiagnostic &missingError = RequireDiagnostic(
        diagnostics,
        "'MissingError' cannot be propagated with '?' because it is neither a native fallible nor an optional");
    REQUIRE_EQ(missingError.notes.size(), 1);
    CHECK_EQ(missingError.notes[0], "a variant with 'Success' and 'Error' cases is an ordinary variant; only a native "
                                    "fallible propagates a failure");
    const SemanticDiagnostic &payloadNone = RequireDiagnostic(
        diagnostics,
        "'PayloadNone' cannot be propagated with '?' because it is neither a native fallible nor an optional");
    REQUIRE_EQ(payloadNone.notes.size(), 1);
    CHECK_EQ(payloadNone.notes[0],
             "type 'PayloadNone' is not a valid Option variant; expected exactly 'Some(T)' and payload-less 'None' "
             "cases");
    const SemanticDiagnostic &namedOption = RequireDiagnostic(
        diagnostics,
        "'NamedOption' cannot be propagated with '?' because it is neither a native fallible nor an optional");
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
    CHECK_EQ(noReturn[0].message,
             "'?' propagates native fallible 'int32 ! ParseError', but the enclosing function returns nothing");
    REQUIRE(noReturn[0].help.has_value());
    CHECK_EQ(*noReturn[0].help,
             "declare the function's error channel, as in '-> T ! E', or handle the failure with 'match'");

    const auto plainReturn = AnalyzeSource(kPropagationPrelude + R"(
        func Counted(flag: bool) -> int32 { return Read(flag)?; }
    )");

    REQUIRE_FALSE(plainReturn.empty());
    CHECK_EQ(plainReturn[0].message,
             "'?' propagates native fallible 'int32 ! ParseError', but the enclosing function returns 'int32'");
}

TEST_CASE("propagation does not cross between a native fallible and a legacy Option") {
    const auto optionInFallible = AnalyzeSource(kPropagationPrelude + R"(
        func Mixed(flag: bool) -> int32 ! ParseError {
            let value: int32 = Lookup(flag)?;
            return value;
        }
    )");

    REQUIRE_FALSE(optionInFallible.empty());
    CHECK_EQ(optionInFallible[0].message,
             "'?' propagates an Option, but the enclosing function returns 'int32 ! ParseError'");
    REQUIRE(optionInFallible[0].help.has_value());
    CHECK_EQ(*optionInFallible[0].help, "native and legacy outcomes do not convert; match the Option and return "
                                        "'.Some(...)' or 'none' explicitly");

    const auto fallibleInOption = AnalyzeSource(kPropagationPrelude + R"(
        func Mixed(flag: bool) -> Option<int32> {
            let value: int32 = Read(flag)?;
            return Option::Some<int32>(value);
        }
    )");

    REQUIRE_FALSE(fallibleInOption.empty());
    CHECK_EQ(fallibleInOption[0].message,
             "'?' propagates native fallible 'int32 ! ParseError', but the enclosing function returns 'Option<int32>'");
}

TEST_CASE("propagation never converts an error type") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Rethrown(flag: bool) -> int32 ! IoError {
            let value: int32 = Read(flag)?;
            return value;
        }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    CHECK_EQ(diagnostics[0].message,
             "'?' propagates error type 'ParseError', but the enclosing function fails with 'IoError'");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0], "'?' moves an error into the outer failure only by identity, sum member "
                                      "injection, or subset widening; it never converts an error");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "map the error to 'IoError' with '? else (e => ...)', or match the value");
}

TEST_CASE("a propagated payload keeps its own type in a chained expression") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Sum(flag: bool) -> int32 ! ParseError {
            return Read(flag)? + Read(flag)?;
        }
        func Mistyped(flag: bool) -> int32 ! ParseError {
            let value: bool = Read(flag)?;
            return 1i32;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "cannot assign 'int32' to 'bool8'");
}

TEST_CASE("propagation requires explicit transfer of a named move-only outcome") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Token { value: int32; }
        extend Token { func =(self: &var Token, other: &Token); }
        func Forward(input: int32 ! Token) -> int32 ! Token {
            let value = input?;
            return value;
        }
    )");
    const auto &error =
        RequireDiagnostic(diagnostics, "move-only value 'input' requires an explicit '<-' in propagation operand");
    CHECK_EQ(error.help, "prefix the outcome with '<-', as in '(<-input)?'");
}

TEST_CASE("propagation records an explicit operand transfer for later reads") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Token { value: int32; }
        extend Token { func =(self: &var Token, other: &Token); }
        variant Option<T> { Some(T), None }
        func Forward(input: Option<Token>) -> Option<Token> {
            let value = (<-input)?;
            let again = (<-input)?;
            return Option::Some<Token>(<-value);
        }
    )");
    RequireDiagnostic(diagnostics, "value 'input' is used after it was moved");
}

TEST_CASE("propagation validates hidden payload transfers at generic instantiation") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Option<T> { Some(T), None }
        func Forward<T>(input: Option<T>) -> Option<T> {
            let value = (<-input)?;
            return Option::Some<T>(<-value);
        }
        struct Pinned { value: int32; }
        extend Pinned {
            func =(self: &var Pinned, other: &Pinned);
            func <-(self: &var Pinned, other: Pinned);
        }
        func Read(value: &int32) {
            Forward<&int32>(Option::Some<&int32>(value));
            Forward<Pinned>(Option::None<Pinned>());
        }
    )");
    RequireDiagnostic(diagnostics, "'?' cannot extract reference payload type '&int32' from an outcome");
    RequireDiagnostic(diagnostics, "'?' cannot extract payload type 'Pinned' because moving it is prohibited");
}
