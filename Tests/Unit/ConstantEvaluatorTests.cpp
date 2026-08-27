#include "Numeric/FloatEncoding.h"
#include "Numeric/FloatParsing.h"
#include "Optimization/ConstantEvaluator.h"
#include "Semantic/PrimitiveCatalog.h"

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
}

TEST_CASE("constant evaluation uses the same arithmetic through 512 bits") {
    const TypeRef uint128 = TypeRef::MakePrimitive(TypeRef::Kind::UInt128);
    const TypeRef int128 = TypeRef::MakePrimitive(TypeRef::Kind::Int128);
    const TypeRef uint256 = TypeRef::MakePrimitive(TypeRef::Kind::UInt256);
    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    constexpr std::string_view uint128Maximum = "340282366920938463463374607431768211455";
    constexpr std::string_view int128Maximum = "170141183460469231731687303715884105727";
    constexpr std::string_view int128Minimum = "-170141183460469231731687303715884105728";
    constexpr std::string_view uint512Maximum =
        "1340780792994259709957402499820584612747936582059239337772356144372176403007354697680187429816690342"
        "7690031858186486050853753882811946569946433649006084095";

    CHECK(Constant(uint512Maximum, uint512).ToLiteral() == uint512Maximum);
    CHECK(Binary(TokenKind::Plus, Constant(uint128Maximum, uint128), Constant("1", uint128)).ToLiteral() == "0");
    CHECK(Binary(TokenKind::Plus, Constant(int128Maximum, int128), Constant("1", int128)).ToLiteral() == int128Minimum);
    CHECK(Binary(TokenKind::Minus, Constant(int128Minimum, int128), Constant("1", int128)).ToLiteral() ==
          int128Maximum);
    CHECK(Binary(TokenKind::Star, Constant("1606938044258990275541962092341162602522202993782792835301376", uint256),
                 Constant("1267650600228229401496703205376", uint256))
              .ToLiteral() == "0");
    CHECK(Binary(TokenKind::Slash, Constant(uint128Maximum, uint128), Constant("10", uint128)).ToLiteral() ==
          "34028236692093846346337460743176821145");
    CHECK(Binary(TokenKind::Percent, Constant(uint128Maximum, uint128), Constant("10", uint128)).ToLiteral() == "5");
    CHECK(Binary(TokenKind::Greater, Constant(uint512Maximum, uint512), Constant(uint128Maximum, uint512))
              .BooleanValue() == true);
}

TEST_CASE("wide constant shifts and casts preserve complete bit patterns") {
    const TypeRef int128 = TypeRef::MakePrimitive(TypeRef::Kind::Int128);
    const TypeRef int256 = TypeRef::MakePrimitive(TypeRef::Kind::Int256);
    const TypeRef uint128 = TypeRef::MakePrimitive(TypeRef::Kind::UInt128);
    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    const TypeRef uint16 = TypeRef::MakeUInt16();
    constexpr std::string_view uint512Maximum =
        "1340780792994259709957402499820584612747936582059239337772356144372176403007354697680187429816690342"
        "7690031858186486050853753882811946569946433649006084095";

    CHECK(Binary(TokenKind::LessLess, Constant("1", uint128), Constant("127", uint16)).ToLiteral() ==
          "170141183460469231731687303715884105728");
    CHECK(Binary(TokenKind::GreaterGreater, Constant("-8", int128), Constant("2", uint16)).ToLiteral() == "-2");
    CHECK(Binary(TokenKind::GreaterGreaterGreater, Constant("-8", int128), Constant("2", uint16)).ToLiteral() ==
          "85070591730234615865843651857942052862");

    const auto negativeWide = CastConstant(Constant("-1", int128), int256);
    REQUIRE(negativeWide.has_value());
    CHECK_EQ(negativeWide->ToLiteral(), "-1");
    const auto narrowed = CastConstant(Constant(uint512Maximum, uint512), uint128);
    REQUIRE(narrowed.has_value());
    CHECK_EQ(narrowed->ToLiteral(), "340282366920938463463374607431768211455");
    CHECK(CastConstant(Constant("18446744073709551616", uint128), TypeRef::MakeBool8())->BooleanValue() == true);
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
    const auto asFloat = CastConstant(wide, TypeRef::MakeFloat64());
    REQUIRE(asFloat.has_value());
    CHECK_EQ(CastConstant(*asFloat, TypeRef::MakeUInt32())->ToLiteral(), "65535");
}

TEST_CASE("floating constants use exact decimal rounding at every width") {
    const TypeRef float8 = TypeRef::MakePrimitive(TypeRef::Kind::Float8);
    CHECK_EQ(Constant("1.0625", float8).RawBits(), 0x38);
    CHECK_EQ(Constant("1.1875", float8).RawBits(), 0x3A);
    CHECK_EQ(Constant("0.1", TypeRef::MakeFloat32()).RawBits(), 0x3DCCCCCD);
    CHECK_EQ(Constant("0.1", TypeRef::MakeFloat64()).RawBits(), 0x3FB999999999999AULL);
    CHECK_EQ(Constant("1e1000", float8).RawBits(), 0x78);
    CHECK_EQ(Constant("-1e-1000", float8).RawBits(), 0x80);

    for (const FloatFormat &format : FloatFormats()) {
        CAPTURE(format.name);
        const TypeRef type = TypeRef::MakePrimitive(FindPrimitive("float" + std::to_string(format.valueBits))->kind);
        const TypedConstant value = Constant("0.1", type);
        CHECK_EQ(Constant(value.ToLiteral(), type).Bits(), value.Bits());
        CHECK_EQ(Constant("1.5", type).Bits(), ParseFloatEncoding("1.5", format)->Bits());
    }
}

TEST_CASE("floating constant arithmetic mirrors software runtime kernels") {
    const TypeRef float8 = TypeRef::MakePrimitive(TypeRef::Kind::Float8);
    const TypedConstant one = Constant("1.0", float8);
    const TypedConstant half = Constant("0.5", float8);
    CHECK_EQ(Binary(TokenKind::Plus, one, half).RawBits(), 0x3C);
    CHECK_EQ(Binary(TokenKind::Minus, one, half).RawBits(), 0x30);
    CHECK_EQ(Binary(TokenKind::Star, Constant("1.5", float8), Constant("1.5", float8)).RawBits(), 0x41);
    CHECK_EQ(Binary(TokenKind::Slash, one, Constant("3.0", float8)).RawBits(), 0x2B);
    CHECK_EQ(Binary(TokenKind::Percent, Constant("5.0", float8), Constant("2.0", float8)).RawBits(), 0x38);
    CHECK(Binary(TokenKind::Less, half, one).BooleanValue() == true);
    CHECK(Binary(TokenKind::BangEqual, Constant("nan", float8), Constant("nan", float8)).BooleanValue() == true);
    CHECK(Binary(TokenKind::Equal, Constant("nan", float8), Constant("nan", float8)).BooleanValue() == false);
    CHECK_EQ(EvaluateUnary(TokenKind::Minus, one)->RawBits(), 0xB8);
}

TEST_CASE("floating constant casts share checked software conversions") {
    const TypeRef float8 = TypeRef::MakePrimitive(TypeRef::Kind::Float8);
    const TypeRef float16 = TypeRef::MakePrimitive(TypeRef::Kind::Float16);
    REQUIRE(CastConstant(Constant("127", TypeRef::MakeInt16()), float8).has_value());
    CHECK_EQ(CastConstant(Constant("127", TypeRef::MakeInt16()), float8)->RawBits(), 0x70);
    CHECK_EQ(CastConstant(Constant("-5.5", float8), TypeRef::MakeInt8())->ToLiteral(), "-5");
    CHECK_FALSE(CastConstant(Constant("240.0", float8), TypeRef::MakeInt8()).has_value());
    CHECK_FALSE(CastConstant(Constant("infinity", float8), TypeRef::MakeInt32()).has_value());
    CHECK_EQ(CastConstant(Constant("1.5", float8), float16)->RawBits(), 0x3E00);
    CHECK(CastConstant(Constant("nan", float8), TypeRef::MakeBool())->BooleanValue() == true);
    CHECK(CastConstant(Constant("-0.0", float8), TypeRef::MakeBool())->BooleanValue() == false);
}
