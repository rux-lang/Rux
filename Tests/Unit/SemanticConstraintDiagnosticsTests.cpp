#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
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
    Lexer lexer(source, "constraints.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "constraints.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

/// Lower an accepted source, so what a constrained call becomes can be asserted rather than only that it was accepted.
HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "constraints.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "constraints.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    return AstToHirLowering(model).Generate();
}

[[nodiscard]] bool HasFunction(const HirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    return std::ranges::any_of(package.modules.front().funcs,
                               [&](const HirFunc &function) { return function.name == name; });
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    for (const HirFunc &function : package.modules.front().funcs) {
        if (function.name == name) {
            return function;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// The callee of the call a one-expression body returns.
std::string ReturnedCalleeName(const HirFunc &function) {
    REQUIRE(function.body.has_value());
    REQUIRE_EQ(function.body->stmts.size(), 1);
    const auto *returned = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *call = dynamic_cast<const HirCallExpr *>(returned->value->get());
    REQUIRE(call != nullptr);
    const auto *callee = dynamic_cast<const HirVarExpr *>(call->callee.get());
    REQUIRE(callee != nullptr);
    return callee->name;
}

/// The named function from whichever module holds it, for the cases whose package has more than one.
const HirFunc &RequireFunctionAnywhere(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// Lower a module that calls into a dependency package, so a bound written in one package and instantiated from
/// another is exercised the way a workspace build exercises it.
HirPackage LowerAgainstDependency(const std::string &userSource, const std::string &dependencyName,
                                  const std::string &dependencySource) {
    Lexer dependencyLexer(dependencySource, dependencyName + ".rux");
    auto dependencyTokens = dependencyLexer.Tokenize();
    REQUIRE_FALSE(dependencyTokens.HasErrors());
    Parser dependencyParser(std::move(dependencyTokens.tokens), dependencyName + ".rux");
    ParseResult parsedDependency = dependencyParser.Parse();
    REQUIRE_FALSE(parsedDependency.HasErrors());

    DepPackage dependency;
    dependency.name = dependencyName;
    dependency.modules.push_back({dependencyName, &parsedDependency.module});

    Lexer lexer(userSource, "app.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "app.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {std::move(dependency)}, "App", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    return AstToHirLowering(model).Generate();
}

/// One bound, a type that satisfies it nominally, a type that satisfies it by declaring the method, and a generic that
/// calls the operation through its parameter.
const std::string kMeasuredPrelude = R"(
    interface Measured {
        func Measure() -> int;
    }
    struct Square { size: int; }
    extend Square : Measured {
        func Measure(self: &Square) -> int { return self.size * self.size; }
    }
    struct Segment { length: int; }
    extend Segment {
        func Measure(self: &Segment) -> int { return self.length; }
    }
    func Total<T: Measured>(value: T) -> int { return value.Measure(); }
)";

/// A displayable type, its conforming implementation, and a generic that needs the bound. Every case below varies one
/// part of this, so the shared prefix keeps each source down to what it is actually testing.
const std::string kDisplayPrelude = R"(
    interface Display {
        func Show() -> int;
    }
    struct Labeled { value: int; }
    extend Labeled : Display {
        func Show(self: &Labeled) -> int { return self.value; }
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
            func Show(self: &Structural) -> int { return self.value; }
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
            func Show(self: &Half) -> int { return self.value; }
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
            func Count(self: &Cell<T>) -> int { return Render<T>(1); }
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
            func Count(self: &Cell<T>) -> int { return Render<T>(1); }
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

TEST_CASE("a bound operation is callable through the type parameter it is declared on") {
    const auto diagnostics = AnalyzeSource(kMeasuredPrelude + R"(
        func Main() -> int {
            let square = Square { size: 5 };
            return Total<Square>(square);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("a call through a parameter that does not provide the operation names the bounds in scope") {
    const auto unbounded = AnalyzeSource(R"(
        func Loose<T>(value: T) -> int { return value.Measure(); }
    )");

    REQUIRE_EQ(unbounded.size(), 1);
    CHECK_EQ(unbounded[0].message, "no interface bound on type parameter 'T' provides method 'Measure'");
    REQUIRE_EQ(unbounded[0].notes.size(), 1);
    CHECK_EQ(unbounded[0].notes[0], "type parameter 'T' has no interface bounds");
    REQUIRE(unbounded[0].help.has_value());
    CHECK_EQ(*unbounded[0].help, "add a bound whose interface declares 'Measure', as in 'T: SomeInterface'");

    const auto wrongOperation = AnalyzeSource(kMeasuredPrelude + R"(
        func Tight<T: Measured>(value: T) -> int { return value.Scale(2); }
    )");

    REQUIRE_EQ(wrongOperation.size(), 1);
    CHECK_EQ(wrongOperation[0].message, "no interface bound on type parameter 'T' provides method 'Scale'");
    CHECK_EQ(wrongOperation[0].notes[0], "type parameter 'T' is bound by 'Measured'");
}

TEST_CASE("a constrained call is checked against the signature its interface declares") {
    const auto diagnostics = AnalyzeSource(R"(
        interface Scaled {
            func Times(factor: int) -> int;
        }
        func Apply<T: Scaled>(value: T) -> int { return value.Times(); }
        func Mistyped<T: Scaled>(value: T) -> int { return value.Times(true); }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "call to 'Times' expects 1 argument, but 0 were provided");
    CHECK_EQ(diagnostics[1].message, "argument 1 to 'Times' has type 'bool8', but parameter 'factor' requires 'int'");
}

TEST_CASE("each instantiation of a constrained generic calls the method of its own type argument") {
    const HirPackage package = LowerSource(kMeasuredPrelude + R"(
        func Main() -> int {
            let square = Square { size: 5 };
            let segment = Segment { length: 9 };
            return Total<Square>(square) + Total<Segment>(segment);
        }
    )");

    CHECK_EQ(ReturnedCalleeName(RequireFunction(package, "Total_Square")), "Square::Measure");
    CHECK_EQ(ReturnedCalleeName(RequireFunction(package, "Total_Segment")), "Segment::Measure");
}

TEST_CASE("a constrained generic is emitted only as its instantiations") {
    const HirPackage package = LowerSource(kMeasuredPrelude + R"(
        func Main() -> int {
            let square = Square { size: 5 };
            return Total<Square>(square);
        }
    )");

    CHECK(HasFunction(package, "Total_Square"));
    // The symbolic form has no method to call for its bound, so lowering never emits it.
    CHECK_FALSE(HasFunction(package, "Total"));
}

TEST_CASE("a bound forwarded to a second constrained generic reaches the same concrete method") {
    const HirPackage package = LowerSource(kMeasuredPrelude + R"(
        func Doubled<U: Measured>(value: U) -> int { return Total<U>(value) * 2; }
        func Main() -> int {
            let square = Square { size: 5 };
            return Doubled<Square>(square);
        }
    )");

    CHECK(HasFunction(package, "Doubled_Square"));
    CHECK_EQ(ReturnedCalleeName(RequireFunction(package, "Total_Square")), "Square::Measure");
    CHECK_FALSE(HasFunction(package, "Total_U"));
}

TEST_CASE("a method of a generic extend block calls the bound of the type it extends") {
    const HirPackage package = LowerSource(kMeasuredPrelude + R"(
        struct Cell<T: Measured> { item: T; }
        extend Cell<T> {
            func Inside(self: &Cell<T>) -> int { return self.item.Measure(); }
        }
        func Main() -> int {
            let cell = Cell<Square> { item: Square { size: 4 } };
            return cell.Inside();
        }
    )");

    CHECK_EQ(ReturnedCalleeName(RequireFunction(package, "Cell::Inside_Square")), "Square::Measure");
}

TEST_CASE("a bound is resolved where it was written, not where the generic is called") {
    // The caller imports the generic and the type it instantiates it with, and never names the interface. A bound is
    // the declaration's promise, so requiring the caller to import an interface it does not mention would be a demand
    // no reader would predict -- and resolving the bound against the caller's scope instead left it resolving to
    // nothing, which lowering then had no conformance to call.
    const HirPackage package = LowerAgainstDependency(R"(
import Shapes::{ Square, Total };

func Main() -> int {
    let square = Square { size: 5 };
    return Total<Square>(square);
}
)",
                                                      "Shapes", R"(
pub interface Measured {
    func Measure() -> int;
}

pub struct Square { size: int; }

extend Square : Measured {
    func Measure(self: &Square) -> int { return self.size * self.size; }
}

pub func Total<T: Measured>(value: T) -> int { return value.Measure(); }
)");

    CHECK_EQ(ReturnedCalleeName(RequireFunctionAnywhere(package, "Total_Square")), "Square::Measure");
}
