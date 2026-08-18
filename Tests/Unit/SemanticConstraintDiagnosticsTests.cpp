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
    Lexer lexer(source, "constraints.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "constraints.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

/// A displayable type, its conforming implementation, and a generic that needs the bound. Every case below varies one
/// part of this, so the shared prefix keeps each source down to what it is actually testing.
const std::string kDisplayPrelude = R"(
    interface Display {
        func Show() -> int;
    }
    struct Labeled { value: int; }
    extend Labeled : Display {
        func Show(self: *Labeled) -> int { return self.value; }
    }
    struct Plain { value: int; }
    func Render<T: Display>(value: T) -> int { return 0; }
)";
} // namespace

TEST_CASE("an unsatisfied interface bound names the missing operation at the call site") {
    const auto diagnostics = AnalyzeSource(kDisplayPrelude + R"(
        func Main() {
            let plain = Plain { value: 1 };
            Render<Plain>(plain);
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message,
             "type argument 'Plain' does not satisfy interface bound 'Display' on type parameter 'T'");
    REQUIRE_EQ(diagnostics[0].notes.size(), 2);
    CHECK_EQ(diagnostics[0].notes[0],
             "interface 'Display' requires method 'Show', which type 'Plain' does not implement");
    CHECK_EQ(diagnostics[0].notes[1], "type parameter 'T' of function 'Render' is bound by 'Display'");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "implement the interface, as in 'extend Plain: Display { ... }'");
    // The call, not the declaration: line 14 is `Render<Plain>(plain);`.
    CHECK_EQ(diagnostics[0].location.line, 14);
}

TEST_CASE("a type that implements the bound interface satisfies the constraint") {
    const auto diagnostics = AnalyzeSource(kDisplayPrelude + R"(
        func Main() {
            let labeled = Labeled { value: 1 };
            Render<Labeled>(labeled);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("a type declaring every required method satisfies the bound without a nominal implementation") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Structural { value: int; }
        extend Structural {
            func Show(self: *Structural) -> int { return self.value; }
        }
        func Render<T: Display>(value: T) -> int { return 0; }
        func Main() {
            let structural = Structural { value: 1 };
            Render<Structural>(structural);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("an interface requiring nothing is satisfied by every type argument") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Marker {}
        func Tag<T: Marker>(value: T) -> int { return 0; }
        func Main() { Tag<int>(1); }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("every bound of a multiply constrained parameter is checked independently") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        interface Sized {
            func Size() -> int;
        }
        struct Half { value: int; }
        extend Half : Display {
            func Show(self: *Half) -> int { return self.value; }
        }
        func Render<T: Display + Sized>(value: T) -> int { return 0; }
        func Main() {
            let half = Half { value: 1 };
            Render<Half>(half);
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message,
             "type argument 'Half' does not satisfy interface bound 'Sized' on type parameter 'T'");
    CHECK_EQ(diagnostics[0].notes[0], "interface 'Sized' requires method 'Size', which type 'Half' does not implement");
}

TEST_CASE("a bound naming an undefined interface is reported where it is written") {
    const auto diagnostics = AnalyzeSource(R"(
        func Render<T: Displya>(value: T) -> int { return 0; }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "interface 'Displya' is not defined");
    CHECK_EQ(diagnostics[0].location.line, 2);
    CHECK_EQ(diagnostics[0].location.column, 24);
}

TEST_CASE("a bound naming something other than an interface says what the name is") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Labeled { value: int; }
        func Render<T: Labeled>(value: T) -> int { return 0; }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "name 'Labeled' is a type, not an interface, and cannot bound type parameter 'T'");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK(diagnostics[0].notes[0].contains("Labeled"));
}

TEST_CASE("a repeated bound and a bound with type arguments are both rejected at the declaration") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        func Twice<T: Display + Display>(value: T) -> int { return 0; }
        func Parameterized<T: Display<int>>(value: T) -> int { return 0; }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "type parameter 'T' repeats interface bound 'Display'");
    CHECK_EQ(diagnostics[1].message, "interface bound 'Display' cannot take type arguments");
}

TEST_CASE("a type parameter passed on as a type argument must carry the bound it is used for") {
    const auto diagnostics = AnalyzeSource(kDisplayPrelude + R"(
        func Forward<U>(value: U) -> int { return Render<U>(value); }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message,
             "type argument 'U' does not satisfy interface bound 'Display' on type parameter 'T'");
    CHECK_EQ(diagnostics[0].notes[0], "type parameter 'U' is not constrained by 'Display'");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "add the bound to the enclosing declaration, as in 'U: Display'");
}

TEST_CASE("a type parameter that repeats the callee's bound satisfies it") {
    const auto diagnostics = AnalyzeSource(kDisplayPrelude + R"(
        func Forward<U: Display>(value: U) -> int { return Render<U>(value); }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("overload selection skips a candidate whose bound the type argument does not satisfy") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Plain { value: int; }
        func Render<T: Display>(value: T, tag: int) -> int { return 0; }
        func Render<T>(value: T, tag: bool) -> int { return 1; }
        func Main() {
            let plain = Plain { value: 1 };
            Render<Plain>(plain, true);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("a generic struct checks its bounds at a type reference and at an initializer") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Plain { value: int; }
        struct Cell<T: Display> { item: T; }
        func Reference(cell: *Cell<Plain>) {}
        func Main() {
            let cell = Cell<Plain> { item: Plain { value: 1 } };
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message,
             "type argument 'Plain' does not satisfy interface bound 'Display' on type parameter 'T'");
    CHECK_EQ(diagnostics[0].notes[1], "type parameter 'T' of struct 'Cell' is bound by 'Display'");
    CHECK_EQ(diagnostics[1].message,
             "type argument 'Plain' does not satisfy interface bound 'Display' on type parameter 'T'");
}

TEST_CASE("an associated function call checks the bounds of the type it is called on") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Plain { value: int; }
        struct Cell<T: Display> { item: T; }
        extend Cell<T> {
            func Count() -> int { return 0; }
        }
        func Main() { Cell::Count<Plain>(); }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message,
             "type argument 'Plain' does not satisfy interface bound 'Display' on type parameter 'T'");
    CHECK_EQ(diagnostics[0].notes[1], "type parameter 'T' of struct 'Cell' is bound by 'Display'");
}

TEST_CASE("an extend block borrows the bounds of the type it extends") {
    const auto satisfied = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Cell<T: Display> { item: T; }
        func Render<T: Display>(count: int) -> int { return count; }
        extend Cell<T> {
            func Count(self: *Cell<T>) -> int { return Render<T>(1); }
        }
    )");

    CHECK(satisfied.empty());

    const auto unsatisfied = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Cell<T> { item: T; }
        func Render<T: Display>(count: int) -> int { return count; }
        extend Cell<T> {
            func Count(self: *Cell<T>) -> int { return Render<T>(1); }
        }
    )");

    REQUIRE_EQ(unsatisfied.size(), 1);
    CHECK_EQ(unsatisfied[0].notes[0], "type parameter 'T' is not constrained by 'Display'");
}

TEST_CASE("a generic enum variant constructor checks the bound of its written type argument") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Plain { value: int; }
        enum Slot<T: Display>: int32 { Empty, Filled(T) }
        func Main() {
            let plain = Plain { value: 1 };
            let slot = Slot::Filled<Plain>(plain);
        }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    CHECK_EQ(diagnostics[0].message,
             "type argument 'Plain' does not satisfy interface bound 'Display' on type parameter 'T'");
    CHECK_EQ(diagnostics[0].notes[1], "type parameter 'T' of enum 'Slot' is bound by 'Display'");
}

TEST_CASE("a bounded parameter satisfies its own bound where the generic refers back to itself") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Cell<T: Display> {
            item: T;
            next: *Cell<T>;
        }
        func Peek<T: Display>(cell: Cell<T>) -> int { return 0; }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("a generic enum checks the bound of its type argument once per written type") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Display {
            func Show() -> int;
        }
        struct Plain { value: int; }
        enum Slot<T: Display>: int32 { Empty, Filled(T) }
        func Reference(slot: Slot<Plain>) {}
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message,
             "type argument 'Plain' does not satisfy interface bound 'Display' on type parameter 'T'");
    CHECK_EQ(diagnostics[0].notes[1], "type parameter 'T' of enum 'Slot' is bound by 'Display'");
}
