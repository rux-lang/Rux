// Synthesized software-float multiplication helpers.

#include "Lowering/HirToLir/SoftFloatBuilder.h"
#include "Numeric/SoftwareFloat.h"

#include <format>

namespace Rux::HirToLirDetail {
namespace {
/// The exact product of two normalized working significands, reduced to the working width for `PackFloat`.
///
/// `MultiplyFloat` multiplies the two p-bit integers under the three extra bits, whose product of 2p - 1 or 2p bits
/// has its leading bit at 2p - 2 or 2p - 1. A fixed right shift by p - 4, with every bit shifted out folded into the
/// sticky bit, puts that leading bit at p + 2 or p + 3, and the value is then `significand * 2^(left + right -
/// (p + 2))` for the operands' exponents. Packing normalizes the second position with one more sticky shift, which is
/// the single shift `ToUnpacked` makes.
///
/// Every working width holds the 2p-bit product except float512's: 2p is 8 of 32 bits for float8, 22 of 32 for
/// float16, 128 of 128 for float80, 226 of 256 for float128 and 474 of 512 for float256. (Float80's significands with
/// their extra bits would need 134, so the extra bits, always zero after unpacking, are dropped first.) Float512's 978
/// bits are formed from 256-bit halves as a high and a low uint512.
[[nodiscard]] SoftFloatValue ProductSignificand(SoftFloatBuilder &b, const FloatFormat &format,
                                                const SoftFloatValue &left, const SoftFloatValue &right) {
    const std::uint32_t width = SoftFloatBitsOf(left.type);
    const std::uint32_t precision = format.precisionBits;
    if (precision < 4 || 2 * precision > 2 * width) {
        b.Synthesis().Fail(std::format("no product for a {}-bit significand in {} bits", precision, width));
        return b.Constant(0, left.type);
    }
    const std::uint32_t shift = precision - 4;
    const SoftFloatValue leftInteger = b.ShiftRight(left, FloatExtraPrecisionBits);
    const SoftFloatValue rightInteger = b.ShiftRight(right, FloatExtraPrecisionBits);
    const auto sticky = [&](const SoftFloatValue &low) {
        return b.Resize(b.IsNonZero(b.And(low, b.LowMask(shift, low.type))), low.type);
    };

    if (2 * precision <= width) {
        const SoftFloatValue product = b.Mul(leftInteger, rightInteger);
        return shift == 0 ? product : b.Or(b.ShiftRight(product, shift), sticky(product));
    }

    // Four partial products of 256-bit halves, each exact in 512 bits. The two middle ones sum to under 2^490, so
    // only the low word's addition can carry, which an unsigned compare against one of its addends detects.
    const std::uint32_t half = width / 2;
    const SoftFloatValue halfMask = b.LowMask(half, left.type);
    const SoftFloatValue leftLow = b.And(leftInteger, halfMask);
    const SoftFloatValue leftHigh = b.ShiftRight(leftInteger, half);
    const SoftFloatValue rightLow = b.And(rightInteger, halfMask);
    const SoftFloatValue rightHigh = b.ShiftRight(rightInteger, half);
    const SoftFloatValue lowest = b.Mul(leftLow, rightLow);
    const SoftFloatValue middle = b.Add(b.Mul(leftLow, rightHigh), b.Mul(leftHigh, rightLow));
    const SoftFloatValue low = b.Add(lowest, b.ShiftLeft(middle, half));
    const SoftFloatValue carry = b.Resize(b.Lt(low, lowest), left.type);
    const SoftFloatValue high = b.Add(b.Add(b.Mul(leftHigh, rightHigh), b.ShiftRight(middle, half)), carry);

    // The shift is below the word, so the high word supplies the top bits and the low word the rest and the sticky
    // bit. The high word is under 2^(2p - width), so it loses nothing moving up by width - shift.
    return b.Or(b.Or(b.ShiftLeft(high, width - shift), b.ShiftRight(low, shift)), sticky(low));
}
} // namespace

void SynthesizeSoftFloatMultiplication(SoftFloatHelperSynthesis &synthesis) {
    // `MultiplyFloat`: its early returns in order, each choosing what one final `PackFloat` encodes.
    SoftFloatBuilder b(synthesis);
    const FloatFormat &format = synthesis.Format();
    const TypeRef work = SoftFloatWorkingType(format);
    const SoftFloatUnpacked left = UnpackSoftFloat(b, format, b.Parameter(0));
    const SoftFloatUnpacked right = UnpackSoftFloat(b, format, b.Parameter(1));
    const auto code = [&](const FloatClass classification) {
        return b.Constant(static_cast<std::int64_t>(classification), TypeRef::MakeUInt8());
    };
    const auto is = [&](const SoftFloatUnpacked &value, const FloatClass classification) {
        return b.Eq(value.classification, code(classification));
    };
    const auto isNaN = [&](const SoftFloatUnpacked &value) {
        return b.Or(is(value, FloatClass::QuietNaN), is(value, FloatClass::SignalingNaN));
    };

    // A zero carrying the product's sign until a case below says otherwise.
    const SoftFloatVariable classification = b.Variable(code(FloatClass::Zero));
    const SoftFloatVariable negative = b.Variable(b.Ne(left.negative, right.negative));
    const SoftFloatVariable exponent = b.Variable(b.Constant(0, TypeRef::MakeInt32()));
    const SoftFloatVariable significand = b.Variable(b.Constant(0, work));
    const SoftFloatVariable payload = b.Variable(b.Constant(0, work));

    const SoftFloatValue leftNaN = isNaN(left);
    b.IfElse(
        b.Or(leftNaN, isNaN(right)),
        [&] {
            // The first NaN, quieted, with its sign and payload.
            b.Set(classification, code(FloatClass::QuietNaN));
            b.Set(negative, b.Select(leftNaN, left.negative, right.negative));
            b.Set(payload, b.Select(leftNaN, left.nanPayload, right.nanPayload));
        },
        [&] {
            const SoftFloatValue leftZero = is(left, FloatClass::Zero);
            const SoftFloatValue rightZero = is(right, FloatClass::Zero);
            const SoftFloatValue leftInfinite = is(left, FloatClass::Infinity);
            const SoftFloatValue rightInfinite = is(right, FloatClass::Infinity);
            const SoftFloatValue invalid = b.Or(is(left, FloatClass::Invalid), is(right, FloatClass::Invalid));
            const SoftFloatValue zeroTimesInfinity =
                b.Or(b.And(leftZero, rightInfinite), b.And(leftInfinite, rightZero));
            b.IfElse(
                b.Or(invalid, zeroTimesInfinity),
                [&] {
                    // `PackFloat` encodes an invalid value as the format's quiet NaN with its sign: here positive.
                    b.Set(classification, code(FloatClass::Invalid));
                    b.Set(negative, b.Bool(false));
                },
                [&] {
                    b.IfElse(
                        b.Or(leftInfinite, rightInfinite), [&] { b.Set(classification, code(FloatClass::Infinity)); },
                        [&] {
                            b.If(b.Not(b.Or(leftZero, rightZero)), [&] {
                                b.Set(classification, code(FloatClass::Normal));
                                b.Set(exponent, b.Add(left.exponent, right.exponent));
                                b.Set(significand, ProductSignificand(b, format, left.significand, right.significand));
                            });
                        });
                });
        });

    const SoftFloatUnpacked result{b.Get(classification), b.Get(negative), b.Get(exponent), b.Get(significand),
                                   b.Get(payload)};
    b.Return(PackSoftFloat(b, format, result));
    b.Finish();
}
} // namespace Rux::HirToLirDetail
