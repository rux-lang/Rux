#include "Numeric/IntegerLiteral.h"
#include "Numeric/WideInteger.h"

#include <doctest.h>
#include <string>

using namespace Rux;

namespace {
/// The decimal spellings of the boundaries the wide widths are checked against, written out rather than computed so
/// the test does not lean on the code it is checking.
constexpr std::string_view UInt128Max = "340282366920938463463374607431768211455";
constexpr std::string_view Int128Max = "170141183460469231731687303715884105727";
constexpr std::string_view Int128MinMagnitude = "170141183460469231731687303715884105728";
constexpr std::string_view UInt256Max =
    "115792089237316195423570985008687907853269984665640564039457584007913129639935";
constexpr std::string_view UInt512Max =
    "1340780792994259709957402499820584612747936582059239337772356144372176403007354697680187429816690342"
    "7690031858186486050853753882811946569946433649006084095";

[[nodiscard]] WideInteger Decimal(const std::string_view digits, const std::uint32_t width = WideInteger::MaxBits) {
    const auto value = WideInteger::Parse(digits, 10, width);
    REQUIRE(value.has_value());
    return *value;
}
} // namespace

TEST_CASE("a wide value round-trips through decimal at every width") {
    CHECK_EQ(Decimal("0").ToDecimal(), "0");
    CHECK_EQ(Decimal("1").ToDecimal(), "1");
    CHECK_EQ(Decimal("999999999").ToDecimal(), "999999999");
    CHECK_EQ(Decimal("1000000000").ToDecimal(), "1000000000");
    CHECK_EQ(Decimal("18446744073709551615").ToDecimal(), "18446744073709551615");
    CHECK_EQ(Decimal(UInt128Max).ToDecimal(), UInt128Max);
    CHECK_EQ(Decimal(UInt256Max).ToDecimal(), UInt256Max);
    CHECK_EQ(Decimal(UInt512Max).ToDecimal(), UInt512Max);
}

TEST_CASE("every base decodes to the same value") {
    const WideInteger expected = Decimal("255");
    CHECK_EQ(WideInteger::Parse("ff", 16, 64), expected.Truncated(64));
    CHECK_EQ(WideInteger::Parse("FF", 16, 64), expected.Truncated(64));
    CHECK_EQ(WideInteger::Parse("11111111", 2, 64), expected.Truncated(64));
    CHECK_EQ(WideInteger::Parse("377", 8, 64), expected.Truncated(64));
    // Underscores are separators wherever they fall between digits.
    CHECK_EQ(WideInteger::Parse("1111_1111", 2, 64), expected.Truncated(64));
    CHECK_EQ(WideInteger::Parse("2_5_5", 10, 64), expected.Truncated(64));
}

TEST_CASE("a value too wide for its width is refused rather than truncated") {
    CHECK_FALSE(WideInteger::Parse("256", 10, 8).has_value());
    CHECK(WideInteger::Parse("255", 10, 8).has_value());
    CHECK_FALSE(WideInteger::Parse("65536", 10, 16).has_value());
    CHECK_FALSE(WideInteger::Parse(UInt128Max, 10, 127).has_value());
    CHECK(WideInteger::Parse(UInt128Max, 10, 128).has_value());
    CHECK_FALSE(WideInteger::Parse(UInt512Max, 10, 511).has_value());
    CHECK(WideInteger::Parse(UInt512Max, 10, 512).has_value());
}

TEST_CASE("a malformed literal decodes to nothing") {
    CHECK_FALSE(WideInteger::Parse("", 10, 64).has_value());
    CHECK_FALSE(WideInteger::Parse("_", 10, 64).has_value());
    CHECK_FALSE(WideInteger::Parse("12x", 10, 64).has_value());
    CHECK_FALSE(WideInteger::Parse("2", 2, 64).has_value());
    CHECK_FALSE(WideInteger::Parse("8", 8, 64).has_value());
    CHECK_FALSE(WideInteger::Parse("g", 16, 64).has_value());
}

TEST_CASE("a width reports the largest value it holds") {
    CHECK_EQ(WideInteger::MaxValue(8, false).ToDecimal(), "255");
    CHECK_EQ(WideInteger::MaxValue(8, true).ToDecimal(), "127");
    CHECK_EQ(WideInteger::MinMagnitude(8, true).ToDecimal(), "128");
    CHECK_EQ(WideInteger::MaxValue(64, false).ToDecimal(), "18446744073709551615");
    CHECK_EQ(WideInteger::MaxValue(128, false).ToDecimal(), UInt128Max);
    CHECK_EQ(WideInteger::MaxValue(128, true).ToDecimal(), Int128Max);
    CHECK_EQ(WideInteger::MinMagnitude(128, true).ToDecimal(), Int128MinMagnitude);
    CHECK_EQ(WideInteger::MaxValue(256, false).ToDecimal(), UInt256Max);
    CHECK_EQ(WideInteger::MaxValue(512, false).ToDecimal(), UInt512Max);
    // An unsigned width has no negative end, so its smallest value is zero.
    CHECK(WideInteger::MinMagnitude(64, false).IsZero());
}

TEST_CASE("negation is two's complement at the value's own width") {
    CHECK(WideInteger::Zero(64).Negated().IsZero());
    CHECK_EQ(Decimal("1", 8).Negated().ToDecimal(), "255");
    CHECK_EQ(Decimal("1", 64).Negated().ToDecimal(), "18446744073709551615");
    CHECK_EQ(Decimal("1", 128).Negated().ToDecimal(), UInt128Max);
    // The most negative value negates to itself, which is the one value that has no positive counterpart.
    const WideInteger mostNegative = WideInteger::MinMagnitude(128, true);
    CHECK_EQ(mostNegative.Negated().ToDecimal(), mostNegative.ToDecimal());
}

TEST_CASE("a value reads back as words and as a machine word when it fits") {
    const WideInteger small = Decimal("18446744073709551615", 128);
    REQUIRE(small.ToUnsigned().has_value());
    CHECK_EQ(*small.ToUnsigned(), 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(small.Word64(0), 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(small.Word64(1), 0);

    const WideInteger wide = Decimal(UInt128Max, 128);
    CHECK_FALSE(wide.ToUnsigned().has_value());
    CHECK_EQ(wide.Word64(0), 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(wide.Word64(1), 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(wide.Word64(2), 0);
}

TEST_CASE("little-endian words are normalized to the requested width") {
    constexpr std::array words{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0xFFFFFFFFFFFFFFFFULL};
    const WideInteger value = WideInteger::FromWords(words, 100);
    CHECK_EQ(value.Width(), 100);
    CHECK_EQ(value.Word64(0), words[0]);
    CHECK_EQ(value.Word64(1), 0x0000000876543210ULL);
    CHECK_EQ(value.Word64(2), 0);
}

TEST_CASE("signedness helpers interpret the top bit without changing the stored pattern") {
    const WideInteger positive = WideInteger::FromUnsigned(0x7F, 8);
    CHECK_FALSE(positive.IsNegative());
    CHECK_EQ(positive.Magnitude(true), positive);
    CHECK_EQ(positive.Extended(128, true).ToDecimal(), "127");

    const WideInteger negative = WideInteger::FromUnsigned(0x80, 8);
    CHECK(negative.IsNegative());
    CHECK_EQ(negative.Magnitude(true).ToDecimal(), "128");
    CHECK_EQ(negative.Magnitude(false), negative);
    CHECK_EQ(negative.Extended(128, true).Word64(0), 0xFFFFFFFFFFFFFF80ULL);
    CHECK_EQ(negative.Extended(128, true).Word64(1), 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(negative.Extended(128, false).ToDecimal(), "128");

    // The sign bit need not coincide with a limb boundary.
    const WideInteger partial = WideInteger::FromUnsigned(0x100, 9);
    CHECK(partial.IsNegative());
    CHECK_EQ(partial.Extended(16, true).Word64(0), 0xFF00);
    CHECK_EQ(partial.Extended(8, true).ToDecimal(), "0");
}

TEST_CASE("truncating keeps the low bits and nothing above them") {
    CHECK_EQ(Decimal("511", 128).Truncated(8).ToDecimal(), "255");
    CHECK_EQ(Decimal("256", 128).Truncated(8).ToDecimal(), "0");
    CHECK_EQ(Decimal(UInt128Max, 128).Truncated(64).ToDecimal(), "18446744073709551615");
}

TEST_CASE("values order by magnitude") {
    CHECK(Decimal("1") < Decimal("2"));
    CHECK(Decimal(UInt128Max) > Decimal("18446744073709551615"));
    CHECK(Decimal(UInt512Max) > Decimal(UInt256Max));
    CHECK(Decimal("7") == Decimal("7"));
    CHECK_FALSE(Decimal("7") == Decimal("8"));
    // The width a value was built at does not change what it is worth.
    CHECK(Decimal("7", 8) == Decimal("7", 512));
}

TEST_CASE("the suffix table names every integer and float width that has a literal") {
    for (const std::string_view suffix : {"i", "i8", "i16", "i32", "i64", "i128", "i256", "i512", "u", "u8", "u16",
                                          "u32", "u64", "u128", "u256", "u512", "f32", "f64"}) {
        CAPTURE(suffix);
        CHECK(FindNumericLiteralSuffix(suffix) != nullptr);
    }
    CHECK_EQ(FindNumericLiteralSuffix("i1024"), nullptr);
    CHECK_EQ(FindNumericLiteralSuffix("q"), nullptr);
    CHECK_EQ(FindNumericLiteralSuffix(""), nullptr);
}

TEST_CASE("the longest suffix wins so a wide one is not read as a narrow one") {
    CHECK_EQ(NumericLiteralSuffixOf("1u128"), "u128");
    CHECK_EQ(NumericLiteralSuffixOf("1i512"), "i512");
    CHECK_EQ(NumericLiteralSuffixOf("1u8"), "u8");
    CHECK_EQ(NumericLiteralSuffixOf("1u"), "u");
    CHECK_EQ(NumericLiteralSuffixOf("42"), "");
    // Hexadecimal digits are not a suffix, however much they look like one.
    CHECK_EQ(NumericLiteralSuffixOf("0xff"), "");
}

TEST_CASE("a literal splits into its sign, base, digits and suffix") {
    const auto hex = SplitIntegerLiteral("0xFF_FFu256");
    REQUIRE(hex.has_value());
    CHECK_FALSE(hex->negative);
    CHECK_EQ(hex->base, 16);
    CHECK_EQ(hex->digits, "FF_FF");
    CHECK_EQ(hex->suffix, "u256");

    const auto negative = SplitIntegerLiteral("-128i8");
    REQUIRE(negative.has_value());
    CHECK(negative->negative);
    CHECK_EQ(negative->base, 10);
    CHECK_EQ(negative->digits, "128");
    CHECK_EQ(negative->suffix, "i8");

    CHECK_FALSE(SplitIntegerLiteral("").has_value());
    // Splitting reports the parts it finds and does not judge them; a run of non-digits survives the split and is
    // refused when it is decoded.
    CHECK(SplitIntegerLiteral("u8").has_value());
    CHECK_FALSE(DecodeIntegerLiteral("u8", WideInteger::MaxBits).has_value());
}

TEST_CASE("a literal fits the width that can hold its magnitude") {
    const auto fits = [](const std::string_view digits, const bool negative, const std::uint32_t width,
                         const bool isSigned) {
        const auto magnitude = WideInteger::Parse(digits, 10, WideInteger::MaxBits);
        REQUIRE(magnitude.has_value());
        return IntegerLiteralFits(*magnitude, negative, width, isSigned);
    };

    CHECK(fits("255", false, 8, false));
    CHECK_FALSE(fits("256", false, 8, false));
    CHECK(fits("127", false, 8, true));
    CHECK_FALSE(fits("128", false, 8, true));
    // The most negative value is written as a magnitude one past the largest positive one.
    CHECK(fits("128", true, 8, true));
    CHECK_FALSE(fits("129", true, 8, true));
    // An unsigned width takes no negative literal, except the one that is not really negative.
    CHECK_FALSE(fits("1", true, 8, false));
    CHECK(fits("0", true, 8, false));

    CHECK(fits(UInt128Max, false, 128, false));
    CHECK_FALSE(fits(UInt128Max, false, 128, true));
    CHECK(fits(Int128Max, false, 128, true));
    CHECK(fits(Int128MinMagnitude, true, 128, true));
    CHECK_FALSE(fits(Int128Max, false, 127, true));
    CHECK(fits(UInt256Max, false, 256, false));
    CHECK_FALSE(fits(UInt256Max, false, 255, false));
    CHECK(fits(UInt512Max, false, 512, false));
}

TEST_CASE("decoding a literal reports the magnitude without its sign") {
    const auto negative = DecodeIntegerLiteral("-128i8", WideInteger::MaxBits);
    REQUIRE(negative.has_value());
    CHECK_EQ(negative->ToDecimal(), "128");

    const auto wide = DecodeIntegerLiteral("340282366920938463463374607431768211455u128", WideInteger::MaxBits);
    REQUIRE(wide.has_value());
    CHECK_EQ(wide->ToDecimal(), UInt128Max);

    // Past the widest width there is, so nothing could hold it.
    CHECK_FALSE(DecodeIntegerLiteral(std::string(200, '9'), WideInteger::MaxBits).has_value());
}
