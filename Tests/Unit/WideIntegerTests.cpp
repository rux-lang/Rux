#include "Numeric/IntegerLiteral.h"
#include "Numeric/WideInteger.h"

#include <array>
#include <cstdint>
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

TEST_CASE("wide addition and subtraction carry across every limb and wrap at the width") {
    const WideInteger one = Decimal("1", 128);
    const WideInteger lowWordMax = Decimal("18446744073709551615", 128);
    CHECK_EQ(lowWordMax.Added(one).ToDecimal(), "18446744073709551616");
    CHECK_EQ(lowWordMax.Added(one).Word64(0), 0);
    CHECK_EQ(lowWordMax.Added(one).Word64(1), 1);
    CHECK_EQ(WideInteger::AllOnes(128).Added(one), WideInteger::Zero(128));

    const WideInteger highWord = Decimal("18446744073709551616", 128);
    CHECK_EQ(highWord.Subtracted(one).ToDecimal(), "18446744073709551615");
    CHECK_EQ(WideInteger::Zero(128).Subtracted(one), WideInteger::AllOnes(128));
    CHECK_EQ(Decimal("123456789012345678901234567890", 128)
                 .Added(Decimal("9876543210", 128))
                 .Subtracted(Decimal("9876543210", 128))
                 .ToDecimal(),
             "123456789012345678901234567890");

    // A partial top limb wraps at the bit width rather than at its 32-bit storage boundary.
    CHECK_EQ(WideInteger::AllOnes(100).Added(WideInteger::FromUnsigned(1, 100)), WideInteger::Zero(100));
    CHECK_EQ(WideInteger::Zero(100).Subtracted(WideInteger::FromUnsigned(1, 100)), WideInteger::AllOnes(100));
}

TEST_CASE("wide bitwise operations normalize their result") {
    constexpr std::array<std::uint64_t, 2> leftWords{0xFFFF0000FFFF0000ULL, 0xAAAAAAAAAAAAAAAAULL};
    constexpr std::array<std::uint64_t, 2> rightWords{0x0F0F0F0F0F0F0F0FULL, 0x5555555555555555ULL};
    const WideInteger left = WideInteger::FromWords(leftWords, 100);
    const WideInteger right = WideInteger::FromWords(rightWords, 100);

    CHECK_EQ(left.BitwiseAnd(right).Word64(0), 0x0F0F00000F0F0000ULL);
    CHECK_EQ(left.BitwiseAnd(right).Word64(1), 0);
    CHECK_EQ(left.BitwiseOr(right).Word64(0), 0xFFFF0F0FFFFF0F0FULL);
    CHECK_EQ(left.BitwiseOr(right).Word64(1), 0x0000000FFFFFFFFFULL);
    CHECK_EQ(left.BitwiseXor(right).Word64(0), 0xF0F00F0FF0F00F0FULL);
    CHECK_EQ(left.BitwiseXor(right).Word64(1), 0x0000000FFFFFFFFFULL);
    CHECK_EQ(left.BitwiseNot().BitwiseNot(), left);
    CHECK_EQ(WideInteger::Zero(100).BitwiseNot(), WideInteger::AllOnes(100));
}

TEST_CASE("wide shifts cross limbs and distinguish logical from arithmetic fill") {
    const WideInteger one = WideInteger::FromUnsigned(1, 128);
    CHECK_EQ(one.ShiftedLeft(64).Word64(0), 0);
    CHECK_EQ(one.ShiftedLeft(64).Word64(1), 1);
    CHECK_EQ(one.ShiftedLeft(127), WideInteger::MinMagnitude(128, true));
    CHECK_EQ(one.ShiftedLeft(128), WideInteger::Zero(128));
    CHECK_EQ(WideInteger::MinMagnitude(128, true).ShiftedRight(127, false), one);
    CHECK_EQ(WideInteger::MinMagnitude(128, true).ShiftedRight(127, true).Word64(0), 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(WideInteger::MinMagnitude(128, true).ShiftedRight(127, true).Word64(1), 0xFFFFFFFFFFFFFFFFULL);
    CHECK_EQ(WideInteger::MinMagnitude(128, true).ShiftedRight(128, false), WideInteger::Zero(128));
    CHECK_EQ(WideInteger::MinMagnitude(128, true).ShiftedRight(128, true), WideInteger::AllOnes(128));

    const WideInteger partial = WideInteger::FromUnsigned(1, 100).ShiftedLeft(99);
    CHECK(partial.IsNegative());
    CHECK_EQ(partial.ShiftedRight(99, false).ToDecimal(), "1");
    CHECK_EQ(partial.ShiftedRight(99, true), WideInteger::AllOnes(100));
}

TEST_CASE("wide rotates wrap bits at arbitrary widths") {
    const WideInteger one128 = WideInteger::FromUnsigned(1, 128);
    CHECK_EQ(one128.RotatedLeft(1).ToDecimal(), "2");
    CHECK_EQ(one128.RotatedLeft(127), WideInteger::MinMagnitude(128, true));
    CHECK_EQ(one128.RotatedRight(1), WideInteger::MinMagnitude(128, true));
    CHECK_EQ(one128.RotatedLeft(128), one128);
    CHECK_EQ(one128.RotatedRight(257), WideInteger::MinMagnitude(128, true));

    const WideInteger one100 = WideInteger::FromUnsigned(1, 100);
    CHECK_EQ(one100.RotatedRight(1), one100.ShiftedLeft(99));
    CHECK_EQ(one100.RotatedRight(1).RotatedLeft(1), one100);
}

TEST_CASE("wide comparison applies signedness at each operand width") {
    const WideInteger unsignedMaximum = WideInteger::AllOnes(128);
    const WideInteger zero = WideInteger::Zero(128);
    CHECK(unsignedMaximum.Compare(zero, false) == std::strong_ordering::greater);
    CHECK(unsignedMaximum.Compare(zero, true) == std::strong_ordering::less);
    CHECK(WideInteger::FromUnsigned(0xFF, 8).Compare(WideInteger::AllOnes(128), true) == std::strong_ordering::equal);
    CHECK(WideInteger::FromUnsigned(0x80, 8).Compare(WideInteger::FromUnsigned(0x81, 8), true) ==
          std::strong_ordering::less);
}

TEST_CASE("wide bit counts respect the exact width") {
    CHECK_EQ(WideInteger::Zero(128).CountLeadingZeros(), 128);
    CHECK_EQ(WideInteger::Zero(128).CountTrailingZeros(), 128);
    CHECK_EQ(WideInteger::Zero(128).PopulationCount(), 0);
    CHECK_EQ(WideInteger::FromUnsigned(1, 128).CountLeadingZeros(), 127);
    CHECK_EQ(WideInteger::FromUnsigned(1, 128).CountTrailingZeros(), 0);
    CHECK_EQ(WideInteger::FromUnsigned(0x100000000ULL, 128).CountTrailingZeros(), 32);
    CHECK_EQ(WideInteger::AllOnes(128).PopulationCount(), 128);
    CHECK_EQ(WideInteger::AllOnes(100).CountLeadingZeros(), 0);
    CHECK_EQ(WideInteger::AllOnes(100).CountTrailingZeros(), 0);
    CHECK_EQ(WideInteger::AllOnes(100).PopulationCount(), 100);
}

TEST_CASE("full-width multiplication preserves both halves of the product") {
    const WideInteger maximum = WideInteger::AllOnes(128);
    const WideIntegerProduct square = maximum.MultipliedFull(maximum);
    CHECK_EQ(square.low.ToDecimal(), "1");
    CHECK_EQ(square.high.ToDecimal(), "340282366920938463463374607431768211454");
    CHECK_EQ(maximum.MultipliedWrapping(maximum).ToDecimal(), "1");

    // Splitting occurs at the exact value width, not at the next limb boundary.
    const WideInteger top100 = WideInteger::FromUnsigned(1, 100).ShiftedLeft(99);
    const WideIntegerProduct crosses100 = top100.MultipliedFull(WideInteger::FromUnsigned(2, 100));
    CHECK(crosses100.low.IsZero());
    CHECK_EQ(crosses100.high.ToDecimal(), "1");

    const WideIntegerProduct largest = WideInteger::AllOnes(512).MultipliedFull(WideInteger::AllOnes(512));
    CHECK_EQ(largest.low.ToDecimal(), "1");
    CHECK_EQ(largest.high, WideInteger::AllOnes(512).Subtracted(WideInteger::FromUnsigned(1, 512)));
}

TEST_CASE("checked multiplication applies unsigned and signed limits") {
    const WideInteger one = WideInteger::FromUnsigned(1, 128);
    const WideInteger two = WideInteger::FromUnsigned(2, 128);
    const WideInteger three = WideInteger::FromUnsigned(3, 128);
    const WideInteger unsignedMaximum = WideInteger::AllOnes(128);
    CHECK_EQ(unsignedMaximum.MultipliedChecked(one, false), unsignedMaximum);
    CHECK_FALSE(unsignedMaximum.MultipliedChecked(two, false).has_value());

    const WideInteger signedMaximum = WideInteger::MaxValue(128, true);
    const WideInteger signedMinimum = WideInteger::MinMagnitude(128, true);
    CHECK_EQ(signedMaximum.MultipliedChecked(one, true), signedMaximum);
    CHECK_FALSE(signedMaximum.MultipliedChecked(two, true).has_value());
    CHECK_EQ(signedMinimum.MultipliedChecked(one, true), signedMinimum);
    CHECK_FALSE(signedMinimum.MultipliedChecked(one.Negated(), true).has_value());

    const WideInteger negativeTwo = two.Negated();
    const auto negativeSix = negativeTwo.MultipliedChecked(three, true);
    REQUIRE(negativeSix.has_value());
    CHECK_EQ(*negativeSix, WideInteger::FromUnsigned(6, 128).Negated());
    CHECK_EQ(WideInteger::Zero(128).MultipliedChecked(signedMinimum, true), WideInteger::Zero(128));
}

TEST_CASE("unsigned wide division returns a quotient and remainder") {
    const WideInteger maximum = WideInteger::AllOnes(128);
    const WideInteger ten = WideInteger::FromUnsigned(10, 128);
    const WideIntegerDivision decimal = maximum.Divided(ten, false);
    REQUIRE(decimal.HasValue());
    CHECK_EQ(decimal.quotient.ToDecimal(), "34028236692093846346337460743176821145");
    CHECK_EQ(decimal.remainder.ToDecimal(), "5");
    CHECK_EQ(decimal.quotient.MultipliedWrapping(ten).Added(decimal.remainder), maximum);

    const WideInteger largest = WideInteger::AllOnes(512);
    const WideIntegerDivision halved = largest.Divided(WideInteger::FromUnsigned(2, 512), false);
    REQUIRE(halved.HasValue());
    CHECK_EQ(halved.quotient, largest.ShiftedRight(1, false));
    CHECK_EQ(halved.remainder.ToDecimal(), "1");
}

TEST_CASE("signed wide division truncates toward zero and gives the remainder the dividend sign") {
    const WideInteger hundred = WideInteger::FromUnsigned(100, 128);
    const WideInteger seven = WideInteger::FromUnsigned(7, 128);
    const WideIntegerDivision negativeDividend = hundred.Negated().Divided(seven, true);
    REQUIRE(negativeDividend.HasValue());
    CHECK_EQ(negativeDividend.quotient.Magnitude(true).ToDecimal(), "14");
    CHECK(negativeDividend.quotient.IsNegative());
    CHECK_EQ(negativeDividend.remainder.Magnitude(true).ToDecimal(), "2");
    CHECK(negativeDividend.remainder.IsNegative());

    const WideIntegerDivision negativeDivisor = hundred.Divided(seven.Negated(), true);
    REQUIRE(negativeDivisor.HasValue());
    CHECK(negativeDivisor.quotient.IsNegative());
    CHECK_EQ(negativeDivisor.remainder.ToDecimal(), "2");

    const WideInteger minimum = WideInteger::MinMagnitude(128, true);
    const WideIntegerDivision minimumByOne = minimum.Divided(WideInteger::FromUnsigned(1, 128), true);
    REQUIRE(minimumByOne.HasValue());
    CHECK_EQ(minimumByOne.quotient, minimum);
    CHECK(minimumByOne.remainder.IsZero());
}

TEST_CASE("wide division reports divide by zero and signed minimum overflow") {
    const WideInteger minimum = WideInteger::MinMagnitude(128, true);
    const WideIntegerDivision zero = minimum.Divided(WideInteger::Zero(128), true);
    CHECK_FALSE(zero.HasValue());
    CHECK_EQ(zero.error, WideIntegerDivisionError::DivideByZero);
    CHECK(zero.quotient.IsZero());
    CHECK(zero.remainder.IsZero());

    const WideIntegerDivision overflow = minimum.Divided(WideInteger::AllOnes(128), true);
    CHECK_FALSE(overflow.HasValue());
    CHECK_EQ(overflow.error, WideIntegerDivisionError::SignedOverflow);

    const WideIntegerDivision unsignedMinimum = minimum.Divided(WideInteger::AllOnes(128), false);
    REQUIRE(unsignedMinimum.HasValue());
    CHECK(unsignedMinimum.quotient.IsZero());
    CHECK_EQ(unsignedMinimum.remainder, minimum);
}

TEST_CASE("checked wide conversions preserve only representable numeric values") {
    const WideInteger negativeOne8 = WideInteger::AllOnes(8);
    const auto signedWide = negativeOne8.ConvertedChecked(128, true, true);
    REQUIRE(signedWide.has_value());
    CHECK_EQ(*signedWide, WideInteger::AllOnes(128));
    CHECK_FALSE(negativeOne8.ConvertedChecked(8, true, false).has_value());

    const WideInteger unsigned255 = WideInteger::FromUnsigned(255, 8);
    CHECK_FALSE(unsigned255.ConvertedChecked(8, false, true).has_value());
    const auto widenedSigned = unsigned255.ConvertedChecked(16, false, true);
    REQUIRE(widenedSigned.has_value());
    CHECK_EQ(widenedSigned->ToDecimal(), "255");

    for (const std::uint32_t width : {8U, 16U, 32U, 64U, 128U, 256U, 512U}) {
        CAPTURE(width);
        CHECK(WideInteger::MaxValue(width, false).ConvertedChecked(width, false, false).has_value());
        CHECK(WideInteger::MaxValue(width, true).ConvertedChecked(width, true, true).has_value());
        CHECK(WideInteger::MinMagnitude(width, true).ConvertedChecked(width, true, true).has_value());
    }
}

TEST_CASE("saturating wide conversions choose the nearest target endpoint") {
    CHECK_EQ(WideInteger::AllOnes(8).ConvertedSaturating(128, true, false), WideInteger::Zero(128));
    CHECK_EQ(WideInteger::FromUnsigned(255, 8).ConvertedSaturating(8, false, true), WideInteger::MaxValue(8, true));
    CHECK_EQ(WideInteger::MinMagnitude(128, true).ConvertedSaturating(8, true, true),
             WideInteger::MinMagnitude(8, true));
    CHECK_EQ(WideInteger::AllOnes(512).ConvertedSaturating(128, false, false), WideInteger::AllOnes(128));
}

TEST_CASE("wrapping and truncating conversions differ when a negative value widens") {
    const WideInteger negativeOne8 = WideInteger::AllOnes(8);
    CHECK_EQ(negativeOne8.ConvertedWrapping(128, true), WideInteger::AllOnes(128));
    CHECK_EQ(negativeOne8.ConvertedTruncating(128).ToDecimal(), "255");
    CHECK_EQ(negativeOne8.ConvertedWrapping(4, true), WideInteger::AllOnes(4));
    CHECK_EQ(negativeOne8.ConvertedTruncating(4), WideInteger::AllOnes(4));

    const WideInteger largest = WideInteger::AllOnes(512);
    CHECK_EQ(largest.ConvertedWrapping(128, false), WideInteger::AllOnes(128));
    CHECK_EQ(largest.ConvertedTruncating(128), WideInteger::AllOnes(128));
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
    constexpr std::array<std::uint64_t, 3> words{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0xFFFFFFFFFFFFFFFFULL};
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
    for (const std::string_view suffix :
         {"i",   "i8",   "i16",  "i32",  "i64", "i128", "i256", "i512", "u",   "u8",   "u16",  "u32",
          "u64", "u128", "u256", "u512", "f8",  "f16",  "f32",  "f64",  "f80", "f128", "f256", "f512"}) {
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
    CHECK_EQ(NumericLiteralSuffixOf("0xff8"), "");
    CHECK_EQ(NumericLiteralSuffixOf("0xff128"), "");
    CHECK_EQ(NumericLiteralSuffixOf("0xFFu8"), "u8");
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
