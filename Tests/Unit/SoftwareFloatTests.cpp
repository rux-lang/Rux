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

[[nodiscard]] std::uint8_t RoundBinary8(const std::int64_t numerator, const std::int64_t scale,
                                        const bool negativeZero = false) {
    if (numerator == 0) {
        return negativeZero ? 0x80 : 0;
    }
    const std::uint8_t sign = numerator < 0 ? 0x80 : 0;
    const std::int64_t magnitude = numerator < 0 ? -numerator : numerator;
    std::uint8_t best = 0;
    std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();
    for (std::uint8_t candidate = 0; candidate <= 0x78; ++candidate) {
        const std::int64_t candidateValue = static_cast<std::int64_t>(Binary8Units(candidate)) * scale;
        const std::int64_t distance =
            magnitude > candidateValue ? magnitude - candidateValue : candidateValue - magnitude;
        if (distance < bestDistance || (distance == bestDistance && (candidate & 1) == 0 && (best & 1) != 0)) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return sign | best;
}

[[nodiscard]] std::uint8_t ReferenceBinary8Multiply(const std::uint8_t left, const std::uint8_t right) {
    const bool negativeZero = Binary8Units(left) == 0 && Binary8Units(right) != 0
                                ? ((left ^ right) & 0x80) != 0
                                : Binary8Units(right) == 0 && ((left ^ right) & 0x80) != 0;
    return RoundBinary8(static_cast<std::int64_t>(Binary8Units(left)) * Binary8Units(right), 512, negativeZero);
}

[[nodiscard]] std::uint8_t ReferenceBinary8Fma(const std::uint8_t left, const std::uint8_t right,
                                               const std::uint8_t addend) {
    const std::int64_t product = static_cast<std::int64_t>(Binary8Units(left)) * Binary8Units(right);
    const std::int64_t sum = product + static_cast<std::int64_t>(Binary8Units(addend)) * 512;
    const bool productNegative = ((left ^ right) & 0x80) != 0;
    const bool negativeZero = product == 0 && Binary8Units(addend) == 0 && productNegative && (addend & 0x80) != 0;
    return RoundBinary8(sum, 512, negativeZero);
}

[[nodiscard]] FloatEncoding One(const FloatFormat &format) {
    WideInteger bits =
        WideInteger::FromUnsigned(format.exponentBias, format.valueBits).ShiftedLeft(format.SignificandFieldBits());
    if (format.explicitIntegerBit) {
        bits = bits.BitwiseOr(WideInteger::FromUnsigned(1, format.valueBits).ShiftedLeft(format.FractionBits()));
    }
    return FloatEncoding::FromBits(format, bits);
}

[[nodiscard]] FloatEncoding Negated(const FloatEncoding &encoding) {
    const FloatFormat &format = encoding.Format();
    const WideInteger sign = WideInteger::FromUnsigned(1, format.valueBits).ShiftedLeft(format.valueBits - 1);
    return FloatEncoding::FromBits(format, encoding.Bits().BitwiseXor(sign));
}

[[nodiscard]] std::uint8_t ReferenceBinary8Divide(const std::uint8_t left, const std::uint8_t right) {
    const std::int64_t leftValue = Binary8Units(left);
    const std::int64_t rightValue = Binary8Units(right);
    const bool negative = ((left ^ right) & 0x80) != 0;
    if (leftValue == 0) {
        return negative ? 0x80 : 0;
    }

    const std::int64_t numerator = (leftValue < 0 ? -leftValue : leftValue) * 512;
    const std::int64_t denominator = rightValue < 0 ? -rightValue : rightValue;
    std::uint8_t best = 0;
    std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();
    for (std::uint8_t candidate = 0; candidate <= 0x78; ++candidate) {
        const std::int64_t distance = numerator > static_cast<std::int64_t>(Binary8Units(candidate)) * denominator
                                        ? numerator - static_cast<std::int64_t>(Binary8Units(candidate)) * denominator
                                        : static_cast<std::int64_t>(Binary8Units(candidate)) * denominator - numerator;
        if (distance < bestDistance || (distance == bestDistance && (candidate & 1) == 0 && (best & 1) != 0)) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return (negative ? 0x80 : 0) | best;
}

[[nodiscard]] std::uint8_t ReferenceBinary8SquareRoot(const std::uint8_t raw) {
    if (Binary8Units(raw) == 0) {
        return raw;
    }
    const std::int64_t scaledValue = static_cast<std::int64_t>(Binary8Units(raw)) * 512;
    std::uint8_t upper = 1;
    while (static_cast<std::int64_t>(Binary8Units(upper)) * Binary8Units(upper) < scaledValue) {
        ++upper;
    }
    if (static_cast<std::int64_t>(Binary8Units(upper)) * Binary8Units(upper) == scaledValue) {
        return upper;
    }
    const std::uint8_t lower = upper - 1;
    const std::int64_t midpointTwice = Binary8Units(lower) + Binary8Units(upper);
    const std::int64_t comparison = 4 * scaledValue - midpointTwice * midpointTwice;
    if (comparison < 0) {
        return lower;
    }
    if (comparison > 0) {
        return upper;
    }
    return (lower & 1) == 0 ? lower : upper;
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

TEST_CASE("software float multiplication matches an exact binary8 oracle") {
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
            CHECK_EQ(Word(MultiplyFloat(left, right)),
                     ReferenceBinary8Multiply(static_cast<std::uint8_t>(leftRaw), static_cast<std::uint8_t>(rightRaw)));
        }
    }
}

TEST_CASE("software fused multiply-add rounds once against an exact binary8 oracle") {
    const FloatFormat &format = Format(8);
    constexpr std::array<std::uint8_t, 16> addends{0x00, 0x80, 0x01, 0x81, 0x08, 0x88, 0x18, 0x98,
                                                   0x38, 0xB8, 0x55, 0xD5, 0x70, 0xF0, 0x77, 0xF7};
    std::uint32_t fusedDifferences = 0;
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
            for (const std::uint8_t addendRaw : addends) {
                const FloatEncoding addend = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(addendRaw, 8));
                CAPTURE(leftRaw);
                CAPTURE(rightRaw);
                CAPTURE(addendRaw);
                const FloatEncoding fused = FusedMultiplyAddFloat(left, right, addend);
                CHECK_EQ(Word(fused), ReferenceBinary8Fma(static_cast<std::uint8_t>(leftRaw),
                                                          static_cast<std::uint8_t>(rightRaw), addendRaw));
                fusedDifferences += fused.Bits() != AddFloat(MultiplyFloat(left, right), addend).Bits();
            }
        }
    }
    CHECK_GT(fusedDifferences, 0);
}

TEST_CASE("software multiplication applies IEEE special-value rules") {
    const FloatFormat &format = Format(32);
    const auto encoding = [&](const std::uint32_t raw) {
        return FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 32));
    };

    CHECK_EQ(MultiplyFloat(encoding(0x00000000), encoding(0x7F800000)).Classify(), FloatClass::QuietNaN);
    CHECK_EQ(Word(MultiplyFloat(encoding(0xFF800000), encoding(0x40000000))), 0xFF800000);
    CHECK_EQ(Word(MultiplyFloat(encoding(0x80000000), encoding(0xC0000000))), 0x00000000);
    CHECK_EQ(Word(MultiplyFloat(encoding(0x7FA12345), encoding(0x7FC54321))), 0x7FE12345);
    CHECK_EQ(FusedMultiplyAddFloat(encoding(0x7F800000), encoding(0x00000000), encoding(0x3F800000)).Classify(),
             FloatClass::QuietNaN);
    CHECK_EQ(FusedMultiplyAddFloat(encoding(0x7F800000), encoding(0x40000000), encoding(0xFF800000)).Classify(),
             FloatClass::QuietNaN);
    CHECK_EQ(Word(FusedMultiplyAddFloat(encoding(0x7F800000), encoding(0xC0000000), encoding(0x3F800000))), 0xFF800000);
}

TEST_CASE("software multiply and fused multiply-add retain full wide precision") {
    for (const FloatFormat &format : FloatFormats()) {
        CAPTURE(format.name);
        const FloatEncoding one = One(format);
        const FloatEncoding minimum = FloatEncoding::MinPositiveSubnormal(format);
        const FloatEncoding maximum = FloatEncoding::MaxFinite(format);
        CHECK_EQ(MultiplyFloat(one, minimum).Bits(), minimum.Bits());
        CHECK_EQ(FusedMultiplyAddFloat(one, maximum, Negated(maximum)).Bits(), FloatEncoding::Zero(format).Bits());
    }
}

TEST_CASE("software division and remainder match exact binary8 oracles") {
    const FloatFormat &format = Format(8);
    for (std::uint32_t leftRaw = 0; leftRaw <= 0xFF; ++leftRaw) {
        const FloatEncoding left = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(leftRaw, 8));
        if (!UnpackFloat(left).IsFinite()) {
            continue;
        }
        for (std::uint32_t rightRaw = 0; rightRaw <= 0xFF; ++rightRaw) {
            const FloatEncoding right = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(rightRaw, 8));
            if (!UnpackFloat(right).IsFinite() || Binary8Units(static_cast<std::uint8_t>(rightRaw)) == 0) {
                continue;
            }
            CAPTURE(leftRaw);
            CAPTURE(rightRaw);
            CHECK_EQ(Word(DivideFloat(left, right)),
                     ReferenceBinary8Divide(static_cast<std::uint8_t>(leftRaw), static_cast<std::uint8_t>(rightRaw)));
            const std::int32_t remainder =
                Binary8Units(static_cast<std::uint8_t>(leftRaw)) % Binary8Units(static_cast<std::uint8_t>(rightRaw));
            CHECK_EQ(Word(RemainderFloat(left, right)),
                     RoundBinary8(remainder, 1, remainder == 0 && (leftRaw & 0x80) != 0));
        }
    }
}

TEST_CASE("software square root matches an exact binary8 oracle") {
    const FloatFormat &format = Format(8);
    for (std::uint32_t raw = 0; raw <= 0x77; ++raw) {
        CAPTURE(raw);
        const FloatEncoding value = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 8));
        CHECK_EQ(Word(SquareRootFloat(value)), ReferenceBinary8SquareRoot(static_cast<std::uint8_t>(raw)));
    }
    CHECK_EQ(Word(SquareRootFloat(FloatEncoding::Zero(format, true))), 0x80);
}

TEST_CASE("software divide remainder and square root apply IEEE special rules") {
    const FloatFormat &format = Format(32);
    const auto encoding = [&](const std::uint32_t raw) {
        return FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 32));
    };

    CHECK_EQ(DivideFloat(encoding(0), encoding(0)).Classify(), FloatClass::QuietNaN);
    CHECK_EQ(DivideFloat(encoding(0x7F800000), encoding(0x7F800000)).Classify(), FloatClass::QuietNaN);
    CHECK_EQ(Word(DivideFloat(encoding(0xBF800000), encoding(0))), 0xFF800000);
    CHECK_EQ(Word(DivideFloat(encoding(0xBF800000), encoding(0x7F800000))), 0x80000000);
    CHECK_EQ(RemainderFloat(encoding(0x7F800000), encoding(0x3F800000)).Classify(), FloatClass::QuietNaN);
    CHECK_EQ(RemainderFloat(encoding(0x3F800000), encoding(0)).Classify(), FloatClass::QuietNaN);
    CHECK_EQ(Word(RemainderFloat(encoding(0xBF800000), encoding(0x7F800000))), 0xBF800000);
    CHECK_EQ(SquareRootFloat(encoding(0xBF800000)).Classify(), FloatClass::QuietNaN);
    CHECK_EQ(Word(SquareRootFloat(encoding(0x7F800000))), 0x7F800000);
    CHECK_EQ(Word(DivideFloat(encoding(0x7FA12345), encoding(0x3F800000))), 0x7FE12345);
}

TEST_CASE("software divide remainder and square root retain wide precision") {
    for (const FloatFormat &format : FloatFormats()) {
        CAPTURE(format.name);
        const FloatEncoding one = One(format);
        const FloatEncoding minimum = FloatEncoding::MinPositiveSubnormal(format);
        const FloatEncoding maximum = FloatEncoding::MaxFinite(format);
        CHECK_EQ(DivideFloat(minimum, one).Bits(), minimum.Bits());
        CHECK_EQ(RemainderFloat(maximum, one).Bits(), FloatEncoding::Zero(format).Bits());
        CHECK_EQ(SquareRootFloat(one).Bits(), one.Bits());
    }
}

TEST_CASE("software float comparison orders every binary8 pair") {
    const FloatFormat &format = Format(8);
    for (std::uint32_t leftRaw = 0; leftRaw <= 0xFF; ++leftRaw) {
        const FloatEncoding left = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(leftRaw, 8));
        for (std::uint32_t rightRaw = 0; rightRaw <= 0xFF; ++rightRaw) {
            const FloatEncoding right = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(rightRaw, 8));
            CAPTURE(leftRaw);
            CAPTURE(rightRaw);
            FloatComparison expected = FloatComparison::Equal;
            const bool unordered =
                left.Classify() == FloatClass::QuietNaN || left.Classify() == FloatClass::SignalingNaN ||
                right.Classify() == FloatClass::QuietNaN || right.Classify() == FloatClass::SignalingNaN;
            if (unordered) {
                expected = FloatComparison::Unordered;
            }
            else if (Binary8Units(static_cast<std::uint8_t>(leftRaw)) <
                     Binary8Units(static_cast<std::uint8_t>(rightRaw))) {
                expected = FloatComparison::Less;
            }
            else if (Binary8Units(static_cast<std::uint8_t>(leftRaw)) >
                     Binary8Units(static_cast<std::uint8_t>(rightRaw))) {
                expected = FloatComparison::Greater;
            }
            CHECK_EQ(CompareFloat(left, right), expected);
        }
    }
}

TEST_CASE("software integer to float conversion matches the exact binary8 oracle") {
    const FloatFormat &format = Format(8);
    for (std::int32_t integer = -32768; integer <= 32767; ++integer) {
        CAPTURE(integer);
        const WideInteger bits = WideInteger::FromUnsigned(static_cast<std::uint16_t>(integer), 16);
        CHECK_EQ(Word(IntegerToFloat(bits, true, format)), RoundBinary8(static_cast<std::int64_t>(integer) * 512, 1));
    }
    for (std::uint32_t integer = 0; integer <= 65535; ++integer) {
        CAPTURE(integer);
        CHECK_EQ(Word(IntegerToFloat(WideInteger::FromUnsigned(integer, 16), false, format)),
                 RoundBinary8(static_cast<std::int64_t>(integer) * 512, 1));
    }
}

TEST_CASE("software float to integer conversion truncates and reports invalid values") {
    const FloatFormat &format = Format(8);
    for (std::uint32_t raw = 0; raw <= 0xFF; ++raw) {
        const FloatEncoding encoding = FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 8));
        if (!UnpackFloat(encoding).IsFinite()) {
            continue;
        }
        CAPTURE(raw);
        const FloatToIntegerResult converted = FloatToInteger(encoding, 16, true);
        REQUIRE(converted.HasValue());
        const std::int32_t expected = Binary8Units(static_cast<std::uint8_t>(raw)) / 512;
        CHECK_EQ(converted.value, WideInteger::FromUnsigned(static_cast<std::uint16_t>(expected), 16));
    }

    const auto encoding = [&](const std::uint8_t raw) {
        return FloatEncoding::FromBits(format, WideInteger::FromUnsigned(raw, 8));
    };
    CHECK_EQ(FloatToInteger(encoding(0x78), 32, true).error, FloatConversionError::NonFinite);
    CHECK_EQ(FloatToInteger(encoding(0x7C), 32, true).error, FloatConversionError::NonFinite);
    CHECK_EQ(FloatToInteger(encoding(0x77), 8, true).error, FloatConversionError::OutOfRange);
    CHECK_EQ(FloatToInteger(encoding(0xF0), 8, true).value, WideInteger::FromUnsigned(0x80, 8));
    CHECK(FloatToInteger(encoding(0xB0), 8, false).HasValue());
    CHECK_EQ(FloatToInteger(encoding(0xB8), 8, false).error, FloatConversionError::OutOfRange);
}

TEST_CASE("software cross-float conversion preserves binary8 values and wide identities") {
    const FloatFormat &binary8 = Format(8);
    const FloatFormat &binary16 = Format(16);
    for (std::uint32_t raw = 0; raw <= 0xFF; ++raw) {
        CAPTURE(raw);
        const FloatEncoding source = FloatEncoding::FromBits(binary8, WideInteger::FromUnsigned(raw, 8));
        CHECK_EQ(ConvertFloat(ConvertFloat(source, binary16), binary8).Bits(), source.Bits());
    }

    for (const FloatFormat &format : FloatFormats()) {
        CAPTURE(format.name);
        const FloatEncoding one = One(format);
        CHECK_EQ(CompareFloat(one, One(binary8)), FloatComparison::Equal);
        CHECK_EQ(ConvertFloat(one, binary8).Bits(), One(binary8).Bits());
    }

    const WideInteger maximum = WideInteger::AllOnes(512);
    CHECK(IntegerToFloat(maximum, false, Format(512)).Classify() == FloatClass::Normal);
    CHECK_EQ(FloatToInteger(IntegerToFloat(maximum, false, Format(512)), 512, false).error,
             FloatConversionError::OutOfRange);
}
