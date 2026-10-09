// Synthesized software-float division and remainder helpers. Each mirrors `DivideFloat` or `RemainderFloat` in
// Numeric/SoftwareFloatDivide.cpp bit for bit, its NaN and special-value rules included.

#include "Lowering/HirToLir/SoftFloatBuilder.h"
#include "Numeric/SoftwareFloat.h"

#include <bit>
#include <cstdint>

namespace Rux::HirToLirDetail {
namespace {
[[nodiscard]] SoftFloatValue ClassCode(SoftFloatBuilder &builder, const FloatClass classification) {
    return builder.Constant(static_cast<std::int64_t>(classification), TypeRef::MakeUInt8());
}

/// Both operands decoded, with the class tests every helper here asks about.
struct DivisionOperands {
    SoftFloatUnpacked left;
    SoftFloatUnpacked right;
    SoftFloatValue leftNaN;
    SoftFloatValue anyNaN;
    SoftFloatValue anyInvalid;
    SoftFloatValue leftZero;
    SoftFloatValue rightZero;
    SoftFloatValue leftInfinity;
    SoftFloatValue rightInfinity;
};

[[nodiscard]] DivisionOperands Decode(SoftFloatBuilder &b, const FloatFormat &format) {
    DivisionOperands operands;
    operands.left = UnpackSoftFloat(b, format, b.Parameter(0));
    operands.right = UnpackSoftFloat(b, format, b.Parameter(1));
    const auto is = [&](const SoftFloatUnpacked &value, const FloatClass classification) {
        return b.Eq(value.classification, ClassCode(b, classification));
    };
    const auto isNaN = [&](const SoftFloatUnpacked &value) {
        return b.Or(is(value, FloatClass::QuietNaN), is(value, FloatClass::SignalingNaN));
    };
    operands.leftNaN = isNaN(operands.left);
    operands.anyNaN = b.Or(operands.leftNaN, isNaN(operands.right));
    operands.anyInvalid = b.Or(is(operands.left, FloatClass::Invalid), is(operands.right, FloatClass::Invalid));
    operands.leftZero = is(operands.left, FloatClass::Zero);
    operands.rightZero = is(operands.right, FloatClass::Zero);
    operands.leftInfinity = is(operands.left, FloatClass::Infinity);
    operands.rightInfinity = is(operands.right, FloatClass::Infinity);
    return operands;
}

/// Turns `result` into `FirstNaN` when either operand is a NaN: the first NaN with its sign and payload, quieted.
void ApplyFirstNaN(SoftFloatBuilder &b, const DivisionOperands &operands, SoftFloatUnpacked &result) {
    const SoftFloatUnpacked &left = operands.left;
    const SoftFloatUnpacked &right = operands.right;
    result.classification = b.Select(operands.anyNaN, ClassCode(b, FloatClass::QuietNaN), result.classification);
    result.negative =
        b.Select(operands.anyNaN, b.Select(operands.leftNaN, left.negative, right.negative), result.negative);
    result.nanPayload = b.Select(operands.leftNaN, left.nanPayload, right.nanPayload);
}

/// `IntegerSignificand`: the unpacked significand without its three extra bits, so the leading bit is at
/// `precisionBits - 1`.
[[nodiscard]] SoftFloatValue IntegerSignificand(SoftFloatBuilder &b, const SoftFloatUnpacked &value) {
    return b.ShiftRight(value.significand, FloatExtraPrecisionBits);
}

void SynthesizeDivide(SoftFloatBuilder &b, const FloatFormat &format) {
    const TypeRef work = SoftFloatWorkingType(format);
    const TypeRef exponentType = TypeRef::MakeInt32();
    const SoftFloatVariable remainder = b.Variable(work);
    const SoftFloatVariable quotient = b.Variable(work);
    const SoftFloatVariable steps = b.Variable(TypeRef::MakeUInt32());
    const DivisionOperands operands = Decode(b, format);
    const SoftFloatUnpacked &left = operands.left;
    const SoftFloatUnpacked &right = operands.right;
    const SoftFloatValue zero = b.Constant(0, work);

    // Restoring division of the integer significands, one quotient bit per step for `precisionBits + 3` steps. The
    // dividend starts in [divisor, 2 * divisor), doubled once if it was below, and every step keeps the remainder
    // below twice the divisor: under 2^(precisionBits + 1), which the working width holds with room to spare. A
    // special operand leaves a zero significand, which runs the same steps harmlessly; its class decides the result.
    const SoftFloatValue divisor = IntegerSignificand(b, right);
    const SoftFloatValue dividend = IntegerSignificand(b, left);
    const SoftFloatValue below = b.Lt(dividend, divisor);
    b.Set(remainder, b.Select(below, b.ShiftLeft(dividend, 1), dividend));
    const SoftFloatValue exponent = b.Sub(b.Sub(left.exponent, right.exponent), b.Resize(below, exponentType));
    b.Set(quotient, zero);
    b.Set(steps, b.Unsigned(format.precisionBits + FloatExtraPrecisionBits, 32));
    // Shifting the quotient left one bit per step places the first bit at `precisionBits + 2`, as setting bit
    // `position - 1` does. The remainder is doubled after the last step too, which keeps it zero or not.
    b.While([&] { return b.IsNonZero(b.Get(steps)); },
            [&] {
                const SoftFloatValue current = b.Get(remainder);
                const SoftFloatValue fits = b.Ge(current, divisor);
                b.Set(quotient, b.Or(b.ShiftLeft(b.Get(quotient), 1), b.Resize(fits, work)));
                b.Set(remainder, b.ShiftLeft(b.Select(fits, b.Sub(current, divisor), current), 1));
                b.Set(steps, b.Sub(b.Get(steps), b.Unsigned(1, 32)));
            });
    // A non-zero remainder means bits beyond the quotient, which the sticky bit records.
    const SoftFloatValue significand = b.Or(b.Get(quotient), b.Resize(b.IsNonZero(b.Get(remainder)), work));

    // `DivideFloat`'s early returns, latest first, so the first that applies decides. 0/0 and inf/inf, like an
    // invalid operand, give the positive quiet NaN that packing an `Invalid` produces.
    const SoftFloatValue negative = b.Ne(left.negative, right.negative);
    SoftFloatUnpacked result{ClassCode(b, FloatClass::Normal), negative, exponent, significand, zero};
    result.classification = b.Select(b.Or(operands.leftZero, operands.rightInfinity), ClassCode(b, FloatClass::Zero),
                                     result.classification);
    result.classification = b.Select(b.Or(operands.leftInfinity, operands.rightZero),
                                     ClassCode(b, FloatClass::Infinity), result.classification);
    const SoftFloatValue undefined =
        b.Or(operands.anyInvalid,
             b.Or(b.And(operands.leftZero, operands.rightZero), b.And(operands.leftInfinity, operands.rightInfinity)));
    result.classification = b.Select(undefined, ClassCode(b, FloatClass::Invalid), result.classification);
    result.negative = b.Select(undefined, b.Bool(false), result.negative);
    ApplyFirstNaN(b, operands, result);
    b.Return(PackSoftFloat(b, format, result));
}

/// Arithmetic modulo a divisor below 2^precisionBits on operands already reduced, so that a sum of two stays below
/// twice the divisor and fits the working width: the counterpart of `ShiftModulo`'s `MultiplyModulo` and `AddModulo`.
class Modular {
public:
    Modular(SoftFloatBuilder &builder, const TypeRef &work, const SoftFloatValue &inputModulus)
        : b(builder)
        , modulus(inputModulus)
        , zero(builder.Constant(0, work))
        , one(builder.Constant(1, work))
        , accumulator(builder.Variable(work))
        , addend(builder.Variable(work))
        , multiplier(builder.Variable(work)) {
    }

    /// All ones when `bit` (0 or 1 at the working type) is one, else zero.
    [[nodiscard]] SoftFloatValue Mask(const SoftFloatValue &bit) {
        return b.Sub(zero, bit);
    }

    [[nodiscard]] SoftFloatValue Add(const SoftFloatValue &left, const SoftFloatValue &right) {
        const SoftFloatValue sum = b.Add(left, right);
        return b.Sub(sum, b.And(modulus, Mask(b.Resize(b.Ge(sum, modulus), sum.type))));
    }

    /// Shift-and-add over the bits of `right` from the bottom, doubling `left` as it goes, until no bits are left:
    /// as many steps as `right` has significant bits.
    [[nodiscard]] SoftFloatValue Multiply(const SoftFloatValue &left, const SoftFloatValue &right) {
        b.Set(accumulator, zero);
        b.Set(addend, left);
        b.Set(multiplier, right);
        b.While([&] { return b.IsNonZero(b.Get(multiplier)); },
                [&] {
                    const SoftFloatValue bits = b.Get(multiplier);
                    const SoftFloatValue term = b.Get(addend);
                    b.Set(accumulator, Add(b.Get(accumulator), b.And(term, Mask(b.And(bits, one)))));
                    b.Set(addend, Add(term, term));
                    b.Set(multiplier, b.ShiftRight(bits, 1));
                });
        return b.Get(accumulator);
    }

    [[nodiscard]] SoftFloatValue One() const {
        return one;
    }

private:
    SoftFloatBuilder &b;
    SoftFloatValue modulus;
    SoftFloatValue zero;
    SoftFloatValue one;
    SoftFloatVariable accumulator;
    SoftFloatVariable addend;
    SoftFloatVariable multiplier;
};

void SynthesizeRemainder(SoftFloatBuilder &b, const FloatFormat &format) {
    const TypeRef work = SoftFloatWorkingType(format);
    const TypeRef count = TypeRef::MakeUInt32();
    const SoftFloatVariable reduced = b.Variable(work);
    const SoftFloatVariable factor = b.Variable(work);
    const SoftFloatVariable index = b.Variable(count);
    const DivisionOperands operands = Decode(b, format);
    const SoftFloatUnpacked &left = operands.left;
    const SoftFloatUnpacked &right = operands.right;
    const SoftFloatValue zero = b.Constant(0, work);
    const SoftFloatValue divisor = IntegerSignificand(b, right);
    Modular modular(b, work, divisor);

    // A NaN operand gives the first NaN quieted; an invalid operand, inf % y and x % 0 the positive quiet NaN.
    const SoftFloatValue undefined = b.Or(operands.anyInvalid, b.Or(operands.leftInfinity, operands.rightZero));
    b.If(b.Or(operands.anyNaN, undefined), [&] {
        SoftFloatUnpacked special{ClassCode(b, FloatClass::Invalid), b.Bool(false), b.Constant(0, TypeRef::MakeInt32()),
                                  zero, zero};
        ApplyFirstNaN(b, operands, special);
        b.Return(PackSoftFloat(b, format, special));
    });

    // x is its own remainder when it is zero, y is infinite, or |x| < |y|. Past this point both are finite and
    // non-zero, so the exponents and significands order the magnitudes.
    const SoftFloatValue smaller =
        b.Or(b.Lt(left.exponent, right.exponent),
             b.And(b.Eq(left.exponent, right.exponent), b.Lt(left.significand, right.significand)));
    b.If(b.Or(b.Or(operands.leftZero, operands.rightInfinity), smaller), [&] { b.Return(b.Parameter(0)); });

    // The exact remainder of dividend * 2^gap by the divisor. Every residue stays below the divisor, under
    // 2^precisionBits, so a gap of less than `width - precisionBits` bits is one shift and one `Mod`. A longer gap,
    // which reaches the millions for float512, takes 2^gap modulo the divisor by square-and-multiply over the gap's
    // bits instead, the way `ShiftModulo` does: the leading bits that fit a shift give the starting power, and each
    // remaining bit costs one modular squaring. A wide `Mod` is bit-serial in the back ends, so a gap reduced a chunk
    // at a time would cost one per chunk, hundreds of thousands for float512.
    const std::uint32_t width = SoftFloatBitsOf(work);
    const std::uint32_t chunk = width - format.precisionBits;
    const std::uint32_t window = static_cast<std::uint32_t>(std::bit_width(width) - 1);
    const SoftFloatValue start = b.Mod(IntegerSignificand(b, left), divisor);
    const SoftFloatValue gap = b.Resize(b.Sub(left.exponent, right.exponent), count);
    b.IfElse(
        b.Lt(gap, b.Unsigned(chunk, 32)), [&] { b.Set(reduced, b.Mod(b.ShiftLeftClamped(start, gap), divisor)); },
        [&] {
            // `squarings` low bits of the gap remain once its leading bits, below 2^window = width, are taken.
            const SoftFloatValue length = b.Sub(b.Unsigned(32, 32), b.CountLeadingZeros(gap));
            const SoftFloatValue windowBits = b.Unsigned(window, 32);
            const SoftFloatValue squarings =
                b.Select(b.Gt(length, windowBits), b.Sub(length, windowBits), b.Unsigned(0, 32));
            b.Set(factor, b.Mod(b.ShiftLeftClamped(modular.One(), b.ShiftRightClamped(gap, squarings)), divisor));
            b.Set(index, squarings);
            b.While([&] { return b.IsNonZero(b.Get(index)); },
                    [&] {
                        const SoftFloatValue next = b.Sub(b.Get(index), b.Unsigned(1, 32));
                        b.Set(index, next);
                        const SoftFloatValue square = modular.Multiply(b.Get(factor), b.Get(factor));
                        const SoftFloatValue bit =
                            b.Resize(b.And(b.ShiftRightClamped(gap, next), b.Unsigned(1, 32)), work);
                        b.Set(factor, modular.Add(square, b.And(square, modular.Mask(bit))));
                    });
            b.Set(reduced, modular.Multiply(start, b.Get(factor)));
        });

    // The remainder in units of the divisor's last place, with the dividend's sign; packing normalizes it as
    // `NormalizeFloat` would, and gives a zero remainder that sign too.
    const SoftFloatUnpacked result{ClassCode(b, FloatClass::Normal), left.negative, right.exponent,
                                   b.ShiftLeft(b.Get(reduced), FloatExtraPrecisionBits), zero};
    b.Return(PackSoftFloat(b, format, result));
}
} // namespace

void SynthesizeSoftFloatDivision(SoftFloatHelperSynthesis &synthesis) {
    SoftFloatBuilder builder(synthesis);
    if (synthesis.Key().operation == SoftFloatOperation::Remainder) {
        SynthesizeRemainder(builder, synthesis.Format());
    }
    else {
        SynthesizeDivide(builder, synthesis.Format());
    }
    builder.Finish();
}
} // namespace Rux::HirToLirDetail
