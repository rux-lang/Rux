// Integer overflow and narrowing checks. A `+`, `-` or `*` of a type no wider than a machine word becomes one checked
// instruction the back ends test with the flags; a wider type has its check written out here. A check whose operands'
// spelling already bounds the result, as in `(a as uint128) * (b as uint128)`, is not emitted at all.

#include "Lowering/HirToLir/HirToLirContext.h"
#include "Numeric/IntegerLiteral.h"
#include "Numeric/WideInteger.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Rux::HirToLirDetail {

namespace {
/// The largest value of an integer type `bits` wide, as a literal a wider or equal `const` accepts.
std::string IntegerMaximumLiteral(const std::uint64_t bits, const bool isSigned) {
    const std::uint64_t valueBits = isSigned ? bits - 1 : bits;
    if (valueBits <= 64) {
        return std::to_string(valueBits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << valueBits) - 1);
    }
    return (isSigned ? "0x7" : "0xF") + std::string(static_cast<std::size_t>(bits / 4 - 1), 'F');
}

/// The unsigned integer type `bits` wide.
[[nodiscard]] TypeRef UnsignedOfWidth(const std::uint64_t bits) {
    switch (bits) {
    case 8:
        return TypeRef::MakePrimitive(TypeRef::Kind::UInt8);
    case 16:
        return TypeRef::MakePrimitive(TypeRef::Kind::UInt16);
    case 32:
        return TypeRef::MakePrimitive(TypeRef::Kind::UInt32);
    case 64:
        return TypeRef::MakePrimitive(TypeRef::Kind::UInt64);
    case 128:
        return TypeRef::MakePrimitive(TypeRef::Kind::UInt128);
    case 256:
        return TypeRef::MakePrimitive(TypeRef::Kind::UInt256);
    default:
        return TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    }
}

constexpr std::uint32_t BoundBits = WideInteger::MaxBits;

[[nodiscard]] std::uint32_t BitsOf(const TypeRef &type) {
    return static_cast<std::uint32_t>(type.SizeInBytes().value_or(8) * 8);
}

[[nodiscard]] WideInteger TypeMaximum(const TypeRef &type) {
    return WideInteger::MaxValue(BitsOf(type), type.IsSigned()).Extended(BoundBits, false);
}

[[nodiscard]] WideInteger Smaller(const WideInteger &left, const WideInteger &right) {
    return left < right ? left : right;
}

/// The exact sum, or every bit set when it needs more than the bound's width, which no type's maximum reaches.
[[nodiscard]] WideInteger SumBound(const WideInteger &left, const WideInteger &right) {
    const WideInteger sum = left.Added(right);
    return sum < left ? WideInteger::AllOnes(BoundBits) : sum;
}

[[nodiscard]] WideInteger ProductBound(const WideInteger &left, const WideInteger &right) {
    const WideIntegerProduct product = left.MultipliedFull(right);
    return product.high.IsZero() ? product.low : WideInteger::AllOnes(BoundBits);
}

/// The count of a shift written as a literal smaller than the shifted type's width.
[[nodiscard]] std::optional<std::uint32_t> LiteralShift(const HirBinaryExpr &shift) {
    const auto *count = HirCast<const HirLiteralExpr>(shift.right.get());
    const std::optional<WideInteger> amount = count ? DecodeIntegerLiteral(count->value, BoundBits) : std::nullopt;
    const std::optional<std::uint64_t> value = amount ? amount->ToUnsigned() : std::nullopt;
    if (!value || *value >= BitsOf(shift.type)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

/// The largest value `expression` can have, when its spelling proves it is never negative: a literal, a widening of an
/// unsigned value, and what `+`, `*`, `/`, `%`, `&` and `>>` make of such operands. Anything read from a variable is
/// bounded only by its type, and only when the type is unsigned.
std::optional<WideInteger> UpperBound(const HirExpr &expression) {
    const TypeRef &type = expression.type;
    if (!type.IsInteger()) {
        return std::nullopt;
    }
    const bool isSigned = type.IsSigned();
    const std::optional<WideInteger> ofType = isSigned ? std::nullopt : std::optional<WideInteger>(TypeMaximum(type));
    if (const auto *literal = HirCast<const HirLiteralExpr>(&expression)) {
        const auto parts = SplitIntegerLiteral(literal->value);
        if (!parts || parts->negative) {
            return ofType;
        }
        const std::optional<WideInteger> value = DecodeIntegerLiteral(literal->value, BoundBits);
        return value ? std::optional<WideInteger>(Smaller(*value, TypeMaximum(type))) : ofType;
    }
    if (const auto *cast = HirCast<const HirCastExpr>(&expression)) {
        if (!cast->operand || !cast->operand->type.IsInteger()) {
            return ofType;
        }
        const std::optional<WideInteger> source = UpperBound(*cast->operand);
        if (!source) {
            return ofType;
        }
        // A value the target holds is kept; one it does not stops the program or keeps the low bits, which an
        // unsigned target bounds by its maximum and a signed one may read as negative.
        if (*source <= TypeMaximum(type)) {
            return source;
        }
        return cast->checksRange || !isSigned ? std::optional<WideInteger>(TypeMaximum(type)) : std::nullopt;
    }
    const auto *binary = HirCast<const HirBinaryExpr>(&expression);
    if (!binary || !binary->left || !binary->right) {
        return ofType;
    }
    const std::optional<WideInteger> left = UpperBound(*binary->left);
    const std::optional<WideInteger> right = UpperBound(*binary->right);
    const WideInteger maximum = TypeMaximum(type);
    switch (binary->op) {
    case TokenKind::Plus:
    case TokenKind::Star:
        // A checked result past the maximum stops the program, so the maximum bounds what continues.
        if (left && right) {
            return Smaller(binary->op == TokenKind::Plus ? SumBound(*left, *right) : ProductBound(*left, *right),
                           maximum);
        }
        return ofType;
    case TokenKind::PlusPercent:
    case TokenKind::StarPercent:
        if (left && right) {
            const WideInteger exact =
                binary->op == TokenKind::PlusPercent ? SumBound(*left, *right) : ProductBound(*left, *right);
            if (exact <= maximum) {
                return exact;
            }
        }
        return ofType;
    case TokenKind::Minus:
        // An unsigned difference that did not stop is at most its left operand.
        return isSigned ? std::nullopt : left;
    case TokenKind::Slash:
        return left && right ? left : ofType;
    case TokenKind::Percent:
        if (left && right) {
            return Smaller(*left, *right);
        }
        return ofType;
    case TokenKind::Amp:
        // Clearing bits of a value never negative leaves it never negative and no larger.
        if (left && right) {
            return Smaller(*left, *right);
        }
        return left ? left : right ? right : ofType;
    case TokenKind::GreaterGreater: {
        if (!left) {
            return ofType;
        }
        const std::optional<std::uint32_t> shift = LiteralShift(*binary);
        return shift ? left->ShiftedRight(*shift, false) : left;
    }
    case TokenKind::LessLess: {
        // A shift that moves no set bit past the type's maximum is a multiplication by a power of two.
        const std::optional<std::uint32_t> shift = LiteralShift(*binary);
        if (left && shift && left->CountLeadingZeros() >= *shift) {
            const WideInteger shifted = left->ShiftedLeft(*shift);
            if (shifted <= maximum) {
                return shifted;
            }
        }
        return ofType;
    }
    default:
        return ofType;
    }
}

/// The smallest value `expression` can have, under the same rules as `UpperBound`; zero for an unsigned value its
/// spelling says nothing about.
std::optional<WideInteger> LowerBound(const HirExpr &expression) {
    const TypeRef &type = expression.type;
    if (!type.IsInteger()) {
        return std::nullopt;
    }
    const std::optional<WideInteger> ofType =
        type.IsSigned() ? std::nullopt : std::optional<WideInteger>(WideInteger::Zero(BoundBits));
    if (HirCast<const HirLiteralExpr>(&expression)) {
        // A literal is its own bound, from below as from above.
        return UpperBound(expression);
    }
    if (const auto *cast = HirCast<const HirCastExpr>(&expression)) {
        const std::optional<WideInteger> upper = cast->operand ? UpperBound(*cast->operand) : std::nullopt;
        if (upper && *upper <= TypeMaximum(type)) {
            const std::optional<WideInteger> lower = LowerBound(*cast->operand);
            return lower ? lower : ofType;
        }
        return ofType;
    }
    const auto *binary = HirCast<const HirBinaryExpr>(&expression);
    if (!binary || !binary->left || !binary->right) {
        return ofType;
    }
    // Each rule needs the exact result to fit the type, which the upper bounds settle; a sum or product then never
    // falls below the sum or product of the operands' smallest values.
    const std::optional<WideInteger> upper = UpperBound(expression);
    const std::optional<WideInteger> left = LowerBound(*binary->left);
    const std::optional<WideInteger> right = LowerBound(*binary->right);
    const std::optional<WideInteger> leftUpper = UpperBound(*binary->left);
    const std::optional<WideInteger> rightUpper = UpperBound(*binary->right);
    if (!upper || !left || !right || !leftUpper || !rightUpper) {
        return ofType;
    }
    const WideInteger maximum = TypeMaximum(type);
    switch (binary->op) {
    case TokenKind::Plus:
    case TokenKind::PlusPercent:
        return SumBound(*leftUpper, *rightUpper) <= maximum ? std::optional<WideInteger>(SumBound(*left, *right))
                                                            : ofType;
    case TokenKind::Star:
    case TokenKind::StarPercent:
        return ProductBound(*leftUpper, *rightUpper) <= maximum
                 ? std::optional<WideInteger>(ProductBound(*left, *right))
                 : ofType;
    case TokenKind::LessLess: {
        const std::optional<std::uint32_t> shift = LiteralShift(*binary);
        if (shift && leftUpper->CountLeadingZeros() >= *shift && leftUpper->ShiftedLeft(*shift) <= maximum) {
            return left->ShiftedLeft(*shift);
        }
        return ofType;
    }
    default:
        return ofType;
    }
}
} // namespace

std::string SignedMinimumLiteral(const TypeRef &type) {
    const std::uint64_t bits = type.SizeInBytes().value_or(8) * 8;
    if (bits <= 64) {
        return "-" + std::to_string(std::uint64_t{1} << (bits - 1));
    }
    return "-0x8" + std::string(static_cast<std::size_t>(bits / 4 - 1), '0');
}

bool HirToLirContext::ResultAlwaysFits(const HirBinaryExpr &expression) {
    if (expression.op == TokenKind::Minus) {
        // A difference never negative needs no check of its own, for either signedness: the smallest left operand is
        // at least the largest right one, and both are never negative.
        const std::optional<WideInteger> left = LowerBound(*expression.left);
        const std::optional<WideInteger> right = UpperBound(*expression.right);
        return left && right && *right <= *left;
    }
    if (expression.op != TokenKind::Plus && expression.op != TokenKind::Star) {
        return false;
    }
    const std::optional<WideInteger> left = UpperBound(*expression.left);
    const std::optional<WideInteger> right = UpperBound(*expression.right);
    if (!left || !right) {
        return false;
    }
    const WideInteger exact = expression.op == TokenKind::Plus ? SumBound(*left, *right) : ProductBound(*left, *right);
    return exact <= TypeMaximum(expression.type);
}

bool HirToLirContext::CastAlwaysFits(const HirExpr &operand, const TypeRef &target) {
    const std::optional<WideInteger> bound = UpperBound(operand);
    return bound && target.IsInteger() && *bound <= TypeMaximum(target);
}

LirReg HirToLirContext::EmitOverflowChecked(const LirOpcode op, const LirReg left, const LirReg right,
                                            const TypeRef &type, const std::string_view spelled,
                                            const SourceLocation &location) {
    const std::string message = std::format("integer overflow in '{}'", spelled);
    const std::uint64_t bytes = type.SizeInBytes().value_or(8);
    if (bytes <= 8) {
        const LirReg result = NewReg();
        LirInstr checked;
        checked.dst = result;
        checked.op = op == LirOpcode::Add ? LirOpcode::CheckedAdd
                   : op == LirOpcode::Sub ? LirOpcode::CheckedSub
                                          : LirOpcode::CheckedMul;
        checked.type = type;
        checked.srcs = {left, right};
        checked.strArg = message;
        checked.source.Edit() = {.file = currentSourceFile,
                                 .function = currentSourceFunction,
                                 .line = location.line,
                                 .column = location.column};
        Emit(std::move(checked));
        return result;
    }
    // A type wider than a machine word has no single flag to test, so the check is written out: a sum or difference
    // overflowed when its sign disagrees with what the operands' signs allow, and a product is checked through its
    // halves.
    if (op == LirOpcode::Mul) {
        return EmitWideProductChecked(left, right, type, message, location);
    }
    const TypeRef flag = TypeRef::MakeBool();
    const LirReg result = EmitBinary(op, left, right, type);
    LirReg fits = LirNoReg;
    if (type.IsSigned()) {
        const LirReg disagree = op == LirOpcode::Add
                                  ? EmitBinary(LirOpcode::And, EmitBinary(LirOpcode::Xor, left, result, type),
                                               EmitBinary(LirOpcode::Xor, right, result, type), type)
                                  : EmitBinary(LirOpcode::And, EmitBinary(LirOpcode::Xor, left, right, type),
                                               EmitBinary(LirOpcode::Xor, left, result, type), type);
        fits = EmitBinary(LirOpcode::CmpGe, disagree, EmitConst("0", type), flag);
    }
    else {
        fits = op == LirOpcode::Add ? EmitBinary(LirOpcode::CmpGe, result, left, flag)
                                    : EmitBinary(LirOpcode::CmpGe, left, right, flag);
    }
    EmitTrapUnless(fits, message, location);
    return result;
}

LirReg HirToLirContext::EmitWideProductChecked(const LirReg left, const LirReg right, const TypeRef &type,
                                               const std::string &message, const SourceLocation &location) {
    // Dividing the product back would cost a wide division on every multiplication, so the product is assembled from
    // the operands' halves instead, where each partial product fits the type exactly. With `a = ah·2^h + al` and the
    // same for `b`, the product fits only when at most one high half is nonzero, the cross term `ah·bl + bh·al` fits
    // a half, and adding the shifted cross term to `al·bl` does not carry. A signed product is checked as the product
    // of the magnitudes against the limit its sign allows, which is one larger for a negative product.
    const TypeRef flag = TypeRef::MakeBool();
    const std::uint64_t bits = BitsOf(type);
    const bool isSigned = type.IsSigned();
    const TypeRef magnitudeType = TypeRef::MakePrimitive(bits <= 128   ? TypeRef::Kind::UInt128
                                                         : bits <= 256 ? TypeRef::Kind::UInt256
                                                                       : TypeRef::Kind::UInt512);
    LirReg a = left;
    LirReg b = right;
    LirReg sign = LirNoReg;
    if (isSigned) {
        const LirReg top = EmitConst(std::to_string(bits - 1), type);
        const LirReg leftSign = EmitBinary(LirOpcode::Shr, left, top, type);
        const LirReg rightSign = EmitBinary(LirOpcode::Shr, right, top, type);
        const auto magnitude = [&](const LirReg value, const LirReg valueSign) {
            const LirReg flipped = EmitBinary(LirOpcode::Xor, value, valueSign, type);
            return EmitConversion(EmitBinary(LirOpcode::Sub, flipped, valueSign, type), type, magnitudeType);
        };
        a = magnitude(left, leftSign);
        b = magnitude(right, rightSign);
        sign = EmitConversion(EmitBinary(LirOpcode::Xor, leftSign, rightSign, type), type, magnitudeType);
    }
    const LirReg zero = EmitConst("0", magnitudeType);
    const LirReg half = EmitConst(std::to_string(bits / 2), magnitudeType);
    const LirReg mask = EmitConst(IntegerMaximumLiteral(bits / 2, false), magnitudeType);
    const LirReg leftHigh = EmitBinary(LirOpcode::Shr, a, half, magnitudeType);
    const LirReg rightHigh = EmitBinary(LirOpcode::Shr, b, half, magnitudeType);
    const LirReg leftLow = EmitBinary(LirOpcode::And, a, mask, magnitudeType);
    const LirReg rightLow = EmitBinary(LirOpcode::And, b, mask, magnitudeType);
    const LirReg oneHighHalf = EmitBinary(LirOpcode::Or, EmitBinary(LirOpcode::CmpEq, leftHigh, zero, flag),
                                          EmitBinary(LirOpcode::CmpEq, rightHigh, zero, flag), flag);
    const LirReg cross = EmitBinary(LirOpcode::Add, EmitBinary(LirOpcode::Mul, leftHigh, rightLow, magnitudeType),
                                    EmitBinary(LirOpcode::Mul, rightHigh, leftLow, magnitudeType), magnitudeType);
    const LirReg crossFits =
        EmitBinary(LirOpcode::CmpEq, EmitBinary(LirOpcode::Shr, cross, half, magnitudeType), zero, flag);
    const LirReg low = EmitBinary(LirOpcode::Mul, leftLow, rightLow, magnitudeType);
    const LirReg product =
        EmitBinary(LirOpcode::Add, EmitBinary(LirOpcode::Shl, cross, half, magnitudeType), low, magnitudeType);
    LirReg fits = EmitBinary(LirOpcode::And, EmitBinary(LirOpcode::And, oneHighHalf, crossFits, flag),
                             EmitBinary(LirOpcode::CmpGe, product, low, flag), flag);
    if (!isSigned) {
        EmitTrapUnless(fits, message, location);
        return product;
    }
    // The sign is all ones for a negative product, so subtracting it raises the limit by one and the exclusive-or and
    // subtraction negate the magnitude.
    const LirReg limit =
        EmitBinary(LirOpcode::Sub, EmitConst(IntegerMaximumLiteral(bits, true), magnitudeType), sign, magnitudeType);
    fits = EmitBinary(LirOpcode::And, fits, EmitBinary(LirOpcode::CmpLe, product, limit, flag), flag);
    EmitTrapUnless(fits, message, location);
    const LirReg signedMagnitude = EmitBinary(LirOpcode::Xor, product, sign, magnitudeType);
    return EmitConversion(EmitBinary(LirOpcode::Sub, signedMagnitude, sign, magnitudeType), magnitudeType, type);
}

void HirToLirContext::EmitIntegerCastCheck(const LirReg value, const TypeRef &from, const TypeRef &to,
                                           const SourceLocation &location) {
    const std::optional<std::uint64_t> fromBytes = from.SizeInBytes();
    const std::optional<std::uint64_t> toBytes = to.SizeInBytes();
    if (!fromBytes || !toBytes) {
        return;
    }
    const std::uint64_t fromBits = *fromBytes * 8;
    const std::uint64_t toBits = *toBytes * 8;
    const bool fromSigned = from.IsSigned();
    const bool toSigned = to.IsSigned();
    // Every value of the source is a value of the target: no check.
    if ((fromSigned == toSigned && toBits >= fromBits) || (!fromSigned && toSigned && toBits > fromBits)) {
        return;
    }
    const TypeRef flag = TypeRef::MakeBool();
    LirReg fits = LirNoReg;
    if (!fromSigned) {
        // An unsigned value only has a top to pass.
        fits = EmitBinary(LirOpcode::CmpLe, value, EmitConst(IntegerMaximumLiteral(toBits, toSigned), from), flag);
    }
    else if (toBits >= fromBits) {
        // A signed value into an unsigned type at least as wide only has to be non-negative.
        fits = EmitBinary(LirOpcode::CmpGe, value, EmitConst("0", from), flag);
    }
    else {
        // A range on both sides is one unsigned comparison: a negative value read as unsigned is above any maximum, and
        // a signed target's range is first shifted to start at zero.
        const TypeRef unsignedFrom = UnsignedOfWidth(fromBits);
        LirReg shifted = value;
        if (toSigned) {
            // Adding the magnitude of the target's minimum, which the wider source holds.
            shifted = EmitBinary(LirOpcode::Add, value, EmitConst(SignedMinimumLiteral(to).substr(1), from), from);
        }
        fits = EmitBinary(LirOpcode::CmpLe, EmitConversion(shifted, from, unsignedFrom),
                          EmitConst(IntegerMaximumLiteral(toBits, false), unsignedFrom), flag);
    }
    EmitTrapUnless(fits, "integer cast out of range", location);
}
} // namespace Rux::HirToLirDetail
