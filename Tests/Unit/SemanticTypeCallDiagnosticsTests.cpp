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
    Lexer lexer(source, "types-and-calls.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "types-and-calls.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}
} // namespace

TEST_CASE("call diagnostics identify the argument and named parameter that disagree") {
    const auto diagnostics = AnalyzeSource(R"(
        func Add(left: int, right: int) {}
        func Main() { Add(1, "two"); }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message,
             "argument 2 to 'Add' has type 'Slice<char8>', but parameter 'right' requires 'int'");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK(diagnostics[0].notes[0].contains("parameter 'right' declared at 'types-and-calls.rux':2:"));
}

TEST_CASE("call arity diagnostics distinguish required defaulted and variadic parameters") {
    const auto diagnostics = AnalyzeSource(R"(
        func Configure(required: int, optional: int = 0) {}
        func Collect(head: int, values: int...) {}
        func Main() {
            Configure();
            Configure(1, 2, 3);
            Collect();
            Collect(1, true);
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 4);
    CHECK_EQ(diagnostics[0].message, "call to 'Configure' expects between 1 and 2 arguments, but 0 were provided");
    CHECK_EQ(diagnostics[1].message, "call to 'Configure' expects between 1 and 2 arguments, but 3 were provided");
    CHECK_EQ(diagnostics[2].message, "call to 'Collect' expects at least 1 argument, but 0 were provided");
    CHECK_EQ(diagnostics[3].message,
             "argument 2 to 'Collect' has type 'bool8', but variadic parameter 'values' requires 'int'");
}

TEST_CASE("overload failures list candidate signatures with declaration context") {
    const auto diagnostics = AnalyzeSource(R"(
        func Choose(value: int) {}
        func Choose(value: bool) {}
        func Main() { Choose("text"); }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "no matching overload for 'Choose' with argument types (Slice<char8>)");
    REQUIRE_EQ(diagnostics[0].notes.size(), 2);
    CHECK(diagnostics[0].notes[0].contains("candidate 'Choose(value: int)' declared at 'types-and-calls.rux':2:"));
    CHECK(diagnostics[0].notes[1].contains("candidate 'Choose(value: bool)' declared at 'types-and-calls.rux':3:"));
}

TEST_CASE("method diagnostics retain receiver and parameter context") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Counter {}
        extend Counter {
            func Add(self: *Counter, amount: int) {}
        }
        func Main() {
            let counter = Counter {};
            counter.Add(false);
            counter.Add();
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "argument 1 to 'Add' has type 'bool8', but parameter 'amount' requires 'int'");
    CHECK_EQ(diagnostics[1].message, "call to 'Add' expects 1 argument, but 0 were provided");
}

TEST_CASE("generic type and call diagnostics state expected and received type-argument counts") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Box<T> { value: T; }
        func Identity<T>(value: T) -> T { return value; }
        func Invalid(value: Box) {}
        func Main() { Identity<int, bool>(1); }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "struct type 'Box' requires 1 type argument, but 0 were provided");
    CHECK_EQ(diagnostics[1].message, "function 'Identity' requires 1 type argument, but 2 were provided");
}

TEST_CASE("struct initializer diagnostics identify duplicate unknown missing and mistyped fields") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Point { x: int; y: bool; }
        func Main() {
            let point = Point { x: "left", x: 2, z: 3 };
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 4);
    CHECK_EQ(diagnostics[0].message,
             "field 'x' in initializer for 'Point' has type 'Slice<char8>', but its declaration requires 'int'");
    CHECK_EQ(diagnostics[1].message, "field 'x' is initialized more than once for 'Point'");
    CHECK_EQ(diagnostics[2].message, "struct 'Point' has no field 'z'");
    REQUIRE_EQ(diagnostics[2].notes.size(), 1);
    CHECK_EQ(diagnostics[2].notes[0], "available fields are 'x' and 'y'");
    CHECK_EQ(diagnostics[3].message, "initializer for 'Point' is missing required field 'y'");
}

TEST_CASE("enum constructors diagnose positional and named payload expectations") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Choice {
            Empty,
            Pair(int, bool),
            Named { value: int; }
        }
        func Main() {
            Choice::Empty(1);
            Choice::Pair(1, "wrong");
            let named = Choice::Named { value: false };
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message, "call to 'Choice::Empty' expects 0 arguments, but 1 was provided");
    CHECK_EQ(diagnostics[1].message,
             "argument 2 to enum variant 'Choice::Pair' has type 'Slice<char8>', but field 2 requires 'bool8'");
    CHECK_EQ(diagnostics[2].message,
             "field 'value' in initializer for 'Choice::Named' has type 'bool8', but its declaration requires 'int'");
}

TEST_CASE("union initializers select one known field with its declared type") {
    CHECK(AnalyzeSource(R"(
        union Bits { integer: int32, decimal: float32 }
        func Main() {
            let bits = Bits { integer: 1i32 };
            let value = bits.integer;
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        union Bits { integer: int32, decimal: float32 }
        func Main() {
            let empty = Bits {};
            let many = Bits { integer: 1i32, decimal: 1.0f32 };
            let unknown = Bits { raw: 1 };
            let wrong = Bits { integer: false };
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 4);
    CHECK_EQ(diagnostics[0].message, "union initializer for 'Bits' must select exactly one field, but 0 were provided");
    CHECK_EQ(diagnostics[1].message, "union initializer for 'Bits' must select exactly one field, but 2 were provided");
    CHECK_EQ(diagnostics[2].message, "union 'Bits' has no field 'raw'");
    CHECK_EQ(diagnostics[3].message,
             "field 'integer' in initializer for union 'Bits' has type 'bool8', but its declaration requires 'int32'");
}

TEST_CASE("a generic struct's field keeps the type parameter it was declared with") {
    // A generic struct's arguments are recovered from the receiver's printed name, where a parameter and a type called
    // the same thing are one spelling. Reading it as a type made a field declared `T` a different type from the
    // enclosing function's `T`, reported against itself as `cannot assign 'T' to 'T'`. The parameter names deliberately
    // differ between the struct and each function, so matching them by spelling would not answer.
    CHECK(AnalyzeSource(R"(
        struct Box<E> {
            value: E;
        }

        struct View<E> {
            data: *var E;
        }

        func Unwrap<T>(box: Box<T>) -> T {
            return box.value;
        }

        func Store<T>(view: View<T>, value: T) {
            view.data[0] = value;
        }

        func Rebox<T>(box: Box<T>) -> Box<T> {
            return Box<T> { value: box.value };
        }
    )")
              .empty());

    // A type parameter shadows a type of the same name inside the generic, and only inside it.
    CHECK(AnalyzeSource(R"(
        struct T {
            tag: int32;
        }

        struct Box<E> {
            value: E;
        }

        func Unwrap<T>(box: Box<T>) -> T {
            return box.value;
        }

        func TagOf(value: T) -> int32 {
            return value.tag;
        }

        func Main() {
            let tag = TagOf(T { tag: 1 });
        }
    )")
              .empty());
}

TEST_CASE("array tuple slice and index diagnostics state the aggregate requirement") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let array = [1, "two"];
            array[false];
            let tuple = (1, true);
            tuple.name;
            tuple.2;
            let scalar = 1;
            scalar[0];
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 5);
    CHECK_EQ(diagnostics[0].message,
             "array element 2 has type 'Slice<char8>', but element 1 established element type 'int'");
    CHECK_EQ(diagnostics[1].message, "index for type 'int[2]' must be an integer or range, but has type 'bool8'");
    CHECK_EQ(diagnostics[2].message, "tuple type '(int, bool8)' has no member 'name'");
    REQUIRE(diagnostics[2].help.has_value());
    CHECK_EQ(*diagnostics[2].help, "tuple members use zero-based numeric indices such as '.0'");
    CHECK_EQ(diagnostics[3].message, "tuple index 2 is out of range for a tuple with 2 elements");
    CHECK_EQ(diagnostics[4].message, "type 'int' cannot be indexed");
}
