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
