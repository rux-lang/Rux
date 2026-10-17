// Synthesized software-float unpack, pack, compare, add and subtract helpers, run against SoftwareFloat.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Lowering/HirToLir/HirToLirSoftFloat.h"
#include "Lowering/HirToLir/SoftFloatBuilder.h"
#include "Numeric/SoftwareFloat.h"
#include "SoftFloatLoweringTestSupport.h"
#include "Types/PrimitiveCatalog.h"

#include <array>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <functional>
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

constexpr std::array<K, 8> FloatKinds{K::Float8,  K::Float16,  K::Float32,  K::Float64,
                                      K::Float80, K::Float128, K::Float256, K::Float512};
/// Random patterns per format and test. float512 runs these in about the same time as the narrow formats.
constexpr int RandomPatterns = 2000;

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

/// A function `name(parameters) -> result` whose body `body` writes through the builder. It borrows a helper's
/// scaffolding and restates the signature, so the inline routines run on their own.
LirFunc Routine(const std::string &name, const std::vector<TypeRef> &parameters, const TypeRef &result,
                const std::function<void(SoftFloatBuilder &)> &body) {
    SoftFloatHelperRegistry registry;
    LirFunc function;
    {
        SoftFloatHelperSynthesis synthesis(registry, {SoftFloatOperation::ConvertTo, K::Float32, K::Float32}, function);
        function.params.front().type = parameters.front();
        for (std::size_t index = 1; index < parameters.size(); ++index) {
            function.params.push_back({synthesis.Builder().DefineParameter(), parameters[index], "extra"});
        }
        function.returnType = result;
        SoftFloatBuilder builder(synthesis);
        body(builder);
        builder.Finish();
        REQUIRE_MESSAGE(synthesis.Failure().empty(), synthesis.Failure());
    }
    function.name = name;
    return function;
}

/// Counts mismatches and keeps the first, so a broken routine reports once rather than once per input.
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

LirRunResult Run(const LirInterpreter &interpreter, const std::string &name, std::vector<WideInteger> arguments) {
    return interpreter.Run(name, arguments);
}
} // namespace

TEST_CASE("synthesized shifts and leading-zero counts match WideInteger without shifting past the width") {
    for (const std::uint32_t width : {8U, 32U, 64U, 128U, 512U}) {
        CAPTURE(width);
        const TypeRef type = SoftFloatUnsignedType(width);
        const TypeRef amount = TypeRef::MakeUInt32();
        const LirPackage package = PackageOf({
            Routine("sticky", {type, amount}, type,
                    [](SoftFloatBuilder &b) { b.Return(b.ShiftRightSticky(b.Parameter(0), b.Parameter(1))); }),
            Routine("left", {type, amount}, type,
                    [](SoftFloatBuilder &b) { b.Return(b.ShiftLeftClamped(b.Parameter(0), b.Parameter(1))); }),
            Routine("right", {type, amount}, type,
                    [](SoftFloatBuilder &b) { b.Return(b.ShiftRightClamped(b.Parameter(0), b.Parameter(1))); }),
            Routine("clz", {type}, amount, [](SoftFloatBuilder &b) { b.Return(b.CountLeadingZeros(b.Parameter(0))); }),
        });
        const LirInterpreter interpreter(package);
        std::mt19937_64 random(width);
        Mismatches mismatches;
        for (int sample = 0; sample < 300; ++sample) {
            const WideInteger value =
                RandomBits(random, width).ShiftedRight(static_cast<std::uint32_t>(random() % width), false);
            const auto shift = static_cast<std::uint32_t>(sample % 3 == 0 ? random() : random() % (width + 8));
            const WideInteger count = WideInteger::FromUnsigned(shift, 32);
            const std::string input = std::format("{} by {}", Hex(value), shift);
            mismatches.Check(Run(interpreter, "sticky", {value, count}),
                             Rux::ShiftRightSticky(value, shift).Truncated(width), "sticky " + input);
            const WideInteger zero = WideInteger::Zero(width);
            mismatches.Check(Run(interpreter, "left", {value, count}), shift >= width ? zero : value.ShiftedLeft(shift),
                             "left " + input);
            mismatches.Check(Run(interpreter, "right", {value, count}),
                             shift >= width ? zero : value.ShiftedRight(shift, false), "right " + input);
            mismatches.Check(Run(interpreter, "clz", {value}), WideInteger::FromUnsigned(value.CountLeadingZeros(), 32),
                             "clz " + input);
        }
        CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
    }
}

TEST_CASE("synthesized variables, branches and loops carry state across blocks") {
    const TypeRef type = SoftFloatUnsignedType(512);
    const TypeRef count = TypeRef::MakeUInt32();
    const TypeRef integer = TypeRef::MakeInt32();
    const LirPackage package = PackageOf({
        Routine("popcount", {type}, count,
                [&](SoftFloatBuilder &b) {
                    const SoftFloatVariable rest = b.Variable(b.Parameter(0));
                    const SoftFloatVariable total = b.Variable(b.Unsigned(0, 32));
                    b.While([&] { return b.IsNonZero(b.Get(rest)); },
                            [&] {
                                b.If(b.BitSet(b.Get(rest), 0),
                                     [&] { b.Set(total, b.Add(b.Get(total), b.Unsigned(1, 32))); });
                                b.Set(rest, b.ShiftRight(b.Get(rest), 1));
                            });
                    b.Return(b.Get(total));
                }),
        // Both arms return, so the join block is unreachable and `Finish` closes it.
        Routine("negative", {integer}, count,
                [&](SoftFloatBuilder &b) {
                    b.IfElse(
                        b.Lt(b.Parameter(0), b.Constant(0, integer)), [&] { b.Return(b.Unsigned(1, 32)); },
                        [&] { b.Return(b.Unsigned(0, 32)); });
                }),
    });
    const LirInterpreter interpreter(package);
    std::mt19937_64 random(512);
    for (int sample = 0; sample < 20; ++sample) {
        const WideInteger value =
            RandomBits(random, 512).ShiftedRight(static_cast<std::uint32_t>(random() % 512), false);
        CHECK_EQ(Run(interpreter, "popcount", {value}).value, WideInteger::FromUnsigned(value.PopulationCount(), 32));
    }
    CHECK_EQ(Run(interpreter, "negative", {WideInteger::FromUnsigned(5, 32).Negated()}).value,
             WideInteger::FromUnsigned(1, 32));
    CHECK_EQ(Run(interpreter, "negative", {WideInteger::FromUnsigned(7, 32)}).value, WideInteger::FromUnsigned(0, 32));
}

TEST_CASE("synthesized unpack and pack round-trip every encoding as SoftwareFloat does") {
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        const LirPackage package = PackageOf({Routine(
            "roundtrip", {TypeRef::MakePrimitive(kind)}, TypeRef::MakePrimitive(kind), [&](SoftFloatBuilder &b) {
                b.Return(PackSoftFloat(b, format, UnpackSoftFloat(b, format, b.Parameter(0))));
            })});
        const LirInterpreter interpreter(package);
        std::vector<WideInteger> inputs = SpecialValues(format);
        std::mt19937_64 random(0x5EED0000U + format.valueBits);
        for (int sample = 0; sample < RandomPatterns; ++sample) {
            inputs.push_back(RandomBits(random, format.valueBits));
        }
        Mismatches mismatches;
        for (const WideInteger &bits : inputs) {
            const FloatEncoding expected = PackFloat(UnpackFloat(FloatEncoding::FromBits(format, bits)));
            mismatches.Check(Run(interpreter, "roundtrip", {bits}), expected.Bits(), Hex(bits));
        }
        CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
    }
}

TEST_CASE("synthesized pack rounds, overflows and underflows arbitrary unpacked values as PackFloat does") {
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        const TypeRef work = SoftFloatWorkingType(format);
        const std::uint32_t width = SoftFloatBitsOf(work);
        const LirPackage package =
            PackageOf({Routine("pack", {TypeRef::MakeUInt8(), TypeRef::MakeBool(), TypeRef::MakeInt32(), work, work},
                               TypeRef::MakePrimitive(kind), [&](SoftFloatBuilder &b) {
                                   const SoftFloatUnpacked value{b.Parameter(0), b.Parameter(1), b.Parameter(2),
                                                                 b.Parameter(3), b.Parameter(4)};
                                   b.Return(PackSoftFloat(b, format, value));
                               })});
        const LirInterpreter interpreter(package);
        const std::int64_t minimumNormal = 1 - static_cast<std::int64_t>(format.exponentBias);
        const std::int64_t maximumNormal =
            static_cast<std::int64_t>(format.MaxExponentField()) - 1 - static_cast<std::int64_t>(format.exponentBias);
        std::mt19937_64 random(0xFACE0000U + format.valueBits);
        Mismatches mismatches;
        for (int sample = 0; sample < RandomPatterns; ++sample) {
            UnpackedFloat value;
            value.format = &format;
            value.classification = random() % 8 != 0 ? FloatClass::Normal : static_cast<FloatClass>(random() % 7);
            value.negative = random() % 2 != 0;
            // Any length, or one just past the precision, whose three extra bits make ties to even common.
            const auto length = static_cast<std::uint32_t>(sample % 4 == 0 ? format.precisionBits + 1 + random() % 5
                                                                           : 1 + random() % width);
            const WideInteger significand = random() % 20 == 0
                                              ? WideInteger::Zero(width)
                                              : RandomBits(random, width).ShiftedRight(width - length, false);
            // Near the bottom of the normal range, near the top, or anywhere between, each a little beyond.
            const std::int64_t span = format.precisionBits + 12;
            const auto within = [&](const std::int64_t low, const std::int64_t high) {
                return low + static_cast<std::int64_t>(random() % static_cast<std::uint64_t>(high - low));
            };
            value.exponent = sample % 3 == 0 ? within(minimumNormal - span, minimumNormal + span)
                           : sample % 3 == 1 ? within(maximumNormal - span, maximumNormal + span)
                                             : within(minimumNormal - span, maximumNormal + span);
            value.significand = significand.Extended(WideInteger::MaxBits, false);
            const WideInteger payload = RandomBits(random, width);
            value.nanPayload = payload.Extended(WideInteger::MaxBits, false);
            const std::vector<WideInteger> arguments{
                WideInteger::FromUnsigned(static_cast<std::uint64_t>(value.classification), 8),
                WideInteger::FromUnsigned(value.negative ? 1 : 0, 8),
                WideInteger::FromUnsigned(static_cast<std::uint64_t>(value.exponent), 64).Truncated(32), significand,
                payload};
            const std::string input =
                std::format("class {} sign {} exponent {} significand {}", static_cast<int>(value.classification),
                            value.negative, value.exponent, Hex(significand));
            mismatches.Check(Run(interpreter, "pack", arguments), PackFloat(value).Bits(), input);
        }
        CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
    }
}

TEST_CASE("synthesized compare helpers order every pair as CompareFloat does") {
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        SoftFloatHelperRegistry registry;
        const std::string symbol = registry.Request({SoftFloatOperation::Compare, kind, K::Unknown});
        std::vector<Diagnostic> diagnostics;
        const LirPackage package = PackageOf(registry.SynthesizePending(diagnostics));
        REQUIRE(diagnostics.empty());
        const LirInterpreter interpreter(package);

        std::vector<std::pair<WideInteger, WideInteger>> pairs;
        const std::vector<WideInteger> specials = SpecialValues(format);
        for (const WideInteger &left : specials) {
            for (const WideInteger &right : specials) {
                pairs.emplace_back(left, right);
            }
        }
        std::mt19937_64 random(0xC0DE0000U + format.valueBits);
        const WideInteger sign = WideInteger::FromUnsigned(1, format.valueBits).ShiftedLeft(format.valueBits - 1);
        for (int sample = 0; sample < RandomPatterns; ++sample) {
            const WideInteger left = RandomBits(random, format.valueBits);
            // Neighbours and sign flips reach the significand and sign comparisons that random pairs rarely do.
            const WideInteger nearby = left.BitwiseXor(WideInteger::FromUnsigned(random() % 4, format.valueBits));
            const WideInteger right = sample % 3 == 0 ? RandomBits(random, format.valueBits)
                                    : sample % 3 == 1 ? nearby
                                                      : nearby.BitwiseXor(sign);
            pairs.emplace_back(left, right);
        }
        Mismatches mismatches;
        for (const auto &[left, right] : pairs) {
            const FloatComparison expected =
                CompareFloat(FloatEncoding::FromBits(format, left), FloatEncoding::FromBits(format, right));
            mismatches.Check(Run(interpreter, symbol, {left, right}),
                             WideInteger::FromUnsigned(static_cast<std::uint64_t>(expected), 8),
                             std::format("{} vs {}", Hex(left), Hex(right)));
        }
        CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
    }
}

TEST_CASE("both back ends compile the synthesized compare helpers and pack and unpack routines") {
    SoftFloatHelperRegistry registry;
    std::vector<LirFunc> functions;
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        static_cast<void>(registry.Request({SoftFloatOperation::Compare, kind, K::Unknown}));
        functions.push_back(Routine(std::format("RoundTrip{}", format.valueBits), {TypeRef::MakePrimitive(kind)},
                                    TypeRef::MakePrimitive(kind), [&](SoftFloatBuilder &b) {
                                        b.Return(PackSoftFloat(b, format, UnpackSoftFloat(b, format, b.Parameter(0))));
                                    }));
    }
    std::vector<Diagnostic> diagnostics;
    for (LirFunc &helper : registry.SynthesizePending(diagnostics)) {
        functions.push_back(std::move(helper));
    }
    REQUIRE(diagnostics.empty());
    const LirPackage package = PackageOf(std::move(functions));
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

// Addition and subtraction

namespace {
constexpr std::array<K, 6> SoftwareKinds{K::Float8, K::Float16, K::Float80, K::Float128, K::Float256, K::Float512};

/// A finite encoding of either sign with exponent field `exponentField` and a random significand, or with only its
/// leading bit when `power` is set; float80 gets the integer bit its exponent calls for.
WideInteger RandomFinite(std::mt19937_64 &random, const FloatFormat &format, const std::uint32_t exponentField,
                         const bool power) {
    const std::uint32_t width = format.valueBits;
    WideInteger bits =
        power ? WideInteger::Zero(width) : RandomBits(random, format.FractionBits()).Extended(width, false);
    if (format.explicitIntegerBit && exponentField != 0) {
        bits = bits.BitwiseOr(WideInteger::FromUnsigned(1, width).ShiftedLeft(format.FractionBits()));
    }
    bits = bits.BitwiseOr(WideInteger::FromUnsigned(exponentField, width).ShiftedLeft(format.SignificandFieldBits()));
    if (random() % 2 != 0) {
        bits = bits.BitwiseOr(WideInteger::FromUnsigned(1, width).ShiftedLeft(width - 1));
    }
    return bits;
}

/// Pairs that reach each path of a sum: every pair of special values; random encodings; neighbours of equal or
/// opposite sign, which cancel all or most of their bits; operands about a precision apart, a power of two among them,
/// which round to nearest and break ties to even; and operands so far apart that the smaller one survives only in the
/// sticky bit.
std::vector<std::pair<WideInteger, WideInteger>> AdditionPairs(const FloatFormat &format) {
    std::vector<std::pair<WideInteger, WideInteger>> pairs;
    const std::vector<WideInteger> specials = SpecialValues(format);
    for (const WideInteger &left : specials) {
        for (const WideInteger &right : specials) {
            pairs.emplace_back(left, right);
        }
    }
    std::mt19937_64 random(0xADD00000U + format.valueBits);
    const std::uint32_t width = format.valueBits;
    const std::uint32_t largest = format.MaxExponentField() - 1;
    const std::uint64_t precision = format.precisionBits;
    const auto below = [](const std::uint32_t exponent, const std::uint64_t distance) {
        return distance >= exponent ? 0U : exponent - static_cast<std::uint32_t>(distance);
    };
    for (int sample = 0; sample < RandomPatterns; ++sample) {
        const auto exponent = static_cast<std::uint32_t>(1 + random() % largest);
        const WideInteger left = RandomFinite(random, format, exponent, false);
        WideInteger right;
        switch (sample % 4) {
        case 0:
            right = RandomBits(random, width);
            pairs.emplace_back(RandomBits(random, width), right);
            continue;
        case 1:
            right = left.BitwiseXor(WideInteger::FromUnsigned(random() % 8, width));
            if (random() % 2 != 0) {
                right = right.BitwiseXor(WideInteger::FromUnsigned(1, width).ShiftedLeft(width - 1));
            }
            break;
        case 2:
            right = RandomFinite(random, format, below(exponent, precision - 3 + random() % 7), random() % 3 == 0);
            break;
        default:
            right = RandomFinite(random, format, below(exponent, precision + 3 + random() % (3 * precision)),
                                 random() % 4 == 0);
            break;
        }
        if (random() % 2 != 0) {
            pairs.emplace_back(right, left);
        }
        else {
            pairs.emplace_back(left, right);
        }
    }
    return pairs;
}

/// Runs the add and subtract helpers of `kind` on every pair and compares them with `AddFloat` and `SubtractFloat`.
void CheckAddition(const K kind, const std::vector<std::pair<WideInteger, WideInteger>> &pairs) {
    const FloatFormat &format = FormatOf(kind);
    CAPTURE(format.valueBits);
    SoftFloatHelperRegistry registry;
    const std::string add = registry.Request({SoftFloatOperation::Add, kind, K::Unknown});
    const std::string subtract = registry.Request({SoftFloatOperation::Subtract, kind, K::Unknown});
    std::vector<Diagnostic> diagnostics;
    const LirPackage package = PackageOf(registry.SynthesizePending(diagnostics));
    REQUIRE(diagnostics.empty());
    const LirInterpreter interpreter(package);
    Mismatches mismatches;
    for (const auto &[left, right] : pairs) {
        const FloatEncoding leftEncoding = FloatEncoding::FromBits(format, left);
        const FloatEncoding rightEncoding = FloatEncoding::FromBits(format, right);
        const std::string input = std::format("{} and {}", Hex(left), Hex(right));
        mismatches.Check(Run(interpreter, add, {left, right}), AddFloat(leftEncoding, rightEncoding).Bits(),
                         "add " + input);
        mismatches.Check(Run(interpreter, subtract, {left, right}), SubtractFloat(leftEncoding, rightEncoding).Bits(),
                         "subtract " + input);
    }
    CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
}
} // namespace

TEST_CASE("synthesized add and subtract helpers match AddFloat and SubtractFloat bit for bit") {
    for (const K kind : SoftwareKinds) {
        CheckAddition(kind, AdditionPairs(FormatOf(kind)));
    }
}

TEST_CASE("synthesized float8 add and subtract helpers match on every pair of encodings") {
    std::vector<std::pair<WideInteger, WideInteger>> pairs;
    for (std::uint64_t left = 0; left < 256; ++left) {
        for (std::uint64_t right = 0; right < 256; ++right) {
            pairs.emplace_back(WideInteger::FromUnsigned(left, 8), WideInteger::FromUnsigned(right, 8));
        }
    }
    CheckAddition(K::Float8, pairs);
}

TEST_CASE("both back ends compile the synthesized add and subtract helpers") {
    SoftFloatHelperRegistry registry;
    for (const K kind : SoftwareKinds) {
        static_cast<void>(registry.Request({SoftFloatOperation::Add, kind, K::Unknown}));
        static_cast<void>(registry.Request({SoftFloatOperation::Subtract, kind, K::Unknown}));
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
