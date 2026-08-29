// The iterator convention as analysis enforces it: which signatures make a type drivable by `for`, and what a subject
// that is not iterable reports.

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
    Lexer lexer(source, "iteration.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "iteration.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

/// `Option` is an ordinary variant, so every case declares the one its iterator reports the end with.
const std::string kIterationPrelude = R"(
    variant Option<T> { Some(T), None }
    struct Counter { value: int32; limit: int32; }
    extend Counter {
        func Next(self: &var Counter) -> Option<int32> {
            if self.value >= self.limit { return Option::None<int32>(); }
            let current = self.value;
            self.value = self.value + 1i32;
            return Option::Some<int32>(current);
        }
    }
)";
} // namespace

TEST_CASE("a type whose Next advances it and reports an end is iterable") {
    const auto diagnostics = AnalyzeSource(kIterationPrelude + R"(
        func Total() -> int32 {
            var counter = Counter { value: 0i32, limit: 3i32 };
            var total = 0i32;
            for item in counter { total = total + item; }
            return total;
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("a container that hands out an iterator is iterable through it") {
    const auto diagnostics = AnalyzeSource(kIterationPrelude + R"(
        struct Span { limit: int32; }
        extend Span {
            func Iterate(self: &Span) -> Counter { return Counter { value: 0i32, limit: self.limit }; }
        }
        func Total() -> int32 {
            let span = Span { limit: 3i32 };
            var total = 0i32;
            for item in span { total = total + item; }
            return total;
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("iterator completion accepts custom generic and non-generic variant type names") {
    const auto generic = AnalyzeSource(R"(
        variant Step<T> { Some(T), None }
        struct Counter { value: int32; }
        extend Counter {
            func Next(self: &var Counter) -> Step<int32> {
                return Step::None<int32>();
            }
        }
        func Walk() {
            var counter = Counter { value: 0i32 };
            for item in counter {}
        }
    )");
    CHECK(generic.empty());

    const auto concrete = AnalyzeSource(R"(
        variant Step { Some(int32), None }
        struct Counter { value: int32; }
        extend Counter {
            func Next(self: &var Counter) -> Step { return Step::None(); }
        }
        func Walk() {
            var counter = Counter { value: 0i32 };
            for item in counter {}
        }
    )");
    CHECK(concrete.empty());
}

TEST_CASE("a scalar enum cannot report iterator completion") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Step: uint8 { None = 1, Some = 2 }
        struct Counter { value: int32; }
        extend Counter {
            func Next(self: &var Counter) -> Step { return Step::None; }
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "iterator method 'Next' on 'Counter' must return an Option-shaped variant");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0],
             "type 'Step' uses a scalar enum for the Option protocol; declare it with 'variant'");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "return a variant with exactly 'Some(T)' and payload-less 'None' cases");
}

TEST_CASE("iterator completion rejects malformed Option-shaped variants") {
    const auto payloadlessSome = AnalyzeSource(R"(
        variant Step { Some, None }
        struct Counter { value: int32; }
        extend Counter {
            func Next(self: &var Counter) -> Step { return Step::None(); }
        }
    )");
    REQUIRE_EQ(payloadlessSome.size(), 1);
    REQUIRE_EQ(payloadlessSome[0].notes.size(), 1);
    CHECK_EQ(payloadlessSome[0].notes[0],
             "type 'Step' is not a valid Option variant; expected exactly 'Some(T)' and payload-less 'None' cases");

    const auto payloadNone = AnalyzeSource(R"(
        variant Step { Some(int32), None(int32) }
        struct Counter { value: int32; }
        extend Counter {
            func Next(self: &var Counter) -> Step { return Step::None(0i32); }
        }
    )");
    REQUIRE_EQ(payloadNone.size(), 1);
    REQUIRE_EQ(payloadNone[0].notes.size(), 1);
    CHECK_EQ(payloadNone[0].notes[0],
             "type 'Step' is not a valid Option variant; expected exactly 'Some(T)' and payload-less 'None' cases");

    const auto extraCase = AnalyzeSource(R"(
        variant Step { Some(int32), None, Paused }
        struct Counter { value: int32; }
        extend Counter {
            func Next(self: &var Counter) -> Step { return Step::None(); }
        }
    )");
    REQUIRE_EQ(extraCase.size(), 1);
    REQUIRE_EQ(extraCase[0].notes.size(), 1);
    CHECK_EQ(extraCase[0].notes[0],
             "type 'Step' is not a valid Option variant; expected exactly 'Some(T)' and payload-less 'None' cases");
}

TEST_CASE("the loop variable takes the item type the iterator reports") {
    const auto diagnostics = AnalyzeSource(kIterationPrelude + R"(
        func Mistyped() -> int32 {
            var counter = Counter { value: 0i32, limit: 3i32 };
            var total = 0i32;
            for item in counter { total = total + item as int32; }
            var flag: bool = false;
            for other in counter { flag = other; }
            return total;
        }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    CHECK_EQ(diagnostics[0].message, "cannot assign 'int32' to 'bool8'");
}

TEST_CASE("a Next that cannot advance its receiver is rejected at its declaration") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Option<T> { Some(T), None }
        struct Counter { value: int32; }
        extend Counter {
            func Next(self: &Counter) -> Option<int32> { return Option::None<int32>(); }
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "iterator method 'Next' on 'Counter' must take a mutable receiver");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0],
             "advancing an iterator writes it, so 'Next' cannot borrow its receiver read-only");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "write the receiver as 'self: &var Counter'");
}

TEST_CASE("a Next taking arguments is rejected at its declaration") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Option<T> { Some(T), None }
        struct Counter { value: int32; }
        extend Counter {
            func Next(self: &var Counter, step: int32) -> Option<int32> { return Option::None<int32>(); }
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "iterator method 'Next' on 'Counter' takes no parameters besides its receiver");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0], "'for' calls 'Next' with no arguments");
}

TEST_CASE("a method named Next that reports no end is not the convention's") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Cursor { value: int32; }
        extend Cursor {
            func Next(self: &Cursor) -> int32 { return self.value + 1i32; }
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("methods named Next returning non-Option protocols are ordinary methods") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Event: int32 { End = 0, Error = 1 }
        variant Outcome { Success(int32), Error(int32) }
        struct Reader { value: int32; }
        struct Operation { value: int32; }
        extend Reader {
            func Next(self: &var Reader) -> Event { return Event::End; }
        }
        extend Operation {
            func Next(self: &var Operation) -> Outcome { return Outcome::Success(self.value); }
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("an Iterate that does not return an iterator is rejected at its declaration") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Span { limit: int32; }
        extend Span {
            func Iterate(self: &Span) -> int32 { return self.limit; }
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "iterator method 'Iterate' on 'Span' must return an iterator");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0], "type 'int32' has no 'Next' returning an 'Option'");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "give the returned type 'func Next(self: &var T) -> Option<Item>'");
}

TEST_CASE("a subject that is not iterable reports what iteration accepts") {
    const auto plain = AnalyzeSource(R"(
        struct Plain { value: int32; }
        func Walk() {
            let plain = Plain { value: 1i32 };
            for item in plain {}
        }
    )");

    REQUIRE_EQ(plain.size(), 1);
    CHECK_EQ(plain[0].message, "cannot iterate over 'Plain'");
    CHECK(plain[0].notes.empty());
    REQUIRE(plain[0].help.has_value());
    CHECK_EQ(*plain[0].help, "iterate an array, a slice, a range, or a type declaring 'Next' or 'Iterate'");
}

TEST_CASE("a subject that almost satisfies the convention says which part is wrong") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Cursor { value: int32; }
        extend Cursor {
            func Next(self: &Cursor) -> int32 { return self.value; }
        }
        func Walk() {
            var cursor = Cursor { value: 1i32 };
            for item in cursor {}
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "cannot iterate over 'Cursor'");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK_EQ(diagnostics[0].notes[0],
             "type 'Cursor' declares 'Next', but not as 'func Next(self: &var Cursor) -> Option<T>' returning an "
             "Option-shaped variant");
}

TEST_CASE("arrays and ranges keep their own iteration") {
    // Slices need `Core::Slice` to name, so the language test covers them; what matters here is that neither form is
    // routed through the convention.
    const auto diagnostics = AnalyzeSource(R"(
        func Walk(values: int32[4]) -> int32 {
            var total = 0i32;
            for value in values { total = total + value; }
            for index in 0..4 { total = total + index as int32; }
            return total;
        }
    )");

    CHECK(diagnostics.empty());
}
