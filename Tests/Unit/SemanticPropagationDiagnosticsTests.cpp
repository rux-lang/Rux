#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
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
    CHECK_EQ(*diagnostics[0].help, "'?' propagates a 'Result<T, E>' or an 'Option<T>'");
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
