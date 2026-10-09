// Synthesized software-float addition and subtraction helpers.

#include "Lowering/HirToLir/SoftFloatBuilder.h"
#include "Numeric/FloatEncoding.h"

namespace Rux::HirToLirDetail {
void SynthesizeSoftFloatAddition(SoftFloatHelperSynthesis &synthesis) {
    // `AddOrSubtract`: its special cases in order, each returning early, then one aligned sum or difference rounded
    // by a single pack. Subtraction is addition with the right operand's sign flipped once it is known to be a number.
    SoftFloatBuilder b(synthesis);
    const FloatFormat &format = synthesis.Format();
    const bool subtract = synthesis.Key().operation == SoftFloatOperation::Subtract;
    const TypeRef floatType = SoftFloatFloatType(format);
    const SoftFloatValue leftValue = b.Parameter(0);
    const SoftFloatValue leftBits = b.BitsOfFloat(leftValue);
    const SoftFloatValue rightBits = b.BitsOfFloat(b.Parameter(1));
    const TypeRef bitsType = leftBits.type;
    const SoftFloatUnpacked left = UnpackSoftFloatBits(b, format, leftBits);
    const SoftFloatUnpacked right = UnpackSoftFloatBits(b, format, rightBits);

    const auto is = [&](const SoftFloatUnpacked &value, const FloatClass classification) {
        return b.Eq(value.classification, b.Constant(static_cast<std::int64_t>(classification), TypeRef::MakeUInt8()));
    };
    const auto isNaN = [&](const SoftFloatUnpacked &value) {
        return b.Or(is(value, FloatClass::QuietNaN), is(value, FloatClass::SignalingNaN));
    };
    const auto constant = [&](const FloatEncoding &encoding) { return b.Constant(encoding.Bits(), bitsType); };
    const SoftFloatValue none = b.Constant(0, bitsType);
    const SoftFloatValue signBit = b.OneAt(format.valueBits - 1, bitsType);
    const auto withSign = [&](const SoftFloatValue &bits, const SoftFloatValue &negative) {
        return b.Or(bits, b.Select(negative, signBit, none));
    };
    const auto returnBits = [&](const SoftFloatValue &bits) { b.Return(b.FloatOfBits(bits, floatType)); };
    const SoftFloatValue quietNaN = constant(FloatEncoding::QuietNaN(format));
    const SoftFloatValue infinity = constant(FloatEncoding::Infinity(format));

    // A NaN is quieted with its sign and payload kept: a NaN already has float80's integer bit, so quieting it is
    // setting the quiet bit. An invalid encoding becomes the default quiet NaN with its own sign.
    const SoftFloatValue quietBit = b.OneAt(format.FractionBits() - 1, bitsType);
    b.If(isNaN(left), [&] { returnBits(b.Or(leftBits, quietBit)); });
    b.If(isNaN(right), [&] { returnBits(b.Or(rightBits, quietBit)); });
    b.If(is(left, FloatClass::Invalid), [&] { returnBits(withSign(quietNaN, left.negative)); });
    b.If(is(right, FloatClass::Invalid), [&] { returnBits(withSign(quietNaN, right.negative)); });

    // Infinities of opposite signs cancel to a positive quiet NaN; otherwise an infinity wins.
    const SoftFloatValue rightNegative = subtract ? b.Not(right.negative) : right.negative;
    const SoftFloatValue rightInfinite = is(right, FloatClass::Infinity);
    b.If(is(left, FloatClass::Infinity), [&] {
        returnBits(b.Select(b.And(rightInfinite, b.Ne(left.negative, rightNegative)), quietNaN,
                            withSign(infinity, left.negative)));
    });
    b.If(rightInfinite, [&] { returnBits(withSign(infinity, rightNegative)); });

    // Two zeros give a zero that is negative only when both are; a zero on the right gives the left operand's own
    // encoding, so a float80 pseudo-denormal stays as it is. A zero on the left falls through to the sum, which with
    // its exponent taken from the right repacks the right operand with its effective sign.
    const SoftFloatValue leftZero = is(left, FloatClass::Zero);
    b.If(is(right, FloatClass::Zero), [&] {
        b.If(leftZero, [&] { returnBits(withSign(none, b.And(left.negative, rightNegative))); });
        b.Return(leftValue);
    });

    // Align the smaller exponent to the larger, keeping what shifts out in the sticky bit.
    const TypeRef exponentType = left.exponent.type;
    const SoftFloatValue noShift = b.Constant(0, exponentType);
    const SoftFloatValue leftExponent = b.Select(leftZero, right.exponent, left.exponent);
    const SoftFloatValue leftBehind = b.Lt(leftExponent, right.exponent);
    const SoftFloatValue rightBehind = b.Lt(right.exponent, leftExponent);
    const SoftFloatValue leftSignificand =
        b.ShiftRightSticky(left.significand, b.Select(leftBehind, b.Sub(right.exponent, leftExponent), noShift));
    const SoftFloatValue rightSignificand =
        b.ShiftRightSticky(right.significand, b.Select(rightBehind, b.Sub(leftExponent, right.exponent), noShift));
    const SoftFloatValue exponent = b.Select(leftBehind, right.exponent, leftExponent);

    // Like signs add. Unlike signs subtract the smaller magnitude from the larger and take the larger's sign, and
    // cancel exactly to positive zero. When the signs agree the result's sign is either one.
    const SoftFloatValue sameSign = b.Eq(left.negative, rightNegative);
    b.If(b.And(b.Not(sameSign), b.Eq(leftSignificand, rightSignificand)), [&] { returnBits(none); });
    const SoftFloatValue leftLarger = b.Gt(leftSignificand, rightSignificand);
    const SoftFloatValue difference =
        b.Select(leftLarger, b.Sub(leftSignificand, rightSignificand), b.Sub(rightSignificand, leftSignificand));
    const SoftFloatUnpacked result{
        b.Constant(static_cast<std::int64_t>(FloatClass::Normal), TypeRef::MakeUInt8()),
        b.Select(leftLarger, left.negative, rightNegative),
        exponent,
        b.Select(sameSign, b.Add(leftSignificand, rightSignificand), difference),
        b.Constant(0, leftSignificand.type),
    };
    // Packing normalizes first, as `NormalizeFloat` before `PackFloat` does.
    b.Return(PackSoftFloat(b, format, result));
    b.Finish();
}
} // namespace Rux::HirToLirDetail
