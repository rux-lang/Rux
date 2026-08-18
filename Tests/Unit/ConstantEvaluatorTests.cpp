#include "Optimization/ConstantEvaluator.h"

#include <array>
#include <doctest.h>
#include <string_view>

using namespace Rux;
using namespace Rux::Optimization;

namespace {
TypedConstant Constant(const std::string_view literal, const TypeRef &type) {
    const auto value = ParseConstant(literal, type);
    REQUIRE(value.has_value());
    return *value;
}

TypedConstant Binary(const TokenKind op, const TypedConstant &left, const TypedConstant &right) {
    const auto value = EvaluateBinary(op, left, right);
    REQUIRE(value.has_value());
    return *value;
}

struct WidthCase {
    TypeRef type;
    std::string_view maximum;
    std::string_view minimum;
};
} // namespace

TEST_CASE("typed constants parse booleans and supported integer literal forms") {
    // A bool is one bit wide however wide it is stored, so every width models the same two values.
    const auto boolean = Constant("true", TypeRef::MakeBool16());
    CHECK(boolean.Width() == 1);
    CHECK(Constant("true", TypeRef::MakePrimitive(TypeRef::Kind::Bool64)).Width() == 1);
    CHECK(boolean.BooleanValue() == true);
    const auto no = Constant("false", TypeRef::MakeBool16());
    CHECK(Binary(TokenKind::AmpAmp, boolean, no).BooleanValue() == false);
    CHECK(Binary(TokenKind::PipePipe, boolean, no).BooleanValue() == true);
    CHECK(EvaluateUnary(TokenKind::Bang, boolean)->BooleanValue() == false);

    CHECK(Constant("0xff", TypeRef::MakeUInt8()).RawBits() == 255);
    CHECK(Constant("0b1010_0101", TypeRef::MakeUInt8()).RawBits() == 165);

    CHECK_FALSE(ParseConstant("1u8", TypeRef::MakeUInt8()).has_value());
    CHECK_FALSE(ParseConstant("1.0", TypeRef::MakeInt32()).has_value());
    CHECK_FALSE(ParseConstant("-1", TypeRef::MakeUInt32()).has_value());
    CHECK_FALSE(ParseConstant("256", TypeRef::MakeUInt8()).has_value());
}

TEST_CASE("integer arithmetic wraps to every declared signed width without host overflow") {
    const std::array cases{
        WidthCase{TypeRef::MakeInt8(), "127", "-128"},
        WidthCase{TypeRef::MakeInt16(), "32767", "-32768"},
        WidthCase{TypeRef::MakeInt32(), "2147483647", "-2147483648"},
        WidthCase{TypeRef::MakeInt64(), "9223372036854775807", "-9223372036854775808"},
        WidthCase{TypeRef::MakeInt(), "9223372036854775807", "-9223372036854775808"},
    };

    for (const auto &[type, maximum, minimum] : cases) {
        const auto max = Constant(maximum, type);
        const auto min = Constant(minimum, type);
        const auto one = Constant("1", type);
        CHECK(Binary(TokenKind::Plus, max, one).ToLiteral() == minimum);
        CHECK(Binary(TokenKind::Minus, min, one).ToLiteral() == maximum);
        CHECK(Binary(TokenKind::Star, min, Constant("2", type)).ToLiteral() == "0");
    }
}

TEST_CASE("integer arithmetic wraps to every declared unsigned width") {
    const std::array cases{
        WidthCase{TypeRef::MakeUInt8(), "255", "0"},
        WidthCase{TypeRef::MakeUInt16(), "65535", "0"},
        WidthCase{TypeRef::MakeUInt32(), "4294967295", "0"},
        WidthCase{TypeRef::MakeUInt64(), "18446744073709551615", "0"},
        WidthCase{TypeRef::MakeUInt(), "18446744073709551615", "0"},
    };

    for (const auto &[type, maximum, unused] : cases) {
        const auto max = Constant(maximum, type);
        const auto zero = Constant("0", type);
        const auto one = Constant("1", type);
        CHECK(Binary(TokenKind::Plus, max, one).ToLiteral() == "0");
        CHECK(Binary(TokenKind::Minus, zero, one).ToLiteral() == maximum);
    }
    CHECK(Binary(TokenKind::StarStar, Constant("3", TypeRef::MakeUInt8()), Constant("5", TypeRef::MakeUInt8()))
              .ToLiteral() == "243");
}

TEST_CASE("division traps remain unfurled for every signed width") {
    const std::array cases{
        WidthCase{TypeRef::MakeInt8(), "127", "-128"},
        WidthCase{TypeRef::MakeInt16(), "32767", "-32768"},
        WidthCase{TypeRef::MakeInt32(), "2147483647", "-2147483648"},
        WidthCase{TypeRef::MakeInt64(), "9223372036854775807", "-9223372036854775808"},
    };

    for (const auto &[type, unused, minimum] : cases) {
        const auto min = Constant(minimum, type);
        const auto minusOne = Constant("-1", type);
        const auto zero = Constant("0", type);
        CHECK_FALSE(EvaluateBinary(TokenKind::Slash, min, minusOne).has_value());
        CHECK_FALSE(EvaluateBinary(TokenKind::Percent, min, minusOne).has_value());
        CHECK_FALSE(EvaluateBinary(TokenKind::Slash, min, zero).has_value());
        CHECK_FALSE(EvaluateBinary(TokenKind::Percent, min, zero).has_value());
    }

    CHECK(
        Binary(TokenKind::Slash, Constant("-7", TypeRef::MakeInt8()), Constant("3", TypeRef::MakeInt8())).ToLiteral() ==
        "-2");
    CHECK(Binary(TokenKind::Percent, Constant("-7", TypeRef::MakeInt8()), Constant("3", TypeRef::MakeInt8()))
              .ToLiteral() == "-1");
}

TEST_CASE("bitwise operations and shifts retain declared-width runtime bits") {
    const auto bits = Constant("0b10100101", TypeRef::MakeUInt8());
    const auto mask = Constant("0b11110000", TypeRef::MakeUInt8());
    CHECK(Binary(TokenKind::Amp, bits, mask).ToLiteral() == "160");
    CHECK(Binary(TokenKind::Pipe, bits, mask).ToLiteral() == "245");
    CHECK(Binary(TokenKind::Caret, bits, mask).ToLiteral() == "85");
    REQUIRE(EvaluateUnary(TokenKind::Tilde, bits).has_value());
    CHECK(EvaluateUnary(TokenKind::Tilde, bits)->ToLiteral() == "90");

    const auto negative = Constant("-8", TypeRef::MakeInt8());
    const auto two = Constant("2", TypeRef::MakeUInt8());
    CHECK(Binary(TokenKind::GreaterGreater, negative, two).ToLiteral() == "-2");
    CHECK(Binary(TokenKind::GreaterGreaterGreater, negative, two).ToLiteral() == "62");
    CHECK(Binary(TokenKind::LessLess, Constant("1", TypeRef::MakeInt8()), Constant("7", TypeRef::MakeUInt8()))
              .ToLiteral() == "-128");

    CHECK_FALSE(EvaluateBinary(TokenKind::LessLess, bits, Constant("8", TypeRef::MakeUInt8())).has_value());
    CHECK_FALSE(EvaluateBinary(TokenKind::GreaterGreater, bits, Constant("-1", TypeRef::MakeInt8())).has_value());
    CHECK(Binary(TokenKind::Less, Constant("-1", TypeRef::MakeInt8()), Constant("1", TypeRef::MakeInt8()))
              .BooleanValue() == true);
    CHECK(Binary(TokenKind::Less, Constant("1", TypeRef::MakeUInt64()),
                 Constant("18446744073709551615", TypeRef::MakeUInt64()))
              .BooleanValue() == true);
}

TEST_CASE("integer casts sign-extend, reinterpret, and truncate like runtime stores") {
    const auto negative8 = Constant("-128", TypeRef::MakeInt8());
    REQUIRE(CastConstant(negative8, TypeRef::MakeInt64()).has_value());
    CHECK(CastConstant(negative8, TypeRef::MakeInt64())->ToLiteral() == "-128");
    CHECK(CastConstant(negative8, TypeRef::MakeUInt8())->ToLiteral() == "128");

    const auto high16 = Constant("65408", TypeRef::MakeUInt16());
    CHECK(CastConstant(high16, TypeRef::MakeInt16())->ToLiteral() == "-128");
    const auto wide = Constant("65535", TypeRef::MakeUInt32());
    CHECK(CastConstant(wide, TypeRef::MakeUInt8())->ToLiteral() == "255");

    // A cast to bool asks whether the whole source value is non-zero, so a value whose low byte is zero is still
    // true. Truncating to the bool's storage width first would have answered about the low byte instead.
    const auto lowByteZero = Constant("256", TypeRef::MakeUInt16());
    CHECK(CastConstant(lowByteZero, TypeRef::MakeBool8())->BooleanValue() == true);
    CHECK(CastConstant(lowByteZero, TypeRef::MakePrimitive(TypeRef::Kind::Bool64))->BooleanValue() == true);
    CHECK(CastConstant(Constant("0", TypeRef::MakeUInt16()), TypeRef::MakeBool8())->BooleanValue() == false);
    CHECK(CastConstant(Constant("true", TypeRef::MakeBool8()), TypeRef::MakeUInt64())->ToLiteral() == "1");
    CHECK_FALSE(CastConstant(wide, TypeRef::MakeFloat64()).has_value());
}
