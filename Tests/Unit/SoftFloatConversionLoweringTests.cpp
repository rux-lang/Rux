// Synthesized software-float conversion helpers, run against SoftwareFloat.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Ir/Hir/Hir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Lowering/HirToLir/HirToLirSoftFloat.h"
#include "Numeric/SoftwareFloat.h"
#include "SoftFloatLoweringTestSupport.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <memory>
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
constexpr std::array<K, 6> SoftwareKinds{K::Float8, K::Float16, K::Float80, K::Float128, K::Float256, K::Float512};
constexpr std::array<K, 14> IntegerKinds{K::Int8,  K::Int16,  K::Int32,  K::Int64,  K::Int128,  K::Int256,  K::Int512,
                                         K::UInt8, K::UInt16, K::UInt32, K::UInt64, K::UInt128, K::UInt256, K::UInt512};
constexpr int RandomValues = 120;

// Copied from SoftFloatArithmeticLoweringTests.cpp.

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
        // A signaling NaN whose payload lies only above a narrower format's fraction: converting it empties it.
        maximum.BitwiseOr(integer).BitwiseOr(one(format.FractionBits() - 2)),
    };
    if (format.explicitIntegerBit) {
        values.push_back(integer);
        values.push_back(integer.BitwiseOr(payload));
        values.push_back(unit.BitwiseXor(integer));
        values.push_back(maximum);
        values.push_back(maximum.BitwiseOr(quiet).BitwiseOr(payload));
    }
    const std::size_t positive = values.size();
    for (std::size_t index = 0; index < positive; ++index) {
        values.push_back(values[index].BitwiseOr(one(width - 1)));
    }
    return values;
}

// Conversion inputs.

struct IntegerType {
    std::uint32_t width = 0;
    bool isSigned = false;
};

IntegerType IntegerOf(const K kind) {
    const PrimitiveInfo *primitive = FindPrimitive(kind);
    return {primitive->bits, primitive->category == PrimitiveCategory::SignedInt};
}

/// The key that owns a conversion: the source when it is a software float, the destination otherwise.
SoftFloatHelperKey KeyOf(const K from, const K to) {
    const bool softwareSource =
        from != K::Float32 && from != K::Float64 && FindPrimitive(from)->category == PrimitiveCategory::Float;
    return softwareSource ? SoftFloatHelperKey{SoftFloatOperation::ConvertTo, from, to}
                          : SoftFloatHelperKey{SoftFloatOperation::ConvertFrom, to, from};
}

/// Zero, one, the limits and their neighbours, powers of two and their neighbours, and patterns one bit longer than
/// `precision` placed so they round as exact ties, just above and just below them; each negated when signed.
std::vector<WideInteger> IntegerBoundaries(const IntegerType type, const std::uint32_t precision) {
    const std::uint32_t width = type.width;
    const auto one = [&](const std::uint32_t bit) { return WideInteger::FromUnsigned(1, width).ShiftedLeft(bit); };
    const WideInteger unit = one(0);
    const WideInteger maximum = WideInteger::MaxValue(width, type.isSigned);
    const WideInteger minimum =
        type.isSigned ? WideInteger::MinMagnitude(width, true).Negated() : WideInteger::Zero(width);
    std::vector<WideInteger> positive{unit, one(1).Added(unit)};
    std::vector<WideInteger> values{WideInteger::Zero(width), maximum,
                                    maximum.Subtracted(unit), minimum,
                                    minimum.Added(unit),      WideInteger::AllOnes(width)};
    const std::uint32_t top = width - (type.isSigned ? 1 : 0);
    for (std::uint32_t bit = 1; bit < top; bit += width <= 64 ? 1 : 7) {
        positive.push_back(one(bit));
        positive.push_back(one(bit).Subtracted(unit));
        positive.push_back(one(bit).Added(unit));
    }
    for (const std::uint32_t length : {precision + 1, precision + 2, precision + 4}) {
        for (const WideInteger &low : {unit, one(1).Added(unit), one(2).Subtracted(unit)}) {
            for (const std::uint32_t shift : {0U, 1U, 9U, top > length ? top - length : 0U}) {
                if (length + shift <= top) {
                    positive.push_back(one(length - 1).BitwiseOr(low).ShiftedLeft(shift));
                }
            }
        }
    }
    for (const WideInteger &value : positive) {
        values.push_back(value);
        if (type.isSigned) {
            values.push_back(value.Negated());
        }
    }
    return values;
}

WideInteger RandomInteger(std::mt19937_64 &random, const std::uint32_t width) {
    return RandomBits(random, width).ShiftedRight(static_cast<std::uint32_t>(random() % width), random() % 2 != 0);
}

/// An encoding of `format` with a random sign and fraction and an unbiased exponent in `[low, high]`, kept to the
/// finite range.
WideInteger RandomNear(std::mt19937_64 &random, const FloatFormat &format, const std::int64_t low,
                       const std::int64_t high) {
    const std::int64_t exponent =
        low + static_cast<std::int64_t>(random() % static_cast<std::uint64_t>(high - low + 1));
    const std::int64_t field = std::clamp<std::int64_t>(exponent + format.exponentBias, 0,
                                                        static_cast<std::int64_t>(format.MaxExponentField()) - 1);
    const std::uint32_t width = format.valueBits;
    WideInteger bits = RandomBits(random, width).Truncated(format.FractionBits()).Extended(width, false);
    if (format.explicitIntegerBit && field != 0) {
        bits = bits.BitwiseOr(WideInteger::FromUnsigned(1, width).ShiftedLeft(format.FractionBits()));
    }
    bits = bits.BitwiseOr(
        WideInteger::FromUnsigned(static_cast<std::uint64_t>(field), width).ShiftedLeft(format.SignificandFieldBits()));
    return random() % 2 != 0 ? bits.BitwiseOr(WideInteger::FromUnsigned(1, width).ShiftedLeft(width - 1)) : bits;
}

/// The constant evaluator's cast: `FloatToInteger`, saturating out of range, and zero for a NaN.
WideInteger SaturatingToInteger(const FloatEncoding &encoding, const IntegerType type) {
    const FloatToIntegerResult converted = FloatToInteger(encoding, type.width, type.isSigned);
    if (converted.HasValue()) {
        return converted.value;
    }
    const FloatClass classification = encoding.Classify();
    if (classification != FloatClass::Infinity && classification != FloatClass::Normal &&
        classification != FloatClass::Subnormal) {
        return WideInteger::Zero(type.width);
    }
    if (encoding.IsNegative()) {
        return type.isSigned ? WideInteger::MinMagnitude(type.width, true).Negated() : WideInteger::Zero(type.width);
    }
    return WideInteger::MaxValue(type.width, type.isSigned);
}

LirPackage Synthesized(const std::vector<SoftFloatHelperKey> &keys) {
    SoftFloatHelperRegistry registry;
    for (const SoftFloatHelperKey &key : keys) {
        static_cast<void>(registry.Request(key));
    }
    std::vector<Diagnostic> diagnostics;
    LirPackage package = PackageOf(registry.SynthesizePending(diagnostics));
    REQUIRE_MESSAGE(diagnostics.empty(), (diagnostics.empty() ? std::string() : diagnostics.front().message));
    return package;
}

LirRunResult Run(const LirInterpreter &interpreter, const SoftFloatHelperKey &key, const WideInteger &argument) {
    const std::vector<WideInteger> arguments{argument};
    return interpreter.Run(SoftFloatHelperName(key), arguments);
}

// Lowering.

HirExprPtr CastOf(const std::string &name, const K from, const K to) {
    auto var = std::make_unique<HirVarExpr>();
    var->name = name;
    var->type = TypeRef::MakePrimitive(from);
    auto cast = std::make_unique<HirCastExpr>();
    cast->operand = std::move(var);
    cast->targetType = TypeRef::MakePrimitive(to);
    cast->type = TypeRef::MakePrimitive(to);
    return cast;
}

/// `func <name>(value: from) -> to { return value as to; }`, lowered for x86-64, and the instruction it returns.
std::pair<LirPackage, LirInstr> LowerCast(const K from, const K to) {
    HirModule module;
    module.name = "main";
    HirFunc &function = module.funcs.emplace_back();
    function.name = "Convert";
    function.returnType = TypeRef::MakePrimitive(to);
    function.params.push_back({"value", TypeRef::MakePrimitive(from), false, 0});
    auto returned = std::make_unique<HirReturnStmt>();
    returned->value = CastOf("value", from, to);
    function.body.emplace();
    function.body->stmts.push_back(std::move(returned));
    HirPackage package;
    package.modules.push_back(std::move(module));
    TargetContext target = TargetContext::CreateNative();
    target.arch = Target::Arch::X86_64;
    HirToLirLowering lowering(std::move(package), target);
    LirPackage lowered = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    for (const LirFunc &lowerFunction : lowered.modules.front().funcs) {
        if (lowerFunction.name != "Convert") {
            continue;
        }
        const LirReg result = *lowerFunction.blocks.back().term->retVal;
        for (const LirBlock &block : lowerFunction.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                if (instruction.dst == result) {
                    return {std::move(lowered), instruction};
                }
            }
        }
    }
    FAIL("the lowered conversion was not found");
    return {};
}
} // namespace

TEST_CASE("synthesized integer-to-float helpers round as IntegerToFloat does") {
    for (const K kind : FloatKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        std::vector<SoftFloatHelperKey> keys;
        for (const K integer : IntegerKinds) {
            keys.push_back(KeyOf(integer, kind));
        }
        const LirPackage package = Synthesized(keys);
        const LirInterpreter interpreter(package);
        std::mt19937_64 random(0x1F00U + format.valueBits);
        Mismatches mismatches;
        for (const K integer : IntegerKinds) {
            const IntegerType type = IntegerOf(integer);
            std::vector<WideInteger> inputs = IntegerBoundaries(type, format.precisionBits);
            for (int sample = 0; sample < RandomValues; ++sample) {
                inputs.push_back(RandomInteger(random, type.width));
            }
            for (const WideInteger &value : inputs) {
                mismatches.Check(Run(interpreter, KeyOf(integer, kind), value),
                                 IntegerToFloat(value, type.isSigned, format).Bits(),
                                 std::format("{} {}", FindPrimitive(integer)->name, Hex(value)));
            }
        }
        CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
    }
}

TEST_CASE("synthesized float-to-integer helpers truncate and saturate as constant folding does") {
    for (const K kind : SoftwareKinds) {
        const FloatFormat &format = FormatOf(kind);
        CAPTURE(format.valueBits);
        std::vector<SoftFloatHelperKey> keys;
        for (const K integer : IntegerKinds) {
            keys.push_back(KeyOf(kind, integer));
        }
        const LirPackage package = Synthesized(keys);
        const LirInterpreter interpreter(package);
        std::mt19937_64 random(0x2F00U + format.valueBits);
        const WideInteger unit = WideInteger::FromUnsigned(1, format.valueBits);
        Mismatches mismatches;
        for (const K integer : IntegerKinds) {
            const IntegerType type = IntegerOf(integer);
            std::vector<WideInteger> inputs = SpecialValues(format);
            // The integer's limits and powers of two as floats, and the encodings either side of each.
            for (const WideInteger &value : IntegerBoundaries(type, format.precisionBits)) {
                const WideInteger bits = IntegerToFloat(value, type.isSigned, format).Bits();
                inputs.push_back(bits);
                inputs.push_back(bits.Added(unit));
                inputs.push_back(bits.Subtracted(unit));
            }
            for (int sample = 0; sample < RandomValues; ++sample) {
                inputs.push_back(RandomBits(random, format.valueBits));
                inputs.push_back(RandomNear(random, format, -3, static_cast<std::int64_t>(type.width) + 2));
            }
            for (const WideInteger &bits : inputs) {
                mismatches.Check(Run(interpreter, KeyOf(kind, integer), bits),
                                 SaturatingToInteger(FloatEncoding::FromBits(format, bits), type),
                                 std::format("{} {}", FindPrimitive(integer)->name, Hex(bits)));
            }
        }
        CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
    }
}

TEST_CASE("synthesized float-to-float helpers convert every pair as ConvertFloat does") {
    for (const K from : FloatKinds) {
        for (const K to : FloatKinds) {
            const bool native = (from == K::Float32 || from == K::Float64) && (to == K::Float32 || to == K::Float64);
            if (from == to || native) {
                continue;
            }
            const FloatFormat &source = FormatOf(from);
            const FloatFormat &target = FormatOf(to);
            CAPTURE(source.valueBits);
            CAPTURE(target.valueBits);
            const SoftFloatHelperKey key = KeyOf(from, to);
            const LirPackage package = Synthesized({key});
            const LirInterpreter interpreter(package);
            std::mt19937_64 random(0x3F00U + source.valueBits * 1000U + target.valueBits);
            std::vector<WideInteger> inputs = SpecialValues(source);
            const std::int64_t minimumNormal = 1 - static_cast<std::int64_t>(target.exponentBias);
            const std::int64_t maximumNormal = static_cast<std::int64_t>(target.MaxExponentField()) - 1 -
                                               static_cast<std::int64_t>(target.exponentBias);
            for (int sample = 0; sample < 2 * RandomValues; ++sample) {
                inputs.push_back(RandomBits(random, source.valueBits));
                WideInteger near =
                    RandomNear(random, source, minimumNormal - target.precisionBits - 2, maximumNormal + 1);
                inputs.push_back(near);
                // The fraction bits the target drops set to an exact tie, just above it and just below it.
                if (source.precisionBits > target.precisionBits) {
                    const std::uint32_t dropped = source.precisionBits - target.precisionBits;
                    const WideInteger half = WideInteger::FromUnsigned(1, source.valueBits).ShiftedLeft(dropped - 1);
                    const WideInteger kept =
                        near.BitwiseAnd(WideInteger::AllOnes(dropped).Extended(source.valueBits, false).BitwiseNot());
                    inputs.push_back(kept.BitwiseOr(half));
                    inputs.push_back(kept.BitwiseOr(half).BitwiseOr(WideInteger::FromUnsigned(1, source.valueBits)));
                    inputs.push_back(kept.BitwiseOr(half.Subtracted(WideInteger::FromUnsigned(1, source.valueBits))));
                }
            }
            Mismatches mismatches;
            for (const WideInteger &bits : inputs) {
                mismatches.Check(Run(interpreter, key, bits),
                                 ConvertFloat(FloatEncoding::FromBits(source, bits), target).Bits(), Hex(bits));
            }
            CHECK_MESSAGE(mismatches.count == 0, mismatches.first);
        }
    }
}

TEST_CASE("synthesized conversions keep NaNs signaling, quiet invalid encodings and saturate non-finite values") {
    const FloatFormat &half = FormatOf(K::Float16);
    const FloatFormat &quad = FormatOf(K::Float128);
    const FloatFormat &extended = FormatOf(K::Float80);
    const FloatFormat &octuple = FormatOf(K::Float512);
    const SoftFloatHelperKey widen = KeyOf(K::Float16, K::Float128);
    const SoftFloatHelperKey narrow = KeyOf(K::Float128, K::Float16);
    const SoftFloatHelperKey invalid = KeyOf(K::Float80, K::Float16);
    const SoftFloatHelperKey toInt32 = KeyOf(K::Float512, K::Int32);
    const SoftFloatHelperKey toUInt8 = KeyOf(K::Float512, K::UInt8);
    const LirPackage package = Synthesized({widen, narrow, invalid, toInt32, toUInt8});
    const LirInterpreter interpreter(package);
    const auto classify = [&](const SoftFloatHelperKey &key, const FloatFormat &target, const WideInteger &bits) {
        const LirRunResult result = Run(interpreter, key, bits);
        REQUIRE_MESSAGE(result.Ok(), result.Describe());
        return FloatEncoding::FromBits(target, *result.value);
    };

    // A signaling float16 NaN with payload 5 widens to a signaling float128 NaN with the same low payload bits.
    const FloatEncoding widened = classify(widen, quad, WideInteger::FromUnsigned(0x7C05, 16));
    CHECK_EQ(widened.Classify(), FloatClass::SignalingNaN);
    CHECK_EQ(widened.Bits().Word64(0), 5U);
    // A signaling float128 NaN whose payload lies only above float16's fraction stays signaling with payload 1.
    const WideInteger high = WideInteger::FromUnsigned(0x7FFF, 128)
                                 .ShiftedLeft(quad.SignificandFieldBits())
                                 .BitwiseOr(WideInteger::FromUnsigned(1, 128).ShiftedLeft(100));
    const FloatEncoding narrowed = classify(narrow, half, high);
    CHECK_EQ(narrowed.Classify(), FloatClass::SignalingNaN);
    CHECK_EQ(narrowed.Bits().Word64(0), 0x7C01U);
    // An unnormal float80 (a non-zero exponent without the integer bit) is invalid and becomes a quiet NaN.
    const WideInteger unnormal =
        WideInteger::FromUnsigned(static_cast<std::uint64_t>(extended.exponentBias), 80).ShiftedLeft(64);
    CHECK_EQ(classify(invalid, half, unnormal).Classify(), FloatClass::QuietNaN);

    const auto integer = [&](const SoftFloatHelperKey &key, const FloatEncoding &value) {
        const LirRunResult result = Run(interpreter, key, value.Bits());
        REQUIRE_MESSAGE(result.Ok(), result.Describe());
        return Hex(*result.value);
    };
    CHECK_EQ(integer(toInt32, FloatEncoding::QuietNaN(octuple)), Hex(WideInteger::Zero(32)));
    CHECK_EQ(integer(toInt32, FloatEncoding::SignalingNaN(octuple, true)), Hex(WideInteger::Zero(32)));
    CHECK_EQ(integer(toInt32, FloatEncoding::Infinity(octuple)), Hex(WideInteger::MaxValue(32, true)));
    CHECK_EQ(integer(toInt32, FloatEncoding::Infinity(octuple, true)),
             Hex(WideInteger::MinMagnitude(32, true).Negated()));
    CHECK_EQ(integer(toUInt8, FloatEncoding::Infinity(octuple)), Hex(WideInteger::MaxValue(8, false)));
    CHECK_EQ(integer(toUInt8, FloatEncoding::Infinity(octuple, true)), Hex(WideInteger::Zero(8)));
    const WideInteger minusOne = WideInteger::FromUnsigned(1, 64).Negated();
    CHECK_EQ(integer(toUInt8, IntegerToFloat(minusOne, true, octuple)), Hex(WideInteger::Zero(8)));
    CHECK_EQ(integer(toInt32, IntegerToFloat(minusOne, true, octuple)), Hex(minusOne.Truncated(32)));
}

TEST_CASE("wide integers and uint64 on x86-64 convert to native floats through helpers") {
    // No machine conversion exists from a wider integer, and x86-64 converts only signed 64-bit integers.
    CHECK(ConvertsThroughSoftFloatHelper(K::Int256, K::Float64, Target::Arch::X86_64));
    CHECK(ConvertsThroughSoftFloatHelper(K::UInt128, K::Float32, Target::Arch::AArch64));
    CHECK(ConvertsThroughSoftFloatHelper(K::UInt64, K::Float64, Target::Arch::X86_64));
    CHECK(ConvertsThroughSoftFloatHelper(K::UInt, K::Float32, Target::Arch::X86_64));
    CHECK(ConvertsThroughSoftFloatHelper(K::Char64, K::Float64, Target::Arch::X86_64));
    CHECK_FALSE(ConvertsThroughSoftFloatHelper(K::UInt64, K::Float64, Target::Arch::AArch64));
    CHECK_FALSE(ConvertsThroughSoftFloatHelper(K::Int64, K::Float64, Target::Arch::X86_64));
    CHECK_FALSE(ConvertsThroughSoftFloatHelper(K::UInt32, K::Float32, Target::Arch::X86_64));
    CHECK_FALSE(ConvertsThroughSoftFloatHelper(K::Bool128, K::Float64, Target::Arch::X86_64));
    CHECK_FALSE(ConvertsThroughSoftFloatHelper(K::Int128, K::Int64, Target::Arch::X86_64));
    CHECK_FALSE(ConvertsThroughSoftFloatHelper(K::Float32, K::Float64, Target::Arch::X86_64));

    const auto [widePackage, wide] = LowerCast(K::Int256, K::Float64);
    CHECK_EQ(wide.op, LirOpcode::Call);
    CHECK_EQ(wide.strArg, "__rux_float_f64_from_int256");
    const auto [unsignedPackage, fromUnsigned] = LowerCast(K::UInt64, K::Float64);
    CHECK_EQ(fromUnsigned.op, LirOpcode::Call);
    CHECK_EQ(fromUnsigned.strArg, "__rux_float_f64_from_uint64");
    const auto [signedPackage, fromSigned] = LowerCast(K::Int64, K::Float64);
    CHECK_EQ(fromSigned.op, LirOpcode::Cast);

    // A wide character converts as the unsigned integer of its width does, through that integer's helper.
    CHECK(ConvertsThroughSoftFloatHelper(K::Char128, K::Float64, Target::Arch::AArch64));
    const auto [charPackage, fromChar] = LowerCast(K::Char128, K::Float64);
    CHECK_EQ(fromChar.op, LirOpcode::Call);
    CHECK_EQ(fromChar.strArg, "__rux_float_f64_from_uint128");
    const auto [softPackage, softFromChar] = LowerCast(K::Char512, K::Float128);
    CHECK_EQ(softFromChar.op, LirOpcode::Call);
    CHECK_EQ(softFromChar.strArg, "__rux_float_f128_from_uint512");

    for (const LirFunc &function : unsignedPackage.modules.front().funcs) {
        if (function.name == "__rux_float_f64_from_uint64") {
            CHECK_FALSE(function.isPublic);
            CHECK_EQ(function.returnType, TypeRef::MakePrimitive(K::Float64));
            REQUIRE_EQ(function.params.size(), 1);
            CHECK_EQ(function.params.front().type, TypeRef::MakePrimitive(K::UInt64));
            return;
        }
    }
    FAIL("the uint64 conversion helper was not synthesized");
}

TEST_CASE("both back ends compile every synthesized conversion helper") {
    std::vector<SoftFloatHelperKey> keys;
    for (const K from : FloatKinds) {
        for (const K to : FloatKinds) {
            const bool native = (from == K::Float32 || from == K::Float64) && (to == K::Float32 || to == K::Float64);
            if (from != to && !native) {
                keys.push_back(KeyOf(from, to));
            }
        }
        for (const K integer : IntegerKinds) {
            keys.push_back(KeyOf(integer, from));
        }
    }
    for (const K from : SoftwareKinds) {
        for (const K integer : IntegerKinds) {
            keys.push_back(KeyOf(from, integer));
        }
    }
    const LirPackage package = Synthesized(keys);
    const RcuEmitter x86(package, "SoftFloat", Target::OS::Windows);
    CHECK_EQ(x86.Generate().size(), 1);
    CHECK(x86.Diagnostics().empty());
    const AArch64RcuEmitter arm(package, "SoftFloat", Target::OS::Linux);
    CHECK_EQ(arm.Generate().size(), 1);
    CHECK(arm.Diagnostics().empty());
}
