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
    Lexer lexer(source, "operators-and-assignments.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "operators-and-assignments.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}
} // namespace

TEST_CASE("unary diagnostics name the operator and required operand family") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            var text = "text";
            let logical = !1;
            let negative = -text;
            let inverted = ~1.0;
            *1;
            text++;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 5);
    CHECK_EQ(diagnostics[0].message, "operator '!' requires a bool operand, but found 'int'");
    CHECK_EQ(diagnostics[1].message, "operator '-' requires a numeric operand, but found 'Slice<char8>'");
    CHECK_EQ(diagnostics[2].message, "operator '~' requires an integer or bool operand, but found 'float64'");
    CHECK_EQ(diagnostics[3].message, "operator '*' requires a pointer operand, but found 'int'");
    CHECK_EQ(diagnostics[4].message, "operator '++' requires a numeric operand, but found 'Slice<char8>'");
}

TEST_CASE("binary diagnostics identify left and right operands across every operator family") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let arithmetic = 1 * "two";
            let mixedNumeric = 1u8 / 2.0f32;
            let bitwiseLeft = 1.0 & 1;
            let bitwiseRight = 1 | 1.0;
            let shiftLeft = true << 1;
            let shiftRight = 1 >> false;
            let logicalLeft = 1 && true;
            let logicalRight = false || 1;
            let compared = 1 < "two";
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 9);
    CHECK_EQ(diagnostics[0].message,
             "operator '*' cannot combine left operand 'int' with right operand 'Slice<char8>'");
    CHECK_EQ(diagnostics[1].message, "operator '/' cannot combine left operand 'uint8' with right operand 'float32'");
    CHECK_EQ(diagnostics[2].message,
             "operator '&' requires an integer, bool, or character left operand, but found 'float64'");
    CHECK_EQ(diagnostics[3].message,
             "operator '|' requires an integer, bool, or character right operand, but found 'float64'");
    CHECK_EQ(diagnostics[4].message, "operator '<<' requires an integer or character left operand, but found 'bool8'");
    CHECK_EQ(diagnostics[5].message, "operator '>>' requires an integer right operand, but found 'bool8'");
    CHECK_EQ(diagnostics[6].message, "operator '&&' requires a bool left operand, but found 'int'");
    CHECK_EQ(diagnostics[7].message, "operator '||' requires a bool right operand, but found 'int'");
    CHECK_EQ(diagnostics[8].message,
             "operator '<' cannot compare left operand 'int' with right operand 'Slice<char8>'");
}

TEST_CASE("signed unsigned and logical shifts report the precise rejected operand") {
    CHECK(AnalyzeSource(R"(
        func Main() {
            let signed: int8 = -8;
            let unsigned: uint8 = 248u8;
            let arithmetic = signed >> 2;
            let logical = signed >>> 2;
            let unsignedShift = unsigned >> 2;
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let unsigned: uint8 = 248u8;
            let invalidLeft = unsigned >>> 2;
            let signed: int8 = -8;
            let invalidRight = signed >>> true;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "operator '>>>' requires a signed integer left operand, but found 'uint8'");
    CHECK_EQ(diagnostics[1].message, "operator '>>>' requires an integer right operand, but found 'bool8'");
}

TEST_CASE("bool integer equality remains valid while unrelated comparisons name both sides") {
    CHECK(AnalyzeSource(R"(
        func Main() {
            let first = true == 1;
            let second = 0 != false;
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        struct Item {}
        func Main() { let invalid = Item {} == 1; }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "operator '==' cannot compare left operand 'Item' with right operand 'int'");
}

TEST_CASE("variant equality accepts structurally comparable payloads") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Label { value: int; }
        extend Label {
            func ==(self: &Label, other: Label) -> bool { return self.value == other.value; }
        }
        variant Inner {
            None,
            Number(int)
        }
        variant Value<T> {
            Unit,
            Pair(T, T),
            Named { inner: Inner; label: Label; },
            Composite((int, bool), int[2]),
            Link(*Value<T>)
        }
        func Equal(left: Value<int>, right: Value<int>) -> bool { return left == right; }
        func Different(left: Value<int>, right: Value<int>) -> bool { return left != right; }
    )");

    for (const auto &diagnostic : diagnostics) {
        INFO(diagnostic.message);
    }
    CHECK(diagnostics.empty());
}

TEST_CASE("variant equality diagnoses the first non-comparable active payload type") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Opaque { value: int; }
        variant Invalid {
            Empty,
            Stored(Opaque),
            Repeated(Opaque, Opaque)
        }
        func Equal(left: Invalid, right: Invalid) -> bool {
            return left == right;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message,
             "variant equality for 'Invalid' is unavailable because payload type 'Opaque' in case "
             "'Invalid::Stored' has no '==' operator");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "declare '==' on 'Opaque' or remove equality on the containing variant");
}

TEST_CASE("generic variant equality requirements are checked per concrete instantiation") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Comparable { value: int; }
        extend Comparable {
            func ==(self: &Comparable, other: Comparable) -> bool { return self.value == other.value; }
        }
        struct Opaque { value: int; }
        variant Maybe<T> { None, Some(T) }

        func Same<T>(left: Maybe<T>, right: Maybe<T>) -> bool {
            return left == right;
        }
        func Valid(left: Maybe<Comparable>, right: Maybe<Comparable>) -> bool {
            return Same<Comparable>(left, right);
        }
        func Invalid(left: Maybe<Opaque>, right: Maybe<Opaque>) -> bool {
            return Same<Opaque>(left, right);
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message,
             "variant equality for 'Maybe<Opaque>' is unavailable because payload type 'Opaque' in case "
             "'Maybe::Some' has no '==' operator");
}

TEST_CASE("equality between distinct variant types remains a type mismatch") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Left { Value(int) }
        variant Right { Value(int) }
        func Compare(left: Left, right: Right) -> bool { return left == right; }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "operator '==' cannot compare left operand 'Left' with right operand 'Right'");
}

TEST_CASE("scalar enum representation casts remain available") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Status: uint8 { Ready = 1, Busy = 2 }
        func Encode(value: Status) -> uint8 { return value as uint8; }
        func Decode(value: uint8) -> Status { return value as Status; }
        func Same(value: Status) -> Status { return value as Status; }
        func Compare(left: Status, right: Status) -> bool {
            return left == right || left < right;
        }
    )");

    for (const auto &diagnostic : diagnostics) {
        INFO(diagnostic.message);
    }
    CHECK(diagnostics.empty());
}

TEST_CASE("unrelated scalar enums cannot be representation-cast directly") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Left: uint8 { Value = 1 }
        enum Right: uint8 { Value = 1 }
        func Convert(value: Left) -> Right { return value as Right; }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "cannot cast enum 'Left' directly to unrelated enum 'Right'");
}

TEST_CASE("variant representation casts are rejected in every direction") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Value { Empty, Number(int) }
        variant Other { Empty, Number(int) }
        func ToScalar(value: Value) -> int { return value as int; }
        func FromScalar(value: int) -> Value { return value as Value; }
        func ToVariant(value: Value) -> Other { return value as Other; }
        func SameVariant(value: Value) -> Value { return value as Value; }
    )");

    REQUIRE_EQ(diagnostics.size(), 4);
    CHECK_EQ(diagnostics[0].message, "cannot cast variant 'Value' to scalar type 'int'");
    CHECK_EQ(diagnostics[1].message, "cannot cast scalar type 'int' to variant 'Value'");
    CHECK_EQ(diagnostics[2].message, "cannot cast variant 'Value' to variant 'Other'");
    CHECK_EQ(diagnostics[3].message, "cannot cast variant 'Value' to variant 'Value'");
}

TEST_CASE("variants have structural equality but no built-in ordering") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Value { Empty, Number(int) }
        func Compare(left: Value, right: Value) -> bool {
            let equal = left == right;
            let notEqual = left != right;
            let less = left < right;
            let lessEqual = left <= right;
            let greater = left > right;
            let greaterEqual = left >= right;
            return equal || notEqual || less || lessEqual || greater || greaterEqual;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 4);
    CHECK_EQ(diagnostics[0].message, "operator '<' is not defined for variant 'Value'");
    CHECK_EQ(diagnostics[1].message, "operator '<=' is not defined for variant 'Value'");
    CHECK_EQ(diagnostics[2].message, "operator '>' is not defined for variant 'Value'");
    CHECK_EQ(diagnostics[3].message, "operator '>=' is not defined for variant 'Value'");
    for (const auto &diagnostic : diagnostics) {
        REQUIRE_EQ(diagnostic.notes.size(), 1);
        CHECK_EQ(diagnostic.notes[0], "variants have structural equality but no built-in ordering");
        REQUIRE(diagnostic.help.has_value());
        CHECK(diagnostic.help->starts_with("declare '"));
        CHECK(diagnostic.help->ends_with("to define this ordering"));
    }
}

TEST_CASE("a declared variant ordering operator overrides the built-in restriction") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Rank { Low, High }
        extend Rank {
            func <(self: &Rank, other: Rank) -> bool { return false; }
        }
        func Ordered(left: Rank, right: Rank) -> bool { return left < right; }
    )");

    for (const auto &diagnostic : diagnostics) {
        INFO(diagnostic.message);
    }
    CHECK(diagnostics.empty());
}

TEST_CASE("variants do not acquire numeric or bitwise operations from their tag") {
    const auto diagnostics = AnalyzeSource(R"(
        variant Value { Empty, Number(int) }
        func Invalid(value: Value) {
            let add = value + 1;
            let subtract = value - 1;
            let multiply = value * 2;
            let bits = value & 1;
            let shifted = value << 1;
            let negated = -value;
            var mutable = Value::Empty;
            let incremented = mutable++;
        }
    )");

    for (const auto &diagnostic : diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_EQ(diagnostics.size(), 7);
    CHECK_EQ(diagnostics[0].message, "operator '+' cannot combine left operand 'Value' with right operand 'int'");
    CHECK_EQ(diagnostics[1].message, "operator '-' cannot combine left operand 'Value' with right operand 'int'");
    CHECK_EQ(diagnostics[2].message, "operator '*' cannot combine left operand 'Value' with right operand 'int'");
    CHECK_EQ(diagnostics[3].message,
             "operator '&' requires an integer, bool, or character left operand, but found 'Value'");
    CHECK_EQ(diagnostics[4].message, "operator '<<' requires an integer or character left operand, but found 'Value'");
    CHECK_EQ(diagnostics[5].message, "operator '-' requires a numeric operand, but found 'Value'");
    CHECK_EQ(diagnostics[6].message, "operator '++' requires a numeric operand, but found 'Value'");
}

TEST_CASE("compound assignments apply the matching operator requirements") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            var count = 1;
            count += "one";
            count <<= false;
            var bits: uint8 = 128u8;
            bits >>>= 1;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message,
             "operator '+=' cannot combine left operand 'int' with right operand 'Slice<char8>'");
    CHECK_EQ(diagnostics[1].message, "operator '<<=' requires an integer right operand, but found 'bool8'");
    CHECK_EQ(diagnostics[2].message, "operator '>>>=' requires a signed integer left operand, but found 'uint8'");
}

TEST_CASE("assignment diagnostics distinguish constants bindings and read-only data") {
    const auto diagnostics = AnalyzeSource(R"(
        const Limit: int = 10;
        struct Box { value: int; }
        func Main() {
            Limit = 11;
            let fixed = 1;
            fixed = 2;
            let pointer: *int = @fixed;
            *pointer = 3;
            let box = Box { value: 1 };
            box.value = 2;
            let values: int[1] = [1];
            values[0] = 2;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 5);
    CHECK_EQ(diagnostics[0].message, "cannot modify constant 'Limit'");
    CHECK_FALSE(diagnostics[0].help.has_value());
    CHECK_EQ(diagnostics[1].message, "cannot modify immutable variable 'fixed'");
    CHECK_EQ(diagnostics[1].help, "declare 'fixed' with 'var' to make it mutable");
    CHECK_EQ(diagnostics[2].message, "cannot modify data through read-only pointer '*int'");
    CHECK_FALSE(diagnostics[2].help.has_value());
    CHECK_EQ(diagnostics[3].message, "cannot modify immutable variable 'box'");
    CHECK_EQ(diagnostics[3].help, "declare 'box' with 'var' to make it mutable");
    CHECK_EQ(diagnostics[4].message, "cannot modify immutable variable 'values'");
    CHECK_EQ(diagnostics[4].help, "declare 'values' with 'var' to make it mutable");
}

TEST_CASE("mutable dereferences fields and indexes remain assignable") {
    CHECK(AnalyzeSource(R"(
        struct Box { value: int; }
        func Main() {
            var number = 1;
            let pointer: *var int = @number;
            *pointer = 2;
            var box = Box { value: 1 };
            box.value = 2;
            var values: int[1] = [1];
            values[0] = 2;
        }
    )")
              .empty());
}

TEST_CASE("assignments and increments reject value expressions as targets") {
    const auto diagnostics = AnalyzeSource(R"(
        func Value() -> int { return 1; }
        func Main() {
            1 = 2;
            (1 + 2) += 3;
            Value()--;
            ++1;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 4);
    CHECK_EQ(diagnostics[0].message, "operator '=' requires an assignable target, but its left operand has type 'int'");
    CHECK_EQ(diagnostics[1].message,
             "operator '+=' requires an assignable target, but its left operand has type 'int'");
    CHECK_EQ(diagnostics[2].message,
             "operator '--' requires an assignable target, but its left operand has type 'int'");
    CHECK_EQ(diagnostics[3].message,
             "operator '++' requires an assignable target, but its left operand has type 'int'");
}

TEST_CASE("cast and type-test diagnostics name source and target types") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Item {}
        interface Marker {}
        func Main() {
            let invalidSource = Item {} as int;
            let invalidTarget = 1 as Item;
            let below = -1 as char8;
            let above = 0x110000u32 as char32;
            let surrogate = 0xD800u32 as char32;
            let runtime = 1 is Marker;
            let staticCheck = 1 is int;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 6);
    CHECK_EQ(diagnostics[0].message, "cannot cast value of type 'Item' to 'int'");
    CHECK_EQ(diagnostics[1].message, "cannot cast value of type 'int' to 'Item'");
    CHECK_EQ(diagnostics[2].message, "constant cast from 'int' to 'char8' is outside the target type's range");
    CHECK_EQ(diagnostics[3].message, "constant cast from 'uint32' to 'char32' is outside the target type's range");
    CHECK_EQ(diagnostics[4].message, "cast from 'uint32' to 'char32' uses invalid surrogate code point U+D800");
    CHECK_EQ(diagnostics[5].message, "type test 'is Marker' is unavailable: interface checks are not implemented");
}

TEST_CASE("function values cast to and from pointers but not to scalars") {
    const auto diagnostics = AnalyzeSource(R"(
        func Load(address: *opaque) {
            let callable = address as func() -> int;
            let back = callable as *opaque;
            let invalidTarget = 1 as func() -> int;
            let invalidSource = callable as int;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "cannot cast value of type 'int' to 'func() -> int'");
    CHECK_EQ(diagnostics[1].message, "cannot cast value of type 'func() -> int' to 'int'");
}

TEST_CASE("a declared indexer answers indexing and selects its overload by index type") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Coord { row: uint; column: uint; }
        struct Vect { data: int[4]; }
        extend Vect {
            func [](self: &Vect, index: uint) -> int { return self.data[index]; }
            func [](self: &Vect, at: Coord) -> int { return self.data[at.row]; }
        }
        struct Box<T> { items: T[2]; }
        extend Box<T> {
            func [](self: &Box<T>, index: uint) -> T { return self.items[index]; }
        }
        func Main() {
            let vect = Vect { data: [1, 2, 3, 4] };
            let byOffset: int = vect[1];
            let at = Coord { row: 0, column: 0 };
            let byCoord: int = vect[at];
            let boxed = Box<int32> { items: [1i32, 2i32] };
            let element: int32 = boxed[0];
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("an index on a type that declares no indexer names the operator that would define it") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Plain { value: int; }
        func Main() {
            let plain = Plain { value: 1 };
            let rejected: int = plain[0];
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "type 'Plain' cannot be indexed");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "declare 'func []' on 'Plain'");
}

TEST_CASE("an indexer result is a value, so no assignment or borrow reaches through it") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Cell { value: int; }
        struct Holder { inner: Cell; }
        extend Holder {
            func [](self: &Holder, index: uint) -> Cell { return self.inner; }
        }
        func Borrow(cell: &Cell) {}
        func Main() {
            var holder = Holder { inner: Cell { value: 1 } };
            holder[0] = Cell { value: 2 };
            holder[0].value = 3;
            Borrow(holder[0]);
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message, "cannot assign through the '[]' operator on 'Holder'");
    CHECK_EQ(diagnostics[1].message, "cannot assign through the '[]' operator on 'Holder'");
    CHECK_EQ(diagnostics[2].message, "argument 1 to 'Borrow' has type 'Cell', but parameter 'cell' requires '&Cell'");
}

TEST_CASE("an indexer call reads its whole receiver rather than projecting into it") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Vect { data: int[4]; }
        extend Vect {
            func [](self: &Vect, index: uint) -> int { return self.data[index]; }
            func Bump(self: &var Vect) { self.data[0] = self.data[0] + 1; }
        }
        func Main() {
            var vect = Vect { data: [1, 2, 3, 4] };
            var exclusive: &var Vect = vect;
            let read: int = vect[0];
            exclusive.Bump();
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "cannot read 'vect' while 'exclusive' holds an exclusive borrow");
}

TEST_CASE("an indexer result transfers as a temporary and leaves its receiver whole") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Owned { value: int; }
        extend Owned {
            func =(self: &var Owned, other: &Owned);
        }
        struct Bag { slot: Owned; }
        extend Bag {
            func [](self: &Bag, index: uint) -> Owned { return Owned { value: 1 }; }
        }
        func Consume(item: Owned) {}
        func Main() {
            let bag = Bag { slot: Owned { value: 5 } };
            Consume(bag[0]);
            Consume(bag[1]);
            let still: int = bag.slot.value;
        }
    )");

    CHECK(diagnostics.empty());
}
