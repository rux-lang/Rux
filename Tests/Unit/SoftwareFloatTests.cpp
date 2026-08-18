#include "Numeric/SoftwareFloat.h"

#include <array>
#include <cstdint>
#include <doctest.h>

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

[[nodiscard]] UnpackedFloat WorkingFloat(const FloatFormat &format, const std::uint64_t significand,
                                         const std::int64_t exponent) {
    return UnpackedFloat{
        .format = &format,
        .classification = FloatClass::Normal,
        .exponent = exponent,
        .significand = WideInteger::FromUnsigned(significand, WideInteger::MaxBits),
    };
}
} // namespace

TEST_CASE("software float unpacking and packing preserves every binary8 encoding") {
    const FloatFormat &format = Format(8);
    for (std::uint32_t raw = 0; raw <= 0xFF; ++raw) {
        CAPTURE(raw);
        const FloatEncoding encoding = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 8));
        CHECK_EQ(Word(PackFloat(UnpackFloat(encoding))), raw);
    }
}

TEST_CASE("software float unpacking normalizes finite values") {
    const UnpackedFloat oneAndAHalf =
        UnpackFloat(FloatEncoding::FromBits(Format(8), WideInteger::FromUnsigned(0x3C, 8)));
    CHECK_EQ(oneAndAHalf.classification, FloatClass::Normal);
    CHECK_EQ(oneAndAHalf.exponent, 0);
    CHECK_EQ(oneAndAHalf.significand.ToUnsigned(), 96);

    const UnpackedFloat minimumSubnormal = UnpackFloat(FloatEncoding::MinPositiveSubnormal(Format(8)));
    CHECK_EQ(minimumSubnormal.classification, FloatClass::Normal);
    CHECK_EQ(minimumSubnormal.exponent, -9);
    CHECK_EQ(minimumSubnormal.significand.ToUnsigned(), 64);
}

TEST_CASE("sticky shifts retain whether any discarded bit was set") {
    CHECK_EQ(ShiftRightSticky(WideInteger::FromUnsigned(64, 512), 4).ToUnsigned(), 4);
    CHECK_EQ(ShiftRightSticky(WideInteger::FromUnsigned(66, 512), 4).ToUnsigned(), 5);
    CHECK_EQ(ShiftRightSticky(WideInteger::FromUnsigned(1, 512), 512).ToUnsigned(), 1);
    CHECK(ShiftRightSticky(WideInteger::Zero(512), 512).IsZero());
}

TEST_CASE("software float packing rounds nearest with ties to even") {
    const FloatFormat &format = Format(8);
    CHECK_EQ(Word(PackFloat(WorkingFloat(format, 68, 0))), 0x38);
    CHECK_EQ(Word(PackFloat(WorkingFloat(format, 76, 0))), 0x3A);
    CHECK_EQ(Word(PackFloat(WorkingFloat(format, 69, 0))), 0x39);

    CHECK_EQ(Word(PackFloat(WorkingFloat(format, 64, -9))), 0x01);
    CHECK_EQ(Word(PackFloat(WorkingFloat(format, 64, -10))), 0x00);
    CHECK_EQ(Word(PackFloat(WorkingFloat(format, 64, -7))), 0x04);

    UnpackedFloat overflow = UnpackFloat(FloatEncoding::MaxFinite(format));
    overflow.significand = overflow.significand.BitwiseOr(WideInteger::FromUnsigned(7, 512));
    CHECK_EQ(Word(PackFloat(overflow)), 0x78);
}

TEST_CASE("software float packing preserves special values and NaN payloads") {
    const FloatFormat &format = Format(32);
    constexpr std::array rawValues{0x00000000U, 0x80000000U, 0x7F800000U, 0xFF800000U, 0x7FC12345U, 0xFFA12345U};
    for (const std::uint32_t raw : rawValues) {
        CAPTURE(raw);
        const FloatEncoding encoding = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 32));
        CHECK_EQ(Word(PackFloat(UnpackFloat(encoding))), raw);
    }
}

TEST_CASE("software float packing supports every format and canonicalizes extended80") {
    for (const FloatFormat &format : FloatFormats()) {
        CAPTURE(format.name);
        for (const FloatEncoding &encoding :
             {FloatEncoding::Zero(format, true), FloatEncoding::Infinity(format), FloatEncoding::QuietNaN(format, true),
              FloatEncoding::MinPositiveNormal(format), FloatEncoding::MinPositiveSubnormal(format),
              FloatEncoding::MaxFinite(format, true)}) {
            CHECK_EQ(PackFloat(UnpackFloat(encoding)).Bits(), encoding.Bits());
        }
    }

    const FloatFormat &format = Format(80);
    const FloatEncoding pseudoDenormal =
        FloatEncoding::FromBits(format, WideInteger::FromUnsigned(0x8000000000000000ULL, 80));
    const FloatEncoding canonical = PackFloat(UnpackFloat(pseudoDenormal));
    CHECK(canonical.IsCanonical());
    CHECK_EQ(canonical.ExponentField(), 1);
    CHECK_EQ(Word(canonical), 0x8000000000000000ULL);
}
