#include "Lexer/Lexer.h"
#include "Numeric/FloatEncoding.h"
#include "Numeric/FloatFormat.h"
#include "Numeric/FloatLiteral.h"
#include "Numeric/FloatParsing.h"
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
    // Each literal is annotated with its suffix's width and with another one. It must satisfy exactly the first: no
    // implicit conversion joins two of these widths, so a literal typed at the wrong width is refused.
    for (const std::string_view suffix : {"f8", "f16", "f80", "f128", "f256", "f512"}) {
        const std::string width(suffix.substr(1));
        for (const bool matching : {true, false}) {
            CAPTURE(suffix);
            CAPTURE(matching);
            const std::string annotated = matching ? width : (width == "8" ? "16" : "8");
            const std::string source =
                "func Main() { let value: float" + annotated + " = 1.0" + std::string(suffix) + "; }";
            Lexer lexer(source, "float.rux");
            auto lexed = lexer.Tokenize();
            REQUIRE_FALSE(lexed.HasErrors());
            Parser parser(std::move(lexed.tokens), "float.rux");
            auto parsed = parser.Parse();
            REQUIRE_FALSE(parsed.HasErrors());
            SemanticAnalyzer analyzer({&parsed.module}, {}, "Float", "Windows");
            const auto diagnostics = analyzer.Analyze().diagnostics;
            if (matching) {
                CHECK(diagnostics.empty());
            }
            else {
                REQUIRE_EQ(diagnostics.size(), 1);
                CHECK(diagnostics.front().message.contains("'float" + width + "'"));
            }
        }
    }
}

TEST_CASE("float512 extreme decimal literals round exactly without exponent-sized arithmetic") {
    const auto &format = Format(512);
    const auto maximum =
        ParseFloatEncoding("2065063539835887924399119494581650169527436049302967034784166417680004155333565293605750967"
                           "753552715142786823706457493482454929376754635907792000880558e1262461",
                           format);
    const auto minimum =
        ParseFloatEncoding("1936986403971803627488623572428543460963250743569242152976820404263225780984727937030763605"
                           "544545579208354072281172669513916893467785427815424510311479e-1262761",
                           format);
    REQUIRE(maximum.has_value());
    REQUIRE(minimum.has_value());
    CHECK(maximum->Bits() == FloatEncoding::MaxFinite(format).Bits());
    CHECK(minimum->Bits() == FloatEncoding::MinPositiveNormal(format).Bits());
    const auto negative =
        ParseFloatEncoding("-193698640397180362748862357242854346096325074356924215297682040426322578098472793703076360"
                           "5544545579208354072281172669513916893467785427815424510311479e-1262761",
                           format);
    REQUIRE(negative.has_value());
    CHECK(negative->Bits() == minimum->Bits().BitwiseOr(WideInteger::FromUnsigned(1, 512).ShiftedLeft(511)));
}

TEST_CASE("bounded decimal intervals fall back at an exact subnormal halfway") {
    // 5^1075 * 10^-1075 is exactly 2^-1075, halfway from zero to binary64's least subnormal.
    // Its power of five exceeds the interval precision, so its enclosing endpoints round differently.
    std::string digits =
        "2470328229206232720882843964341106861825299013071623822127928412503377536351043759326499181808179961"
        "8989828234772285886546332835517796989819938739800539093906315035659515570226392290858392449105184435"
        "9318028499365361525003193704576782492193656236698636584807570015857692699037063119282795585513329278"
        "3433840935197801553124659726357957462276646527282722005637400648549997709659947045402082816622623785"
        "7393450736339007967761930577506740176324673600968951340535537458516661134223766678604162159680461914"
        "4672918403005300575308490487653917113865916462395249126236538818796362393732804238910186723484976682"
        "3508986338858792562830275599565752445550725518931369083625477918694866799496832404970582102851318545"
        "1396213837722826145437693412532098591327667236328125";
    const auto &format = Format(64);
    const auto halfway = ParseFloatEncoding(digits + "e-1075", format);
    REQUIRE(halfway.has_value());
    CHECK(halfway->Bits() == FloatEncoding::Zero(format).Bits());
    const auto negative = ParseFloatEncoding("-" + digits + "e-1075", format);
    REQUIRE(negative.has_value());
    CHECK(negative->Bits() == FloatEncoding::Zero(format, true).Bits());
    digits.back() = '4';
    const auto below = ParseFloatEncoding(digits + "e-1075", format);
    REQUIRE(below.has_value());
    CHECK(below->Bits() == FloatEncoding::Zero(format).Bits());
    digits.back() = '6';
    const auto above = ParseFloatEncoding(digits + "e-1075", format);
    REQUIRE(above.has_value());
    CHECK(above->Bits() == FloatEncoding::MinPositiveSubnormal(format).Bits());
}
