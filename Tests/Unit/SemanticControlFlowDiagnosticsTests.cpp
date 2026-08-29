#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeControlFlow(const std::string &source) {
    Lexer lexer(source, "control-flow.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "control-flow.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

bool HasMessage(const std::vector<SemanticDiagnostic> &diagnostics, const std::string &message) {
    return std::ranges::any_of(diagnostics,
                               [&](const SemanticDiagnostic &diagnostic) { return diagnostic.message == message; });
}
} // namespace

TEST_CASE("return diagnostics distinguish arity type and incomplete control-flow paths") {
    const auto diagnostics = AnalyzeControlFlow(R"(
        enum Result { Yes, No }

        func Missing(flag: bool) -> int {
            if flag { return 1; }
        }
        func Bare() -> int { return; }
        func Valueless() { return 1; }
        func Wrong() -> int { return true; }
        func Complete(flag: bool) -> int {
            if flag { return 1; } else { return 2; }
        }
        func Matched(result: Result) -> int {
            match result {
                .Yes => { return 1; },
                .No => { return 0; }
            }
        }
        func Forever() -> int { while true {} }
        func Escapes() -> int { while true { break; } }
    )");

    CHECK(HasMessage(diagnostics, "function 'Missing' must return a value of type 'int' on every control-flow path"));
    CHECK(HasMessage(diagnostics, "'return' requires a value of type 'int'"));
    CHECK(HasMessage(diagnostics, "'return' cannot have a value in a function with no return type"));
    CHECK(HasMessage(diagnostics, "'return' value must have type 'int', but found 'bool8'"));
    CHECK_FALSE(
        HasMessage(diagnostics, "function 'Complete' must return a value of type 'int' on every control-flow path"));
    CHECK_FALSE(
        HasMessage(diagnostics, "function 'Matched' must return a value of type 'int' on every control-flow path"));
    CHECK_FALSE(
        HasMessage(diagnostics, "function 'Forever' must return a value of type 'int' on every control-flow path"));
    CHECK(HasMessage(diagnostics, "function 'Escapes' must return a value of type 'int' on every control-flow path"));
}

TEST_CASE("conditions and loop exits name the construct and the offending type or label") {
    const auto diagnostics = AnalyzeControlFlow(R"(
        func Conditions() {
            if 1 {} else if "text" {}
            while 2 {}
            do {} while 3;
            let selected = 4 ? 1 : 2;
        }
        func Exits() {
            break;
            continue;
            loop {
                break missing;
                continue missing;
                break;
            }
        }
    )");

    CHECK(HasMessage(diagnostics, "condition for 'if' must have type 'bool', but found 'int'"));
    CHECK(HasMessage(diagnostics, "condition for 'else if' must have type 'bool', but found 'Slice<char8>'"));
    CHECK(HasMessage(diagnostics, "condition for 'while' must have type 'bool', but found 'int'"));
    CHECK(HasMessage(diagnostics, "condition for 'do-while' must have type 'bool', but found 'int'"));
    CHECK(HasMessage(diagnostics, "condition for '?:' must have type 'bool', but found 'int'"));
    CHECK(HasMessage(diagnostics, "'break' can only be used inside 'while', 'for', or 'loop'"));
    CHECK(HasMessage(diagnostics, "'continue' can only be used inside 'while', 'for', or 'loop'"));
    CHECK(HasMessage(diagnostics, "'break' refers to unknown loop label 'missing'"));
    CHECK(HasMessage(diagnostics, "'continue' refers to unknown loop label 'missing'"));
}

TEST_CASE("match diagnostics validate guards patterns duplicates reachability and enum coverage") {
    const auto diagnostics = AnalyzeControlFlow(R"(
        enum Mode { Fast, Slow, Safe }

        func Inspect(mode: Mode) -> int {
            match mode {
                .Fast if 1 => 1,
                .Fast => 2,
                .Fast => 4,
                .Slow => 3
            }
            return 0;
        }

        func Literal(value: bool) {
            match value {
                1 => 0,
                flag => 1,
                true => 2
            }
        }
    )");

    CHECK(HasMessage(diagnostics, "pattern guard must have type 'bool', but found 'int'"));
    CHECK(HasMessage(diagnostics, "duplicate pattern in match"));
    CHECK(HasMessage(diagnostics, "match on 'Mode' is not exhaustive; missing Mode::Safe"));
    CHECK(HasMessage(diagnostics, "pattern has type 'int', but the matched value has type 'bool8'"));
    CHECK(HasMessage(diagnostics, "match arm is unreachable because an earlier pattern matches every value"));
}

TEST_CASE("case pattern diagnostics distinguish enum enumerators from variant cases") {
    const auto diagnostics = AnalyzeControlFlow(R"(
        enum Mode { Fast, Slow }
        variant Signal { Waiting, Ready }
        variant Event {
            Empty,
            Pair(int, bool),
            Named { value: int; flag: bool; }
        }

        func EnumPayload(mode: Mode) {
            match mode {
                Mode::Fast(value) => 1,
                .Slow => 2
            }
        }

        func MissingEnum(mode: Mode) {
            match mode {
                Mode::Missing => 1,
                else => 2
            }
        }

        func MissingVariant(signal: Signal) {
            match signal {
                Signal::Missing => 1,
                else => 2
            }
        }

        func UnitPayload(signal: Signal) {
            match signal {
                Signal::Ready(value) => 1,
                else => 2
            }
        }

        func WrongOwner(mode: Mode) {
            match mode {
                Signal::Waiting => 1,
                else => 2
            }
        }

        func NamedFields(event: Event) {
            match event {
                Event::Named { value, value: other } => 1,
                Event::Pair { unknown: item, flag } => 2,
                else => 3
            }
        }

        func NotCaseType(value: int) {
            match value {
                .Ready => 1,
                else => 2
            }
        }
    )");

    CHECK(HasMessage(diagnostics, "enum enumerator 'Mode::Fast' cannot bind payload fields"));
    CHECK(HasMessage(diagnostics, "match on 'Mode' is not exhaustive; missing Mode::Fast"));
    CHECK(HasMessage(diagnostics, "enum 'Mode' has no enumerator 'Missing'"));
    CHECK(HasMessage(diagnostics, "variant 'Signal' has no case 'Missing'"));
    CHECK(HasMessage(diagnostics, "pattern for 'Signal::Ready' expects 0 fields, but found 1"));
    CHECK(HasMessage(diagnostics, "variant pattern 'Signal::Waiting' cannot match value of type 'Mode'"));
    CHECK(HasMessage(diagnostics, "duplicate field 'value' in variant pattern"));
    CHECK(HasMessage(diagnostics, "unknown field 'unknown' in variant pattern"));
    CHECK(HasMessage(diagnostics, "unknown field 'flag' in variant pattern"));
    CHECK(HasMessage(diagnostics, "cannot infer enum or variant type for shorthand pattern '.Ready' from type 'int'"));
}

TEST_CASE("enum and variant matches accept qualified contextual generic and borrowed patterns") {
    const auto diagnostics = AnalyzeControlFlow(R"(
        enum Mode { Fast, Slow }
        variant Signal { Waiting, Ready }
        variant Envelope<T> {
            Empty,
            Pair(T, bool),
            Named { value: T; flag: bool; }
        }

        func ReadMode(mode: Mode) -> int {
            return match mode {
                Mode::Fast => 1,
                .Slow => 2
            };
        }

        func ReadSignal(signal: &Signal) -> int {
            return match signal {
                Signal::Waiting => 1,
                .Ready => 2
            };
        }

        func ReadEnvelope(value: &Envelope<int>) -> int {
            return match value {
                Envelope::Empty => 0,
                .Pair(number, flag) if flag => number,
                .Pair(number, _) => number,
                .Named { value: number, flag: _ } => number
            };
        }
    )");

    for (const auto &diagnostic : diagnostics) {
        INFO(diagnostic.message);
    }
    CHECK(diagnostics.empty());
}

TEST_CASE("all-unit variants remain closed variants for coverage and reachability") {
    const auto missing = AnalyzeControlFlow(R"(
        variant Phase { Start, Middle, End }
        func Inspect(phase: Phase) -> int {
            return match phase {
                .Start => 1,
                .Middle => 2
            };
        }
    )");
    CHECK(HasMessage(missing, "match on 'Phase' is not exhaustive; missing Phase::End"));

    const auto guarded = AnalyzeControlFlow(R"(
        variant Phase { Start, End }
        func Inspect(phase: Phase, enabled: bool) -> int {
            return match phase {
                .Start if enabled => 1,
                .Start => 2,
                .End => 3,
                .End => 4
            };
        }
    )");
    CHECK(HasMessage(guarded, "duplicate pattern in match"));
    CHECK_FALSE(HasMessage(guarded, "match on 'Phase' is not exhaustive; missing Phase::Start"));
    CHECK_FALSE(HasMessage(guarded, "match on 'Phase' is not exhaustive; missing Phase::End"));
}
