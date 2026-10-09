// Synthesized software-float conversion helpers: integer to float, float to integer and float to float.

#include "Lowering/HirToLir/HirToLirSoftFloat.h"
#include "Lowering/HirToLir/SoftFloatBuilder.h"
#include "Numeric/FloatEncoding.h"
#include "Numeric/SoftwareFloat.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <format>

namespace Rux::HirToLirDetail {
namespace {
[[nodiscard]] const FloatFormat *FloatFormatOf(const TypeRef::Kind kind) noexcept {
    const PrimitiveInfo *primitive = FindPrimitive(kind);
    return primitive && primitive->category == PrimitiveCategory::Float ? FindFloatFormat(primitive->bits) : nullptr;
}

[[nodiscard]] bool IsInteger(const TypeRef::Kind kind) noexcept {
    const PrimitiveInfo *primitive = FindPrimitive(kind);
    return primitive && primitive->bits != 0 &&
           (primitive->category == PrimitiveCategory::SignedInt ||
            primitive->category == PrimitiveCategory::UnsignedInt);
}

[[nodiscard]] SoftFloatValue ClassCode(SoftFloatBuilder &builder, const FloatClass classification) {
    return builder.Constant(static_cast<std::int64_t>(classification), TypeRef::MakeUInt8());
}

/// `ConvertFloat`: the value unpacked in `source`, its significand moved to `target`'s leading-bit position with every
/// bit shifted out kept sticky, and packed again. A NaN keeps the low bits of its payload, signaling or quiet as it
/// was, and an invalid encoding becomes a quiet NaN.
[[nodiscard]] SoftFloatValue ConvertFloatValue(SoftFloatBuilder &b, const FloatFormat &source,
                                               const SoftFloatValue &value, const FloatFormat &target) {
    SoftFloatUnpacked unpacked = UnpackSoftFloat(b, source, value);
    const TypeRef work = SoftFloatWorkingType(target);
    // A wider precision has a working type at least as wide, so a left shift fits after the resize, and a right shift
    // leaves a significand that fits the narrower working type.
    if (source.precisionBits < target.precisionBits) {
        unpacked.significand =
            b.ShiftLeft(b.Resize(unpacked.significand, work), target.precisionBits - source.precisionBits);
    }
    else {
        const SoftFloatValue distance = b.Unsigned(source.precisionBits - target.precisionBits, 32);
        unpacked.significand = b.Resize(b.ShiftRightSticky(unpacked.significand, distance), work);
    }
    unpacked.nanPayload = b.Resize(unpacked.nanPayload, work);
    unpacked.classification = b.Select(b.Eq(unpacked.classification, ClassCode(b, FloatClass::Invalid)),
                                       ClassCode(b, FloatClass::QuietNaN), unpacked.classification);
    return PackSoftFloat(b, target, unpacked);
}

/// `IntegerToFloat`. The magnitude is normalized in a register wide enough for both the integer and `target`'s
/// working type, so a wide integer keeps every bit it loses to rounding in the sticky bit, and is then narrowed to the
/// working type, which holds the normalized significand exactly.
[[nodiscard]] SoftFloatValue IntegerToFloatValue(SoftFloatBuilder &b, const SoftFloatValue &value,
                                                 const FloatFormat &target) {
    const std::uint32_t width = SoftFloatBitsOf(value.type);
    const TypeRef bitsType = SoftFloatUnsignedType(width);
    const SoftFloatValue bits = b.Resize(value, bitsType);
    const bool isSigned = value.type.IsSigned();
    const SoftFloatValue negative = isSigned ? b.BitSet(bits, width - 1) : b.Bool(false);
    // The minimum's negation wraps to itself, which read unsigned is its magnitude.
    const SoftFloatValue magnitude = isSigned ? b.Select(negative, b.Sub(b.Constant(0, bitsType), bits), bits) : bits;

    const TypeRef work = SoftFloatWorkingType(target);
    const TypeRef common = SoftFloatUnsignedType(std::max(width, SoftFloatBitsOf(work)));
    // Read as a significand whose leading bit sits where a normalized one does, the magnitude has the exponent of that
    // position; normalizing moves it and the exponent together to the magnitude's own leading bit.
    SoftFloatUnpacked unpacked{
        .classification = ClassCode(b, FloatClass::Normal),
        .negative = negative,
        .exponent = b.Constant(static_cast<std::int64_t>(target.precisionBits + FloatExtraPrecisionBits - 1),
                               TypeRef::MakeInt32()),
        .significand = b.Resize(magnitude, common),
        .nanPayload = b.Constant(0, work),
    };
    NormalizeSoftFloat(b, target, unpacked);
    unpacked.significand = b.Resize(unpacked.significand, work);
    return PackSoftFloat(b, target, unpacked);
}

/// `FloatToInteger` with the language's saturation: a value truncates toward zero, one beyond the range or an
/// infinity becomes the nearest limit, and a NaN, an invalid encoding or any negative value for an unsigned
/// `integer` becomes zero.
[[nodiscard]] SoftFloatValue FloatToIntegerValue(SoftFloatBuilder &b, const FloatFormat &source,
                                                 const SoftFloatValue &value, const TypeRef &integer) {
    const SoftFloatUnpacked unpacked = UnpackSoftFloat(b, source, value);
    const std::uint32_t width = SoftFloatBitsOf(integer);
    const bool isSigned = integer.IsSigned();
    const TypeRef bitsType = SoftFloatUnsignedType(width);
    const TypeRef exponentType = TypeRef::MakeInt32();
    const SoftFloatValue &exponent = unpacked.exponent;
    const SoftFloatValue normal = b.Eq(unpacked.classification, ClassCode(b, FloatClass::Normal));

    // The integer part: the significand without its three extra bits has its leading bit at `precisionBits - 1`, and
    // moves to the exponent's position. Out of range the amounts are clamped and the result is replaced below.
    const TypeRef common = SoftFloatUnsignedType(std::max(width, SoftFloatBitsOf(unpacked.significand.type)));
    const SoftFloatValue significand = b.Resize(b.ShiftRight(unpacked.significand, FloatExtraPrecisionBits), common);
    const SoftFloatValue point = b.Constant(static_cast<std::int64_t>(source.precisionBits) - 1, exponentType);
    const SoftFloatValue none = b.Constant(0, exponentType);
    const SoftFloatValue above = b.Ge(exponent, point);
    const SoftFloatValue left = b.Select(above, b.Sub(exponent, point), none);
    const SoftFloatValue right = b.Select(above, none, b.Sub(point, exponent));
    const SoftFloatValue magnitude =
        b.Resize(b.ShiftRightClamped(b.ShiftLeftClamped(significand, left), right), bitsType);
    const SoftFloatValue zero = b.Constant(0, bitsType);
    const SoftFloatValue truncated = b.Select(unpacked.negative, b.Sub(zero, magnitude), magnitude);

    // A leading bit at `width - 1` (`width` unsigned) or above is beyond the range, except that a signed negative
    // value there is either exactly the minimum or saturates to it. Below zero the value is a fraction.
    const SoftFloatValue limit = b.Constant(static_cast<std::int64_t>(width) - (isSigned ? 1 : 0), exponentType);
    SoftFloatValue fits = b.And(normal, b.And(b.Ge(exponent, none), b.Lt(exponent, limit)));
    if (!isSigned) {
        fits = b.And(fits, b.Not(unpacked.negative));
    }
    const SoftFloatValue overflows =
        b.Or(b.Eq(unpacked.classification, ClassCode(b, FloatClass::Infinity)), b.And(normal, b.Ge(exponent, limit)));
    const SoftFloatValue minimum =
        isSigned ? b.Constant(WideInteger::MinMagnitude(width, true).Negated(), bitsType) : zero;
    const SoftFloatValue saturated =
        b.Select(unpacked.negative, minimum, b.Constant(WideInteger::MaxValue(width, isSigned), bitsType));
    const SoftFloatValue result = b.Select(overflows, saturated, b.Select(fits, truncated, zero));
    return b.Resize(result, integer);
}
} // namespace

void SynthesizeSoftFloatConversion(SoftFloatHelperSynthesis &synthesis) {
    SoftFloatBuilder b(synthesis);
    const SoftFloatHelperKey &key = synthesis.Key();
    const FloatFormat &format = synthesis.Format();
    const FloatFormat *otherFormat = FloatFormatOf(key.other);
    if (!otherFormat && !IsInteger(key.other)) {
        synthesis.Fail(std::format("a conversion with '{}'", TypeRef::MakePrimitive(key.other).ToString()));
        return;
    }
    const SoftFloatValue value = b.Parameter(0);
    if (key.operation == SoftFloatOperation::ConvertTo) {
        b.Return(otherFormat ? ConvertFloatValue(b, format, value, *otherFormat)
                             : FloatToIntegerValue(b, format, value, TypeRef::MakePrimitive(key.other)));
    }
    else {
        b.Return(otherFormat ? ConvertFloatValue(b, *otherFormat, value, format)
                             : IntegerToFloatValue(b, value, format));
    }
    b.Finish();
}

/// An integer wider than 64 bits has no machine conversion to float32 or float64 on either architecture, and x86-64
/// converts only signed 64-bit integers, so a uint64 (or a uint, or a char64) at or above 2^63 would come out negative.
/// A bool is 0 or 1 at any width, which every back end converts correctly.
bool ConvertsThroughSoftFloatHelper(const TypeRef::Kind from, const TypeRef::Kind to, const Target::Arch arch) {
    const PrimitiveInfo *original = FindPrimitive(from);
    const PrimitiveInfo *source = FindPrimitive(SoftFloatHelperOperandKind(from));
    if ((to != TypeRef::Kind::Float32 && to != TypeRef::Kind::Float64) || !original || !source ||
        original->category == PrimitiveCategory::Bool || !IsInteger(source->kind)) {
        return false;
    }
    return source->bits > 64 ||
           (arch == Target::Arch::X86_64 && source->category == PrimitiveCategory::UnsignedInt && source->bits == 64);
}
} // namespace Rux::HirToLirDetail
