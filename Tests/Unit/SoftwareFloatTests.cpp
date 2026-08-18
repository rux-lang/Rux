#include "Numeric/SoftwareFloat.h"

#include <array>
#include <cstdint>
#include <doctest.h>
#include <limits>

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

[[nodiscard]] std::int32_t Binary8Units(const std::uint8_t raw) {
    const std::uint32_t exponent = (raw >> 3) & 0xF;
    const std::uint32_t fraction = raw & 0x7;
    const std::int32_t magnitude = exponent == 0 ? static_cast<std::int32_t>(fraction)
                                                 : static_cast<std::int32_t>((8 + fraction) << (exponent - 1));
    return (raw & 0x80) != 0 ? -magnitude : magnitude;
}

[[nodiscard]] std::uint8_t ReferenceBinary8Add(const std::uint8_t left, const std::uint8_t right) {
    const std::int32_t sum = Binary8Units(left) + Binary8Units(right);
    if (sum == 0) {
        return Binary8Units(left) == 0 && Binary8Units(right) == 0 && (left & right & 0x80) != 0 ? 0x80 : 0;
    }

    const std::uint8_t sign = sum < 0 ? 0x80 : 0;
    const std::int32_t magnitude = sum < 0 ? -sum : sum;
    std::uint8_t best = 0;
    std::int32_t bestDistance = std::numeric_limits<std::int32_t>::max();
    // Treat the infinity encoding as the next even significand after the maximum finite value. This gives the IEEE
    // overflow threshold while keeping the oracle entirely in integer arithmetic.
    for (std::uint8_t candidate = 0; candidate <= 0x78; ++candidate) {
        if (((candidate >> 3) & 0xF) == 0xF && candidate != 0x78) {
            continue;
        }
        const std::int32_t distance = magnitude > Binary8Units(candidate) ? magnitude - Binary8Units(candidate)
                                                                          : Binary8Units(candidate) - magnitude;
        if (distance < bestDistance || (distance == bestDistance && (candidate & 1) == 0 && (best & 1) != 0)) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return sign | best;
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

TEST_CASE("software float addition and subtraction handle finite rounding") {
    const FloatFormat &format = Format(8);
    const auto encoding = [&](const std::uint8_t raw) {
        return FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 8));
    };
    const auto add = [&](const std::uint8_t left, const std::uint8_t right) {
        return static_cast<std::uint8_t>(Word(AddFloat(encoding(left), encoding(right))));
    };
    const auto subtract = [&](const std::uint8_t left, const std::uint8_t right) {
        return static_cast<std::uint8_t>(Word(SubtractFloat(encoding(left), encoding(right))));
    };

    CHECK_EQ(add(0x38, 0x38), 0x40);
    CHECK_EQ(add(0x3C, 0x3C), 0x44);
    CHECK_EQ(add(0x01, 0x01), 0x02);
    CHECK_EQ(add(0x38, 0x18), 0x38);
    CHECK_EQ(add(0x39, 0x18), 0x3A);
    CHECK_EQ(add(0x38, 0x1A), 0x39);
    CHECK_EQ(add(0x77, 0x77), 0x78);
    CHECK_EQ(add(0x38, 0xB8), 0x00);
    CHECK_EQ(subtract(0x40, 0x38), 0x38);
    CHECK_EQ(subtract(0x38, 0x40), 0xB8);
    CHECK_EQ(subtract(0x3C, 0x3C), 0x00);
}

TEST_CASE("software float addition applies IEEE special-value rules") {
    const FloatFormat &format = Format(32);
    const auto encoding = [&](const std::uint32_t raw) {
        return FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 32));
    };

    CHECK_EQ(Word(AddFloat(encoding(0x80000000), encoding(0x80000000))), 0x80000000);
    CHECK_EQ(Word(AddFloat(encoding(0x00000000), encoding(0x80000000))), 0x00000000);
    CHECK_EQ(Word(SubtractFloat(encoding(0x80000000), encoding(0x00000000))), 0x80000000);
    CHECK_EQ(Word(AddFloat(encoding(0x7F800000), encoding(0x3F800000))), 0x7F800000);
    CHECK_EQ(Word(SubtractFloat(encoding(0x3F800000), encoding(0x7F800000))), 0xFF800000);
    CHECK_EQ(AddFloat(encoding(0x7F800000), encoding(0xFF800000)).Classify(), FloatClass::QuietNaN);
    CHECK_EQ(SubtractFloat(encoding(0x7F800000), encoding(0x7F800000)).Classify(), FloatClass::QuietNaN);
    CHECK_EQ(Word(AddFloat(encoding(0x7FC12345), encoding(0x3F800000))), 0x7FC12345);
    CHECK_EQ(Word(AddFloat(encoding(0x7FA12345), encoding(0x7FC54321))), 0x7FE12345);
}

TEST_CASE("software float add and subtract match an exact binary8 oracle") {
    const FloatFormat &format = Format(8);
    for (std::uint32_t leftRaw = 0; leftRaw <= 0xFF; ++leftRaw) {
        const FloatEncoding left = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(leftRaw, 8));
        if (!UnpackFloat(left).IsFinite()) {
            continue;
        }
        for (std::uint32_t rightRaw = 0; rightRaw <= 0xFF; ++rightRaw) {
            const FloatEncoding right = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(rightRaw, 8));
            if (!UnpackFloat(right).IsFinite()) {
                continue;
            }
            CAPTURE(leftRaw);
            CAPTURE(rightRaw);
            CHECK_EQ(Word(AddFloat(left, right)),
                     ReferenceBinary8Add(static_cast<std::uint8_t>(leftRaw), static_cast<std::uint8_t>(rightRaw)));
            CHECK_EQ(Word(SubtractFloat(left, right)),
                     ReferenceBinary8Add(static_cast<std::uint8_t>(leftRaw),
                                         static_cast<std::uint8_t>(rightRaw) ^ std::uint8_t{0x80}));
        }
    }
}
