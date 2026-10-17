// Synthesized software-float multiply helpers, run against SoftwareFloat.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Lowering/HirToLir/HirToLirSoftFloat.h"
#include "Lowering/HirToLir/SoftFloatBuilder.h"
#include "Numeric/SoftwareFloat.h"
#include "SoftFloatLoweringTestSupport.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
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
using Pairs = std::vector<std::pair<WideInteger, WideInteger>>;

constexpr std::array<K, 6> SoftwareKinds{K::Float8, K::Float16, K::Float80, K::Float128, K::Float256, K::Float512};
constexpr int RandomPairs = 2000;
/// Pairs per format for each targeted family: exponent sums at either end of the range, and ties.
constexpr int TargetedPairs = 400;

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

/// The encoding of `significand * 2^(exponent - (p - 1))` for a p-bit integer `significand` with its top bit set and
/// an unbiased `exponent` in the normal range, with a random sign.
WideInteger Encode(std::mt19937_64 &random, const FloatFormat &format, const std::int64_t exponent,
                   const WideInteger &significand) {
    const std::uint32_t width = format.valueBits;
    const auto biased = static_cast<std::uint64_t>(exponent + format.exponentBias);
    WideInteger bits = WideInteger::FromUnsigned(biased, width)
                           .ShiftedLeft(format.SignificandFieldBits())
                           .BitwiseOr(significand.Truncated(format.SignificandFieldBits()).Extended(width, false));
    if (random() % 2 != 0) {
        bits = bits.BitwiseOr(WideInteger::FromUnsigned(1, width).ShiftedLeft(width - 1));
    }
    return bits;
}

/// A random p-bit significand with its top bit set.
WideInteger RandomSignificand(std::mt19937_64 &random, const FloatFormat &format) {
    const std::uint32_t precision = format.precisionBits;
    return RandomBits(random, precision).BitwiseOr(WideInteger::FromUnsigned(1, precision).ShiftedLeft(precision - 1));
}

/// Two normal exponents summing to `sum`, the left one random among those that allow it.
std::pair<std::int64_t, std::int64_t> ExponentsSummingTo(std::mt19937_64 &random, const FloatFormat &format,
                                                         const std::int64_t sum) {
    const std::int64_t minimum = 1 - static_cast<std::int64_t>(format.exponentBias);
    const std::int64_t maximum = static_cast<std::int64_t>(format.MaxExponentField()) - 1 - format.exponentBias;
    const std::int64_t low = std::max(minimum, sum - maximum);
    const std::int64_t high = std::min(maximum, sum - minimum);
    const std::int64_t left = low + static_cast<std::int64_t>(random() % static_cast<std::uint64_t>(high - low + 1));
    return {left, sum - left};
}

/// Finite pairs whose exponents sum to just around the top of the normal range, where the product overflows or
/// rounds up into overflow, and to around and below the bottom, where it is subnormal or underflows to zero.
Pairs ExtremeSums(const FloatFormat &format) {
    const std::int64_t minimum = 1 - static_cast<std::int64_t>(format.exponentBias);
    const std::int64_t maximum = static_cast<std::int64_t>(format.MaxExponentField()) - 1 - format.exponentBias;
    const auto precision = static_cast<std::int64_t>(format.precisionBits);
    std::mt19937_64 random(0xE0000000U + format.valueBits);
    Pairs pairs;
    for (int sample = 0; sample < TargetedPairs; ++sample) {
        const auto offset = [&](const std::int64_t span) {
            return static_cast<std::int64_t>(random() % static_cast<std::uint64_t>(span));
        };
        const std::int64_t sum =
            sample % 2 == 0 ? maximum - 2 + offset(4) : minimum - precision - 2 + offset(precision + 5);
        const auto [left, right] = ExponentsSummingTo(random, format, sum);
        // A significand of all ones now and then, so the product rounds up across the boundary.
        const WideInteger ones = WideInteger::AllOnes(format.precisionBits);
        pairs.emplace_back(Encode(random, format, left, sample % 5 == 0 ? ones : RandomSignificand(random, format)),
                           Encode(random, format, right, RandomSignificand(random, format)));
    }
    return pairs;
}

/// Products exactly halfway between two neighbours: 1.5 times an odd p-bit significand below 2^p * 2 / 3 needs p + 1
/// bits, the last of them set. The exponent sums cover the normal range and its subnormal edge, and a subnormal
/// operand times a power of two lands on subnormal ties too.
Pairs Ties(const FloatFormat &format) {
    const std::uint32_t precision = format.precisionBits;
    const std::int64_t minimum = 1 - static_cast<std::int64_t>(format.exponentBias);
    const std::int64_t maximum = static_cast<std::int64_t>(format.MaxExponentField()) - 1 - format.exponentBias;
    const auto one = [&](const std::uint32_t bit) { return WideInteger::FromUnsigned(1, precision).ShiftedLeft(bit); };
    std::mt19937_64 random(0x71E00000U + format.valueBits);
    Pairs pairs;
    for (int sample = 0; sample < TargetedPairs; ++sample) {
        const WideInteger odd = RandomBits(random, precision - 3)
                                    .Extended(precision, false)
                                    .BitwiseOr(one(precision - 1))
                                    .BitwiseOr(one(0));
        const WideInteger threeHalves = one(precision - 1).BitwiseOr(one(precision - 2));
        const std::int64_t span = sample % 2 == 0 ? maximum - minimum : static_cast<std::int64_t>(precision) + 4;
        const std::int64_t sum = minimum - (sample % 2 == 0 ? 0 : static_cast<std::int64_t>(precision) + 2) +
                                 static_cast<std::int64_t>(random() % static_cast<std::uint64_t>(span));
        const auto [left, right] = ExponentsSummingTo(random, format, sum);
        if (sample % 4 == 3) {
            // A subnormal whose last bit is set, scaled down by a power of two: the result halves it once or more.
            const WideInteger subnormal = RandomBits(random, format.FractionBits())
                                              .BitwiseOr(WideInteger::FromUnsigned(1, format.FractionBits()));
            pairs.emplace_back(
                subnormal.Extended(format.valueBits, false),
                Encode(random, format, -1 - static_cast<std::int64_t>(random() % 3), one(precision - 1)));
            continue;
        }
        const WideInteger first = Encode(random, format, left, odd);
        const WideInteger second = Encode(random, format, right, threeHalves);
        pairs.push_back(sample % 3 == 0 ? std::pair{second, first} : std::pair{first, second});
    }
    return pairs;
}

/// Runs every pair through the format's multiply helper and compares with `MultiplyFloat`.
void CheckMultiplication(const FloatFormat &format, const LirInterpreter &interpreter, const std::string &symbol,
                         const Pairs &pairs) {
    Mismatches mismatches;
    for (const auto &[left, right] : pairs) {
        const FloatEncoding expected =
            MultiplyFloat(FloatEncoding::FromBits(format, left), FloatEncoding::FromBits(format, right));
        const std::array arguments{left, right};
        mismatches.Check(interpreter.Run(symbol, arguments), expected.Bits(),
                         std::format("{} * {}", Hex(left), Hex(right)));
    }
    CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
}
} // namespace

TEST_CASE("synthesized multiply helpers match MultiplyFloat bit for bit") {
    for (const K kind : SoftwareKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        SoftFloatHelperRegistry registry;
        const std::string symbol = registry.Request({SoftFloatOperation::Multiply, kind, K::Unknown});
        std::vector<Diagnostic> diagnostics;
        const LirPackage package = PackageOf(registry.SynthesizePending(diagnostics));
        REQUIRE(diagnostics.empty());
        const LirInterpreter interpreter(package);

        SUBCASE("every pair of special values") {
            Pairs pairs;
            const std::vector<WideInteger> specials = SpecialValues(format);
            for (const WideInteger &left : specials) {
                for (const WideInteger &right : specials) {
                    pairs.emplace_back(left, right);
                }
            }
            CheckMultiplication(format, interpreter, symbol, pairs);
        }
        SUBCASE("random encodings") {
            std::mt19937_64 random(0x3A170000U + format.valueBits);
            Pairs pairs;
            for (int sample = 0; sample < RandomPairs; ++sample) {
                const WideInteger left = RandomBits(random, format.valueBits);
                pairs.emplace_back(left, RandomBits(random, format.valueBits));
            }
            CheckMultiplication(format, interpreter, symbol, pairs);
        }
        SUBCASE("exponent sums near overflow and underflow") {
            CheckMultiplication(format, interpreter, symbol, ExtremeSums(format));
        }
        SUBCASE("products halfway between two neighbours") {
            CheckMultiplication(format, interpreter, symbol, Ties(format));
        }
    }
}

TEST_CASE("every working width but float512's holds the product of two significands") {
    for (const K kind : SoftwareKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        const std::uint32_t product = 2 * format.precisionBits;
        CHECK_EQ(product <= SoftFloatWorkingBits(format), kind != K::Float512);
        CHECK(product <= 2 * SoftFloatWorkingBits(format));
    }
}

TEST_CASE("both back ends compile the synthesized multiply helpers") {
    SoftFloatHelperRegistry registry;
    for (const K kind : SoftwareKinds) {
        static_cast<void>(registry.Request({SoftFloatOperation::Multiply, kind, K::Unknown}));
    }
    std::vector<Diagnostic> diagnostics;
    const LirPackage package = PackageOf(registry.SynthesizePending(diagnostics));
    REQUIRE(diagnostics.empty());
    for (const Target::OS os : {Target::OS::Windows, Target::OS::Linux}) {
        CAPTURE(static_cast<int>(os));
        const RcuEmitter x86(package, "SoftFloat", os);
        CHECK_EQ(x86.Generate().size(), 1);
        CHECK(x86.Diagnostics().empty());
        const AArch64RcuEmitter arm(package, "SoftFloat", os);
        CHECK_EQ(arm.Generate().size(), 1);
        CHECK(arm.Diagnostics().empty());
    }
}
