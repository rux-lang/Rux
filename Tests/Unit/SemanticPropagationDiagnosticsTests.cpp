#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
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

/// A failure propagates only from a native fallible and absence only from a native optional, so the prelude declares
/// one of each.
const std::string kPropagationPrelude = R"(
    enum ParseError: int32 { Empty, Bad }
    enum IoError: int32 { Closed }
    func Read(flag: bool) -> int32 throws ParseError { return 7i32; }
    func Lookup(flag: bool) -> int32? { return 1i32; }
)";
} // namespace

TEST_CASE("a propagated outcome evaluates to its success payload") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Doubled(flag: bool) -> int32 throws ParseError {
            let value: int32 = try Read(flag);
            return value * 2i32;
        }
        func Found(flag: bool) -> int32? {
            let value: int32 = Lookup(flag)?;
            return value;
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("the conditional operator keeps its own parse beside the propagation operator") {
    const auto diagnostics = AnalyzeSource(R"(
        func Pick(flag: bool) -> int32 { return if flag { 1i32 } else { 2i32 }; }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("propagation rejects a value that is not an optional") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Bad(flag: bool) -> int32 throws ParseError {
            let value: int32 = 3i32?;
            return value;
        }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    CHECK_EQ(diagnostics[0].message, "'int32' cannot be propagated with '?' because it is not an optional");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "'?' propagates the absence of an optional 'T?'");
}

TEST_CASE("an Option-shaped variant is an ordinary variant that cannot be propagated") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Maybe<T> { Some(T), None }
        variant LocalMaybe { Some(int32), None }

        func Lookup() -> Maybe<int32> { return Maybe::Some<int32>(8i32); }
        func Optional() -> Maybe<int32> {
            let value = Lookup()?;
            return Maybe::Some<int32>(value);
        }
        func LookupLocal(value: LocalMaybe) -> LocalMaybe {
            let item = value?;
            return LocalMaybe::Some(item);
        }
    )");

    for (const std::string_view type : {"Maybe<int32>", "LocalMaybe"}) {
        const SemanticDiagnostic &rejected = RequireDiagnostic(
            diagnostics, std::format("'{}' cannot be propagated with '?' because it is not an optional", type));
        REQUIRE_EQ(rejected.notes.size(), 1);
        CHECK_EQ(rejected.notes[0], "a variant with 'Some' and 'None' cases is an ordinary variant; only a native "
                                    "optional propagates absence");
        REQUIRE(rejected.help.has_value());
        CHECK_EQ(*rejected.help, "return 'T?' and propagate a native optional, or match the variant");
    }
}

TEST_CASE("scalar enums named like outcomes are ordinary values") {
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

    for (const std::string_view type : {"ResultLookalike", "OptionLookalike"}) {
        const SemanticDiagnostic &rejected = RequireDiagnostic(
            diagnostics, std::format("'{}' cannot be propagated with '?' because it is not an optional", type));
        CHECK(rejected.notes.empty());
    }
}

TEST_CASE("every variant with outcome case names is ordinary, whatever its payloads") {
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

    const SemanticDiagnostic &missingError =
        RequireDiagnostic(diagnostics, "'MissingError' cannot be propagated with '?' because it is not an optional");
    REQUIRE_EQ(missingError.notes.size(), 1);
    CHECK_EQ(missingError.notes[0], "a variant with 'Success' and 'Error' cases is an ordinary variant; only a native "
                                    "fallible propagates a failure");
    for (const std::string_view type : {"PayloadNone", "NamedOption"}) {
        const SemanticDiagnostic &rejected = RequireDiagnostic(
            diagnostics, std::format("'{}' cannot be propagated with '?' because it is not an optional", type));
        REQUIRE_EQ(rejected.notes.size(), 1);
        CHECK_EQ(rejected.notes[0], "a variant with 'Some' and 'None' cases is an ordinary variant; only a native "
                                    "optional propagates absence");
    }
}

TEST_CASE("a propagated payload keeps its own type in a chained expression") {
    const auto diagnostics = AnalyzeSource(kPropagationPrelude + R"(
        func Sum(flag: bool) -> int32 throws ParseError {
            return try Read(flag) + Read(flag);
        }
        func Mistyped(flag: bool) -> int32 throws ParseError {
            let value: bool = try Read(flag);
            return 1i32;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "cannot assign 'int32' to 'bool8'");
}

TEST_CASE("propagation records an explicit operand transfer for later reads") {
    const auto diagnostics = AnalyzeSource(R"(
        #NoCopy
        struct Token { value: int32; }
        extend Token { }
        func Forward(input: Token?) -> Token? {
            let value = (move input)?;
            let again = (move input)?;
            return move value;
        }
    )");
    RequireDiagnostic(diagnostics, "value 'input' is used after it was moved");
}

TEST_CASE("propagation validates hidden payload transfers at generic instantiation") {
    const auto diagnostics = AnalyzeSource(R"(
        func Forward<T>(input: T?) -> T? {
            let value = (move input)?;
            return move value;
        }
        #NoCopy
        #NoMove
        struct Pinned { value: int32; }
        extend Pinned {
        }
        func Read(value: &int32) {
            Forward<&int32>(none);
            Forward<Pinned>(none);
        }
    )");
    RequireDiagnostic(diagnostics, "'?' cannot extract reference payload type '&int32' from an outcome");
    RequireDiagnostic(diagnostics, "'?' cannot extract payload type 'Pinned' because moving it is prohibited");
}
