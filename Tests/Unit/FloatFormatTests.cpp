#include "Lexer/Lexer.h"
#include "Numeric/FloatEncoding.h"
#include "Numeric/FloatFormat.h"
#include "Numeric/FloatLiteral.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Types/PrimitiveCatalog.h"

#include <array>
#include <cstdint>
#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;

namespace {
[[nodiscard]] const FloatFormat &Format(const std::uint32_t bits) {
    const FloatFormat *format = FindFloatFormat(bits);
    REQUIRE(format != nullptr);
    return *format;
}

[[nodiscard]] std::uint64_t Word(const FloatEncoding &encoding, const std::size_t index = 0) {
    return encoding.Bits().Word64(index);
}
} // namespace

TEST_CASE("every float width has one exact binary layout") {
    struct Expected {
        std::uint32_t bits;
        std::uint32_t bytes;
        std::uint32_t exponent;
        std::uint32_t precision;
        std::int32_t bias;
        bool explicitIntegerBit;
    };

    constexpr std::array expected{
        Expected{8, 1, 4, 4, 7, false},
        Expected{16, 2, 5, 11, 15, false},
        Expected{32, 4, 8, 24, 127, false},
        Expected{64, 8, 11, 53, 1023, false},
        Expected{80, 16, 15, 64, 16383, true},
        Expected{128, 16, 15, 113, 16383, false},
        Expected{256, 32, 19, 237, 262143, false},
        Expected{512, 64, 23, 489, 4194303, false},
    };

    const auto formats = FloatFormats();
    REQUIRE_EQ(formats.size(), expected.size());
    for (std::size_t index = 0; index < formats.size(); ++index) {
        CAPTURE(index);
        CHECK_EQ(formats[index].valueBits, expected[index].bits);
        CHECK_EQ(formats[index].storageBytes, expected[index].bytes);
        CHECK_EQ(formats[index].exponentBits, expected[index].exponent);
        CHECK_EQ(formats[index].precisionBits, expected[index].precision);
        CHECK_EQ(formats[index].exponentBias, expected[index].bias);
        CHECK_EQ(formats[index].explicitIntegerBit, expected[index].explicitIntegerBit);
        CHECK_EQ(formats[index].SignificandFieldBits() + formats[index].exponentBits + 1, formats[index].valueBits);
    }
    CHECK_EQ(Format(8).name, "binary8-e4m3");
    CHECK_EQ(Format(80).name, "extended80");
    CHECK_EQ(FindFloatFormat(24), nullptr);
}

TEST_CASE("float layouts agree with the primitive catalog") {
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        if (primitive.category != PrimitiveCategory::Float) {
            continue;
        }
        CAPTURE(primitive.name);
        const FloatFormat &format = Format(primitive.bits);
        CHECK_EQ(format.valueBits, primitive.bits);
        CHECK_EQ(format.storageBytes, primitive.size);
    }
}

TEST_CASE("IEEE special values have canonical encodings at every width") {
    for (const FloatFormat &format : FloatFormats()) {
        CAPTURE(format.name);
        const FloatEncoding positiveZero = FloatEncoding::Zero(format);
        const FloatEncoding negativeZero = FloatEncoding::Zero(format, true);
        const FloatEncoding infinity = FloatEncoding::Infinity(format);
        const FloatEncoding quietNaN = FloatEncoding::QuietNaN(format);
        const FloatEncoding signalingNaN = FloatEncoding::SignalingNaN(format);
        CHECK_EQ(positiveZero.Classify(), FloatClass::Zero);
        CHECK_EQ(negativeZero.Classify(), FloatClass::Zero);
        CHECK_FALSE(positiveZero.IsNegative());
        CHECK(negativeZero.IsNegative());
        CHECK_EQ(infinity.Classify(), FloatClass::Infinity);
        CHECK_EQ(quietNaN.Classify(), FloatClass::QuietNaN);
        CHECK_EQ(signalingNaN.Classify(), FloatClass::SignalingNaN);
        CHECK(infinity.IsCanonical());
        CHECK(quietNaN.IsCanonical());
        CHECK(signalingNaN.IsCanonical());
        CHECK_EQ(FloatEncoding::MaxFinite(format).Classify(), FloatClass::Normal);
        CHECK_EQ(FloatEncoding::MinPositiveNormal(format).Classify(), FloatClass::Normal);
        CHECK_EQ(FloatEncoding::MinPositiveSubnormal(format).Classify(), FloatClass::Subnormal);
        CHECK_EQ(infinity.ToLittleEndianBytes().size(), format.storageBytes);
    }

    CHECK_EQ(Word(FloatEncoding::Infinity(Format(8))), 0x78);
    CHECK_EQ(Word(FloatEncoding::QuietNaN(Format(8))), 0x7C);
    CHECK_EQ(Word(FloatEncoding::SignalingNaN(Format(8))), 0x79);
    CHECK_EQ(Word(FloatEncoding::MaxFinite(Format(8))), 0x77);
    CHECK_EQ(Word(FloatEncoding::MinPositiveNormal(Format(8))), 0x08);
    CHECK_EQ(Word(FloatEncoding::MinPositiveSubnormal(Format(8))), 0x01);

    CHECK_EQ(Word(FloatEncoding::Infinity(Format(16))), 0x7C00);
    CHECK_EQ(Word(FloatEncoding::QuietNaN(Format(16))), 0x7E00);
    CHECK_EQ(Word(FloatEncoding::SignalingNaN(Format(16))), 0x7C01);
    CHECK_EQ(Word(FloatEncoding::Infinity(Format(32))), 0x7F800000);
    CHECK_EQ(Word(FloatEncoding::Infinity(Format(64))), 0x7FF0000000000000ULL);
}

TEST_CASE("extended80 keeps its explicit integer bit and zero storage padding") {
    const FloatFormat &format = Format(80);
    const FloatEncoding infinity = FloatEncoding::Infinity(format);
    CHECK_EQ(Word(infinity), 0x8000000000000000ULL);
    CHECK_EQ(Word(infinity, 1), 0x7FFF);
    CHECK_EQ(Word(FloatEncoding::QuietNaN(format)), 0xC000000000000000ULL);
    CHECK_EQ(Word(FloatEncoding::SignalingNaN(format)), 0x8000000000000001ULL);

    const auto bytes = infinity.ToLittleEndianBytes();
    REQUIRE_EQ(bytes.size(), 16);
    for (std::size_t index = 10; index < bytes.size(); ++index) {
        CHECK_EQ(bytes[index], 0);
    }

    const WideInteger pseudoDenormalBits = WideInteger::FromUnsigned(0x8000000000000000ULL, 80);
    const FloatEncoding pseudoDenormal = FloatEncoding::FromBits(format, pseudoDenormalBits);
    CHECK_EQ(pseudoDenormal.Classify(), FloatClass::Normal);
    CHECK_FALSE(pseudoDenormal.IsCanonical());

    const WideInteger unnormalBits =
        WideInteger::FromUnsigned(1, 80).ShiftedLeft(64).BitwiseOr(WideInteger::FromUnsigned(1, 80));
    const FloatEncoding unnormal = FloatEncoding::FromBits(format, unnormalBits);
    CHECK_EQ(unnormal.Classify(), FloatClass::Invalid);
    CHECK_FALSE(unnormal.IsCanonical());
}

TEST_CASE("floating literals retain an exact normalized decimal value") {
    const auto finite = SplitFloatLiteral("1_234.500_0e-2f128");
    REQUIRE(finite.has_value());
    CHECK_EQ(finite->kind, FloatLiteralKind::Finite);
    CHECK_FALSE(finite->negative);
    CHECK_EQ(finite->digits, "12345");
    CHECK_EQ(finite->decimalExponent, -3);
    CHECK_EQ(finite->suffix, "f128");

    const auto zero = SplitFloatLiteral("-0.000f8");
    REQUIRE(zero.has_value());
    CHECK(zero->negative);
    CHECK_EQ(zero->digits, "0");
    CHECK_EQ(zero->decimalExponent, 0);

    const auto infinity = SplitFloatLiteral("-infinityf512");
    REQUIRE(infinity.has_value());
    CHECK(infinity->negative);
    CHECK_EQ(infinity->kind, FloatLiteralKind::Infinity);
    CHECK_EQ(infinity->suffix, "f512");
    REQUIRE(SplitFloatLiteral("nanf16").has_value());
    CHECK_EQ(SplitFloatLiteral("nanf16")->kind, FloatLiteralKind::QuietNaN);
    REQUIRE(SplitFloatLiteral("snan").has_value());
    CHECK_EQ(SplitFloatLiteral("snan")->kind, FloatLiteralKind::SignalingNaN);

    CHECK_FALSE(SplitFloatLiteral("1__2.0").has_value());
    CHECK_FALSE(SplitFloatLiteral("1.0e+").has_value());
    CHECK_FALSE(SplitFloatLiteral("1.0i32").has_value());
    CHECK_FALSE(SplitFloatLiteral("1.2.3").has_value());
}

TEST_CASE("every float suffix resolves to its declared primitive width") {
    for (const std::string_view suffix : {"f8", "f16", "f80", "f128", "f256", "f512"}) {
        const std::string source = "func Main() { let value = 1.0" + std::string(suffix) + "; }";
        Lexer lexer(source, "float.rux");
        auto lexed = lexer.Tokenize();
        REQUIRE_FALSE(lexed.HasErrors());
        Parser parser(std::move(lexed.tokens), "float.rux");
        auto parsed = parser.Parse();
        REQUIRE_FALSE(parsed.HasErrors());
        SemanticAnalyzer analyzer({&parsed.module}, {}, "Float", "Windows");
        const auto diagnostics = analyzer.Analyze().diagnostics;
        REQUIRE_EQ(diagnostics.size(), 1);
        CHECK(diagnostics.front().message.contains("primitive type 'float" + std::string(suffix.substr(1)) + "'"));
    }
}
