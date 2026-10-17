// Synthesized software-float divide and remainder helpers, run against SoftwareFloat.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Lowering/HirToLir/HirToLirSoftFloat.h"
#include "Lowering/HirToLir/SoftFloatBuilder.h"
#include "Numeric/SoftwareFloat.h"
#include "SoftFloatLoweringTestSupport.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::HirToLirDetail;
using Rux::Testing::LirInterpreter;
using Rux::Testing::LirRunResult;

namespace {
using K = TypeRef::Kind;
using Pair = std::pair<WideInteger, WideInteger>;

/// float32 and float64 too: AArch64 computes their remainder with the helper (`Target::HasNativeFloatRemainder`).
constexpr std::array<K, 8> FloatKinds{K::Float8,  K::Float16,  K::Float32,  K::Float64,
                                      K::Float80, K::Float128, K::Float256, K::Float512};
/// Random pairs per format and operation.
constexpr int RandomPairs = 2000;

const FloatFormat &FormatOf(const K kind) {
    return *FindFloatFormat(FindPrimitive(kind)->bits);
}

std::string Hex(const WideInteger &value) {
    std::string text;
    for (std::size_t word = (value.Width() + 63) / 64; word-- > 0;) {
        text += std::format("{:016x}", value.Word64(word));
    }
    return text;
}

LirPackage PackageOf(std::vector<LirFunc> functions) {
    LirPackage package;
    LirModule &module = package.modules.emplace_back();
    module.name = "helpers";
    module.funcs = std::move(functions);
    return package;
}

/// Counts mismatches and keeps the first, so a broken helper reports once rather than once per input.
struct Mismatches {
    int count = 0;
    std::string first;

    void Check(const LirRunResult &result, const WideInteger &expected, const std::string &input) {
        if (result.Ok() && result.value && Hex(*result.value) == Hex(expected)) {
            return;
        }
        if (count++ == 0) {
            first = std::format("{}: got {}, expected {} {}", input, result.value ? Hex(*result.value) : "nothing",
                                Hex(expected), result.Describe());
        }
    }
};

WideInteger RandomBits(std::mt19937_64 &random, const std::uint32_t width) {
    std::array<std::uint64_t, 8> words{};
    for (std::uint64_t &word : words) {
        word = random();
    }
    return WideInteger::FromWords(words, width);
}

/// Signed zeros, the subnormal and normal extremes, ones, the largest finites, infinities and NaNs with payloads,
/// plus float80's pseudo-denormals, unnormals, pseudo-infinities and pseudo-NaNs.
std::vector<WideInteger> SpecialValues(const FloatFormat &format) {
    const std::uint32_t width = format.valueBits;
    const auto one = [&](const std::uint32_t bit) { return WideInteger::FromUnsigned(1, width).ShiftedLeft(bit); };
    const auto exponent = [&](const std::uint32_t field) {
        return WideInteger::FromUnsigned(field, width).ShiftedLeft(format.SignificandFieldBits());
    };
    const WideInteger integer = format.explicitIntegerBit ? one(format.FractionBits()) : WideInteger::Zero(width);
    const WideInteger maximum = exponent(format.MaxExponentField());
    const WideInteger quiet = one(format.FractionBits() - 1);
    const WideInteger payload = WideInteger::FromUnsigned(0xA5A5A5A5A5A5A5A5ULL, 64)
                                    .Truncated(format.FractionBits() - 1)
                                    .Extended(width, false);
    const WideInteger unit = exponent(static_cast<std::uint32_t>(format.exponentBias)).BitwiseOr(integer);
    std::vector<WideInteger> values{
        WideInteger::Zero(width),
        FloatEncoding::MinPositiveSubnormal(format).Bits(),
        FloatEncoding::MinPositiveNormal(format).Bits().Subtracted(WideInteger::FromUnsigned(1, width)),
        FloatEncoding::MinPositiveNormal(format).Bits(),
        unit,
        unit.BitwiseOr(one(0)),
        exponent(static_cast<std::uint32_t>(format.exponentBias) + 1).BitwiseOr(integer).BitwiseOr(quiet),
        FloatEncoding::MaxFinite(format).Bits(),
        FloatEncoding::Infinity(format).Bits(),
        FloatEncoding::QuietNaN(format).Bits(),
        FloatEncoding::SignalingNaN(format).Bits(),
        maximum.BitwiseOr(integer).BitwiseOr(quiet).BitwiseOr(payload),
        maximum.BitwiseOr(integer).BitwiseOr(payload),
    };
    if (format.explicitIntegerBit) {
        values.push_back(integer);                                     // the smallest pseudo-denormal
        values.push_back(integer.BitwiseOr(payload));                  // a pseudo-denormal
        values.push_back(unit.BitwiseXor(integer));                    // an unnormal
        values.push_back(maximum);                                     // a pseudo-infinity
        values.push_back(maximum.BitwiseOr(quiet).BitwiseOr(payload)); // a pseudo-NaN
    }
    const std::size_t positive = values.size();
    for (std::size_t index = 0; index < positive; ++index) {
        values.push_back(values[index].BitwiseOr(one(width - 1)));
    }
    return values;
}

/// Builds finite encodings of one format from a sign, a biased exponent field and a fraction.
class Values {
public:
    Values(const FloatFormat &inputFormat, const std::uint64_t seed)
        : format(&inputFormat)
        , random(seed) {
    }

    [[nodiscard]] std::uint64_t Next() {
        return random();
    }

    [[nodiscard]] WideInteger Bits() {
        return RandomBits(random, format->valueBits);
    }

    /// A random fraction whose low `cleared` bits are zero.
    [[nodiscard]] WideInteger Fraction(const std::uint32_t cleared = 0) {
        const std::uint32_t drop = format->valueBits - format->FractionBits();
        return Bits().ShiftedLeft(drop).ShiftedRight(drop, false).ShiftedRight(cleared, false).ShiftedLeft(cleared);
    }

    /// The encoding with `field` clamped to the finite range; float80's integer bit follows the field.
    [[nodiscard]] WideInteger Encode(const bool negative, std::int64_t field, const WideInteger &fraction) const {
        const std::uint32_t width = format->valueBits;
        field = std::clamp<std::int64_t>(field, 0, static_cast<std::int64_t>(format->MaxExponentField()) - 1);
        WideInteger bits = WideInteger::FromUnsigned(static_cast<std::uint64_t>(field), width)
                               .ShiftedLeft(format->SignificandFieldBits())
                               .BitwiseOr(fraction);
        if (format->explicitIntegerBit && field != 0) {
            bits = bits.BitwiseOr(WideInteger::FromUnsigned(1, width).ShiftedLeft(format->FractionBits()));
        }
        return negative ? bits.BitwiseOr(WideInteger::FromUnsigned(1, width).ShiftedLeft(width - 1)) : bits;
    }

    /// A random finite value with a field of `field` and a random sign and fraction.
    [[nodiscard]] WideInteger Finite(const std::int64_t field, const std::uint32_t cleared = 0) {
        const bool negative = random() % 2 != 0;
        return Encode(negative, field, Fraction(cleared));
    }

    /// A field drawn from [low, high].
    [[nodiscard]] std::int64_t Field(const std::int64_t low, const std::int64_t high) {
        return low + static_cast<std::int64_t>(random() % static_cast<std::uint64_t>(high - low + 1));
    }

    [[nodiscard]] std::int64_t Bias() const {
        return format->exponentBias;
    }

    [[nodiscard]] std::int64_t Top() const {
        return static_cast<std::int64_t>(format->MaxExponentField()) - 1;
    }

private:
    const FloatFormat *format;
    std::mt19937_64 random;
};

/// The divide and remainder helpers of one format, synthesized through a standalone registry.
struct Helpers {
    LirPackage package;
    std::string divide;
    std::string remainder;
};

Helpers Synthesize(const K kind) {
    SoftFloatHelperRegistry registry;
    Helpers helpers;
    helpers.divide = registry.Request({SoftFloatOperation::Divide, kind, K::Unknown});
    helpers.remainder = registry.Request({SoftFloatOperation::Remainder, kind, K::Unknown});
    std::vector<Diagnostic> diagnostics;
    helpers.package = PackageOf(registry.SynthesizePending(diagnostics));
    REQUIRE(diagnostics.empty());
    return helpers;
}

/// Runs every pair through the helper and its reference, reporting the first mismatch and the most steps one run took.
std::uint64_t CompareAll(const FloatFormat &format, const LirInterpreter &interpreter, const std::string &symbol,
                         const std::vector<Pair> &pairs,
                         FloatEncoding (*reference)(const FloatEncoding &, const FloatEncoding &)) {
    Mismatches mismatches;
    std::uint64_t steps = 0;
    for (const auto &[left, right] : pairs) {
        const LirRunResult result = interpreter.Run(symbol, std::vector{left, right});
        steps = std::max(steps, result.steps);
        const FloatEncoding expected =
            reference(FloatEncoding::FromBits(format, left), FloatEncoding::FromBits(format, right));
        mismatches.Check(result, expected.Bits(), std::format("{} {} {}", symbol, Hex(left), Hex(right)));
    }
    CHECK_MESSAGE(mismatches.count == 0, std::format("{} mismatches, first {}", mismatches.count, mismatches.first));
    return steps;
}
} // namespace

TEST_CASE("synthesized divide and remainder helpers match on every pair of special values") {
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        const Helpers helpers = Synthesize(kind);
        const LirInterpreter interpreter(helpers.package);
        std::vector<Pair> pairs;
        const std::vector<WideInteger> specials = SpecialValues(format);
        for (const WideInteger &left : specials) {
            for (const WideInteger &right : specials) {
                pairs.emplace_back(left, right);
            }
        }
        static_cast<void>(CompareAll(format, interpreter, helpers.divide, pairs, &DivideFloat));
        static_cast<void>(CompareAll(format, interpreter, helpers.remainder, pairs, &RemainderFloat));
    }
}

TEST_CASE("synthesized divide helpers match DivideFloat on random, exact, tied, subnormal and overflowing quotients") {
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        const Helpers helpers = Synthesize(kind);
        const LirInterpreter interpreter(helpers.package);
        Values values(format, 0xD1F0000U + format.valueBits);
        const std::int64_t bias = values.Bias();
        const std::int64_t top = values.Top();
        const std::int64_t span = format.precisionBits + 8;
        std::vector<Pair> pairs;
        for (int sample = 0; sample < RandomPairs; ++sample) {
            switch (sample % 4) {
            case 0: // any two encodings
                pairs.emplace_back(values.Bits(), values.Bits());
                break;
            case 1: // a quotient in the normal range
                pairs.emplace_back(values.Finite(values.Field(1, top)), values.Finite(values.Field(1, top)));
                break;
            case 2: // a quotient that underflows to a subnormal or zero
                pairs.emplace_back(values.Finite(values.Field(0, span)),
                                   values.Finite(values.Field(bias, bias + span)));
                break;
            default: // a quotient that overflows, or nearly does
                pairs.emplace_back(values.Finite(values.Field(top - span, top)),
                                   values.Finite(values.Field(bias - span, bias)));
                break;
            }
        }
        // Exact quotients: a short significand times a small integer divides back without rounding.
        const std::uint32_t half = std::max<std::uint32_t>(1, format.precisionBits / 2 - 1);
        for (int sample = 0; sample < 200; ++sample) {
            const WideInteger divisor =
                values.Finite(values.Field(bias - span, bias + span), format.FractionBits() / 2);
            const WideInteger integer =
                WideInteger::FromUnsigned(1 + values.Next() % ((std::uint64_t{1} << half) - 1), format.valueBits);
            const FloatEncoding factor = IntegerToFloat(integer, false, format);
            const WideInteger product = MultiplyFloat(FloatEncoding::FromBits(format, divisor), factor).Bits();
            pairs.emplace_back(product, divisor);
            pairs.emplace_back(product, factor.Bits());
        }
        // Ties: halving a subnormal or the smallest normals with an odd last bit lands exactly between two values.
        for (int sample = 0; sample < 200; ++sample) {
            const WideInteger dividend =
                values.Encode(values.Next() % 2 != 0, values.Field(0, 2),
                              values.Fraction().BitwiseOr(WideInteger::FromUnsigned(1, format.valueBits)));
            const WideInteger power = values.Encode(values.Next() % 2 != 0, values.Field(bias + 1, bias + 3),
                                                    WideInteger::Zero(format.valueBits));
            pairs.emplace_back(dividend, power);
        }
        static_cast<void>(CompareAll(format, interpreter, helpers.divide, pairs, &DivideFloat));
    }
}

TEST_CASE("synthesized remainder helpers match RemainderFloat on random pairs, exact multiples and wide gaps") {
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        const Helpers helpers = Synthesize(kind);
        const LirInterpreter interpreter(helpers.package);
        Values values(format, 0x4E30000U + format.valueBits);
        const std::int64_t top = values.Top();
        const std::int64_t span = format.precisionBits + 8;
        // A gap of hundreds of thousands of bits costs float256 and float512 tens of milliseconds interpreted, so they
        // draw the two cases with unbounded gaps an eighth as often, and fewer of the longest gaps below.
        const bool wide = format.valueBits >= 256;
        std::vector<Pair> pairs;
        for (int sample = 0; sample < RandomPairs; ++sample) {
            const std::int64_t field = values.Field(0, top);
            const bool longGap = sample % 4 < 2;
            switch (sample % 4 + (wide && longGap && sample / 4 % 8 != 0 ? 2 : 0)) {
            case 0: // any two encodings
                pairs.emplace_back(values.Bits(), values.Bits());
                break;
            case 1: // any gap
                pairs.emplace_back(values.Finite(field), values.Finite(field - values.Field(0, top)));
                break;
            case 2: // a gap of at most a few significands
                pairs.emplace_back(values.Finite(field), values.Finite(field - values.Field(0, span)));
                break;
            default: // equal or nearly equal exponents, either side larger
                pairs.emplace_back(values.Finite(field), values.Finite(field + values.Field(-2, 2)));
                break;
            }
        }
        // Exact multiples give a signed zero.
        for (int sample = 0; sample < 100; ++sample) {
            const WideInteger divisor = values.Finite(values.Field(1, top / 2), format.FractionBits() / 2);
            const WideInteger integer = WideInteger::FromUnsigned(1 + values.Next() % 255, format.valueBits);
            pairs.emplace_back(
                MultiplyFloat(FloatEncoding::FromBits(format, divisor), IntegerToFloat(integer, false, format)).Bits(),
                divisor);
        }
        // Gaps around the helper's seams: one shift and `Mod` below `width - precisionBits`, square-and-multiply from
        // there, with no squaring while the gap stays below the working width and one more per bit above it.
        const std::int64_t width = SoftFloatWorkingBits(format);
        const std::int64_t chunk = width - format.precisionBits;
        for (const std::int64_t seam : {chunk, width, 2 * width, 4 * width}) {
            for (std::int64_t gap = seam - 2; gap <= seam + 2; ++gap) {
                for (int sample = 0; sample < 4 && gap + 1 <= top; ++sample) {
                    const std::int64_t field = values.Field(gap + 1, top);
                    pairs.emplace_back(values.Finite(field), values.Finite(field - gap));
                }
            }
        }
        // From the top of the range to a subnormal divisor, the longest gaps there are.
        for (int sample = 0; sample < (wide ? 10 : 40); ++sample) {
            pairs.emplace_back(values.Finite(values.Field(top - 2, top)), values.Finite(values.Field(0, 1)));
        }
        const std::uint64_t steps = CompareAll(format, interpreter, helpers.remainder, pairs, &RemainderFloat);
        MESSAGE(std::format("float{} remainder: at most {} interpreted steps per call", format.valueBits, steps));
    }
}

TEST_CASE("the remainder helpers reduce the longest gap with one modular squaring per bit") {
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        const Helpers helpers = Synthesize(kind);
        const LirInterpreter interpreter(helpers.package);
        Values values(format, 0xB16);
        // The largest finite against an odd subnormal: about 8.4 million bits for float512. A chunk-at-a-time
        // reduction would take one wide `Mod` per `width - precisionBits` bits; square-and-multiply takes one
        // multiplication of at most `precisionBits` steps per gap bit beyond the working width, and one more.
        const WideInteger left = values.Encode(false, values.Top(), values.Fraction());
        const WideInteger right =
            values.Encode(true, 0, values.Fraction().BitwiseOr(WideInteger::FromUnsigned(3, format.valueBits)));
        const LirRunResult result = interpreter.Run(helpers.remainder, std::vector{left, right});
        REQUIRE_MESSAGE(result.Ok(), result.Describe());
        CHECK_EQ(
            Hex(*result.value),
            Hex(RemainderFloat(FloatEncoding::FromBits(format, left), FloatEncoding::FromBits(format, right)).Bits()));
        const std::uint64_t gap = 2 * static_cast<std::uint64_t>(format.exponentBias) + format.precisionBits;
        const auto window = static_cast<std::uint64_t>(std::bit_width(SoftFloatWorkingBits(format)) - 1);
        const std::uint64_t squarings = std::max(static_cast<std::uint64_t>(std::bit_width(gap)), window) - window;
        const std::uint64_t bound = 2000 + (squarings + 1) * std::uint64_t{format.precisionBits} * 40;
        MESSAGE(std::format("float{} remainder across the full gap: {} steps, bound {}", format.valueBits, result.steps,
                            bound));
        CHECK_LT(result.steps, bound);
    }
}

TEST_CASE("both back ends compile the synthesized divide and remainder helpers") {
    SoftFloatHelperRegistry registry;
    for (const K kind : FloatKinds) {
        static_cast<void>(registry.Request({SoftFloatOperation::Divide, kind, K::Unknown}));
        static_cast<void>(registry.Request({SoftFloatOperation::Remainder, kind, K::Unknown}));
    }
    std::vector<Diagnostic> diagnostics;
    const LirPackage package = PackageOf(registry.SynthesizePending(diagnostics));
    REQUIRE(diagnostics.empty());
    for (const Target::OS os : {Target::OS::Windows, Target::OS::Linux}) {
        CAPTURE(static_cast<int>(os));
        const RcuEmitter x86(package, "SoftFloatDivide", os);
        CHECK_EQ(x86.Generate().size(), 1);
        CHECK(x86.Diagnostics().empty());
        const AArch64RcuEmitter arm(package, "SoftFloatDivide", os);
        CHECK_EQ(arm.Generate().size(), 1);
        CHECK(arm.Diagnostics().empty());
    }
}
