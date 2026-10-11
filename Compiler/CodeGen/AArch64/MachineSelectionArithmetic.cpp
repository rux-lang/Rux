// Integer and floating-point arithmetic, comparisons and conversions.

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/MachineSelector.h"
#include "CodeGen/IntegerLiteral.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Rux::AArch64Machine::Detail {
using namespace Layout;

namespace {
[[nodiscard]] Op IntegerOp(const LirOpcode op) {
    switch (op) {
    case LirOpcode::Add:
        return Op::Add;
    case LirOpcode::Sub:
        return Op::Sub;
    case LirOpcode::Mul:
        return Op::Mul;
    case LirOpcode::And:
        return Op::And;
    case LirOpcode::Or:
        return Op::Or;
    default:
        return Op::Xor;
    }
}

[[nodiscard]] Op FloatOp(const LirOpcode op) {
    switch (op) {
    case LirOpcode::Add:
        return Op::AddFp;
    case LirOpcode::Sub:
        return Op::SubFp;
    case LirOpcode::Mul:
        return Op::MulFp;
    default:
        return Op::DivFp;
    }
}

[[nodiscard]] bool Commutative(const LirOpcode op) {
    return op == LirOpcode::Add || op == LirOpcode::Mul || op == LirOpcode::And || op == LirOpcode::Or ||
           op == LirOpcode::Xor;
}

[[nodiscard]] A64Condition IntegerCondition(const LirOpcode op, const bool isSigned) {
    switch (op) {
    case LirOpcode::CmpEq:
        return A64Condition::Eq;
    case LirOpcode::CmpNe:
        return A64Condition::Ne;
    case LirOpcode::CmpLt:
        return isSigned ? A64Condition::Lt : A64Condition::Cc;
    case LirOpcode::CmpLe:
        return isSigned ? A64Condition::Le : A64Condition::Ls;
    case LirOpcode::CmpGt:
        return isSigned ? A64Condition::Gt : A64Condition::Hi;
    default:
        return isSigned ? A64Condition::Ge : A64Condition::Cs;
    }
}

/// The condition that holds after the operands of a comparison are exchanged.
[[nodiscard]] A64Condition Swapped(const A64Condition condition) {
    switch (condition) {
    case A64Condition::Lt:
        return A64Condition::Gt;
    case A64Condition::Gt:
        return A64Condition::Lt;
    case A64Condition::Le:
        return A64Condition::Ge;
    case A64Condition::Ge:
        return A64Condition::Le;
    case A64Condition::Cc:
        return A64Condition::Hi;
    case A64Condition::Hi:
        return A64Condition::Cc;
    case A64Condition::Ls:
        return A64Condition::Cs;
    case A64Condition::Cs:
        return A64Condition::Ls;
    default:
        return condition;
    }
}

[[nodiscard]] std::uint64_t Truncate(const std::uint64_t value, const int width) {
    return width >= 8 ? value : value & ((std::uint64_t{1} << (width * 8)) - 1);
}

[[nodiscard]] std::uint64_t SignExtend(const std::uint64_t value, const int width) {
    if (width >= 8)
        return value;
    const int bits = width * 8;
    const std::uint64_t truncated = value & ((std::uint64_t{1} << bits) - 1);
    return (truncated >> (bits - 1)) != 0 ? truncated | ~((std::uint64_t{1} << bits) - 1) : truncated;
}
} // namespace

void Selector::SelectBinary(const LirInstr &instruction) {
    const Reg destination = Value(instruction.dst);
    LirReg left = instruction.srcs[0];
    LirReg right = instruction.srcs[1];
    if (KindOf(instruction.type) == ValueKind::Fpr) {
        Emit(FloatOp(instruction.op), static_cast<std::uint8_t>(Size(instruction.type)),
             {Operand::Def(destination), Operand::Use(Value(left)), Operand::Use(Value(right))});
        return;
    }
    const int width = ScalarSize(instruction.type) == 8 ? 8 : 4;
    if (Commutative(instruction.op) && integerConstants.contains(left) && !integerConstants.contains(right))
        std::swap(left, right);
    {
        // A 32-bit operation clears the upper half of its register; the operands' own zero bits bound the rest.
        const int leftZero = KnownZeroAbove(left);
        const int rightZero = KnownZeroAbove(right);
        int known = width == 4 ? 32 : 64;
        switch (instruction.op) {
        case LirOpcode::And:
            known = std::min({known, leftZero, rightZero});
            break;
        case LirOpcode::Or:
        case LirOpcode::Xor:
            known = std::min(known, std::max(leftZero, rightZero));
            break;
        case LirOpcode::Add:
            known = std::min(known, std::max(leftZero, rightZero) + 1);
            break;
        case LirOpcode::Mul:
            known = std::min(known, leftZero + rightZero);
            break;
        default:
            break;
        }
        NoteZeroAbove(instruction.dst, known);
    }
    const auto size = static_cast<std::uint8_t>(width);
    Op op = IntegerOp(instruction.op);
    if (const auto constant = integerConstants.find(right); constant != integerConstants.end()) {
        const std::uint64_t value = Truncate(constant->second, width);
        std::optional<std::int64_t> immediate;
        switch (instruction.op) {
        case LirOpcode::Add:
        case LirOpcode::Sub:
            if (TryEncodeArithImm12(value)) {
                immediate = static_cast<std::int64_t>(value);
            }
            else if (const auto negated = Truncate(0 - value, width); TryEncodeArithImm12(negated)) {
                // Adding the negation is subtracting, and the other way round.
                op = op == Op::Add ? Op::Sub : Op::Add;
                immediate = static_cast<std::int64_t>(negated);
            }
            break;
        case LirOpcode::And:
        case LirOpcode::Or:
        case LirOpcode::Xor:
            if (TryEncodeBitmaskImm(value, width == 8))
                immediate = static_cast<std::int64_t>(value);
            break;
        default:
            break;
        }
        if (immediate) {
            if (*immediate == 0 && (op == Op::Add || op == Op::Sub || op == Op::Or || op == Op::Xor)) {
                EmitCopy(destination, Value(left), RegClass::General);
                return;
            }
            Emit(op, size, {Operand::Def(destination), Operand::Use(Value(left)), Operand::Imm(*immediate)});
            return;
        }
    }
    Emit(op, size, {Operand::Def(destination), Operand::Use(Value(left)), Operand::Use(Value(right))});
}

namespace {
/// The multiplier and shift for signed division by `divisor` (Hacker's Delight, figure 10-1), for |divisor| >= 2.
std::pair<std::int64_t, int> SignedMagic(const std::int64_t divisor) {
    constexpr std::uint64_t Two63 = std::uint64_t{1} << 63;
    const std::uint64_t absolute =
        divisor < 0 ? 0 - static_cast<std::uint64_t>(divisor) : static_cast<std::uint64_t>(divisor);
    const std::uint64_t t = Two63 + (static_cast<std::uint64_t>(divisor) >> 63);
    const std::uint64_t anc = t - 1 - t % absolute;
    int p = 63;
    std::uint64_t q1 = Two63 / anc;
    std::uint64_t r1 = Two63 - q1 * anc;
    std::uint64_t q2 = Two63 / absolute;
    std::uint64_t r2 = Two63 - q2 * absolute;
    std::uint64_t delta = 0;
    do {
        ++p;
        q1 *= 2;
        r1 *= 2;
        if (r1 >= anc) {
            ++q1;
            r1 -= anc;
        }
        q2 *= 2;
        r2 *= 2;
        if (r2 >= absolute) {
            ++q2;
            r2 -= absolute;
        }
        delta = absolute - r2;
    }
    while (q1 < delta || (q1 == delta && r1 == 0));
    std::uint64_t magic = q2 + 1;
    if (divisor < 0)
        magic = 0 - magic;
    return {static_cast<std::int64_t>(magic), p - 64};
}
} // namespace

bool Selector::SelectSignedConstantDivision(const LirInstr &instruction) {
    // As on x86-64: a power of two adds d - 1 to a negative dividend and shifts; any other divisor takes the signed
    // high half of a magic multiple. A narrower value is sign-extended first; a remainder is n - q * d.
    const auto constant = integerConstants.find(instruction.srcs[1]);
    const int size = ScalarSize(instruction.type);
    if (constant == integerConstants.end() || !instruction.type.IsSigned() || !instruction.type.IsInteger() || size > 8)
        return false;
    const auto divisor = static_cast<std::int64_t>(SignExtend(constant->second, size));
    if (divisor == 0 || divisor == -1 || divisor == std::numeric_limits<std::int64_t>::min())
        return false;
    const bool remainder = instruction.op == LirOpcode::Mod;
    const Reg dividend = Extended(instruction.srcs[0]);
    const Reg destination = Value(instruction.dst);
    const Reg quotient = remainder ? NewGpr() : destination;
    const std::uint64_t absolute =
        divisor < 0 ? 0 - static_cast<std::uint64_t>(divisor) : static_cast<std::uint64_t>(divisor);
    if (divisor == 1) {
        EmitCopy(quotient, dividend, RegClass::General);
    }
    else if (std::has_single_bit(absolute)) {
        const int shift = std::countr_zero(absolute);
        const Reg sign = NewGpr();
        const Reg bias = NewGpr();
        const Reg biased = NewGpr();
        Emit(Op::Sar, 8, {Operand::Def(sign), Operand::Use(dividend), Operand::Imm(63)});
        Emit(Op::Shr, 8, {Operand::Def(bias), Operand::Use(sign), Operand::Imm(64 - shift)});
        Emit(Op::Add, 8, {Operand::Def(biased), Operand::Use(dividend), Operand::Use(bias)});
        if (divisor < 0) {
            const Reg shifted = NewGpr();
            Emit(Op::Sar, 8, {Operand::Def(shifted), Operand::Use(biased), Operand::Imm(shift)});
            Emit(Op::Neg, 8, {Operand::Def(quotient), Operand::Use(shifted)});
        }
        else {
            Emit(Op::Sar, 8, {Operand::Def(quotient), Operand::Use(biased), Operand::Imm(shift)});
        }
    }
    else {
        const auto [magic, shift] = SignedMagic(divisor);
        Reg high = NewGpr();
        Emit(Op::MulHighSigned, 8,
             {Operand::Def(high), Operand::Use(dividend), Operand::Use(Constant(static_cast<std::uint64_t>(magic)))});
        if ((divisor > 0 && magic < 0) || (divisor < 0 && magic > 0)) {
            const Reg corrected = NewGpr();
            Emit(divisor > 0 ? Op::Add : Op::Sub, 8,
                 {Operand::Def(corrected), Operand::Use(high), Operand::Use(dividend)});
            high = corrected;
        }
        if (shift > 0) {
            const Reg shifted = NewGpr();
            Emit(Op::Sar, 8, {Operand::Def(shifted), Operand::Use(high), Operand::Imm(shift)});
            high = shifted;
        }
        const Reg sign = NewGpr();
        Emit(Op::Shr, 8, {Operand::Def(sign), Operand::Use(high), Operand::Imm(63)});
        Emit(Op::Add, 8, {Operand::Def(quotient), Operand::Use(high), Operand::Use(sign)});
    }
    if (!remainder)
        return true;
    Emit(Op::MultiplySubtract, 8,
         {Operand::Def(destination), Operand::Use(quotient),
          Operand::Use(Constant(static_cast<std::uint64_t>(divisor))), Operand::Use(dividend)});
    return true;
}

bool Selector::SelectConstantDivision(const LirInstr &instruction) {
    // As on x86-64 (Granlund and Montgomery, figure 4.1): the quotient of any 64-bit n is
    // (t + ((n - t) >> 1)) >> (l - 1) where t is the high half of m * n. A power of two is a shift and a mask.
    const auto constant = integerConstants.find(instruction.srcs[1]);
    const int size = ScalarSize(instruction.type);
    if (constant == integerConstants.end() || instruction.type.IsSigned() || !instruction.type.IsInteger() || size > 8)
        return false;
    const std::uint64_t divisor = Truncate(constant->second, size);
    if (divisor == 0)
        return false;
    const bool remainder = instruction.op == LirOpcode::Mod;
    const Reg dividend = Extended(instruction.srcs[0]);
    const Reg destination = Value(instruction.dst);
    if (std::has_single_bit(divisor)) {
        const auto shift = std::countr_zero(divisor);
        if (remainder) {
            if (divisor == 1)
                Emit(Op::MovImm, 8, {Operand::Def(destination), Operand::Imm(0)});
            else if (TryEncodeBitmaskImm(divisor - 1, true))
                Emit(Op::And, 8,
                     {Operand::Def(destination), Operand::Use(dividend),
                      Operand::Imm(static_cast<std::int64_t>(divisor - 1))});
            else
                Emit(Op::And, 8,
                     {Operand::Def(destination), Operand::Use(dividend), Operand::Use(Constant(divisor - 1))});
        }
        else if (shift != 0) {
            Emit(Op::Shr, 8, {Operand::Def(destination), Operand::Use(dividend), Operand::Imm(shift)});
        }
        else {
            EmitCopy(destination, dividend, RegClass::General);
        }
        return true;
    }
    const int l = 64 - std::countl_zero(divisor - 1);
    std::uint64_t rest = (l == 64 ? 0 : std::uint64_t{1} << l) - divisor;
    std::uint64_t quotient64 = 0;
    for (int bit = 0; bit < 64; ++bit) {
        const bool carry = (rest >> 63) != 0;
        rest <<= 1;
        quotient64 <<= 1;
        if (carry || rest >= divisor) {
            rest -= divisor;
            quotient64 |= 1;
        }
    }
    if (quotient64 == ~std::uint64_t{0})
        return false;
    const Reg high = NewGpr();
    Emit(Op::MulHigh, 8, {Operand::Def(high), Operand::Use(dividend), Operand::Use(Constant(quotient64 + 1))});
    const Reg difference = NewGpr();
    const Reg half = NewGpr();
    const Reg sum = NewGpr();
    Emit(Op::Sub, 8, {Operand::Def(difference), Operand::Use(dividend), Operand::Use(high)});
    Emit(Op::Shr, 8, {Operand::Def(half), Operand::Use(difference), Operand::Imm(1)});
    const Reg quotient = remainder ? NewGpr() : destination;
    if (l > 1) {
        Emit(Op::Add, 8, {Operand::Def(sum), Operand::Use(half), Operand::Use(high)});
        Emit(Op::Shr, 8, {Operand::Def(quotient), Operand::Use(sum), Operand::Imm(l - 1)});
    }
    else {
        Emit(Op::Add, 8, {Operand::Def(quotient), Operand::Use(half), Operand::Use(high)});
    }
    if (!remainder)
        return true;
    Emit(Op::MultiplySubtract, 8,
         {Operand::Def(destination), Operand::Use(quotient), Operand::Use(Constant(divisor)), Operand::Use(dividend)});
    return true;
}

void Selector::SelectDivision(const LirInstr &instruction) {
    if (SelectConstantDivision(instruction) || SelectSignedConstantDivision(instruction))
        return;
    // Both operands at 64 bits, extended by the type's signedness, as the established emitter divides them; neither
    // division instruction traps.
    const bool isSigned = instruction.type.IsSigned();
    const Reg dividend = Extended(instruction.srcs[0]);
    const Reg divisor = Extended(instruction.srcs[1]);
    const Reg destination = Value(instruction.dst);
    const bool remainder = instruction.op == LirOpcode::Mod;
    const Reg quotient = remainder ? NewGpr() : destination;
    Emit(isSigned ? Op::Idiv : Op::Div, 8, {Operand::Def(quotient), Operand::Use(dividend), Operand::Use(divisor)});
    if (remainder)
        Emit(Op::MultiplySubtract, 8,
             {Operand::Def(destination), Operand::Use(quotient), Operand::Use(divisor), Operand::Use(dividend)});
}

void Selector::SelectShift(const LirInstr &instruction) {
    const Reg destination = Value(instruction.dst);
    if (instruction.op == LirOpcode::RotateRight) {
        const auto amount = ParseIntegerLiteralBits(instruction.strArg).value_or(0);
        const std::uint8_t size = instruction.type.kind == TypeRef::Kind::UInt64 ? 8 : 4;
        if (amount % (size * 8) != 0) {
            Emit(Op::Ror, size,
                 {Operand::Def(destination), Operand::Use(Value(instruction.srcs[0])),
                  Operand::Imm(static_cast<std::int64_t>(amount % (size * 8)))});
            if (size == 4)
                NoteZeroAbove(instruction.dst, 32);
        }
        else {
            EmitCopy(destination, Value(instruction.srcs[0]), RegClass::General);
        }
        return;
    }
    const Op op = instruction.op == LirOpcode::Shl                                ? Op::Shl
                : instruction.op == LirOpcode::Shr && instruction.type.IsSigned() ? Op::Sar
                                                                                  : Op::Shr;
    // A 32-bit value shifted by a constant below 32 is shifted in its W register.
    if (const auto constant = integerConstants.find(instruction.srcs[1]);
        constant != integerConstants.end() && ScalarSize(instruction.type) == 4 && (constant->second & 63U) < 32) {
        const auto count = static_cast<std::int64_t>(constant->second & 63U);
        if (count == 0)
            EmitCopy(destination, Value(instruction.srcs[0]), RegClass::General);
        else
            Emit(op, 4, {Operand::Def(destination), Operand::Use(Value(instruction.srcs[0])), Operand::Imm(count)});
        int known = 32;
        if (op == Op::Shr)
            known = static_cast<int>(32 - count);
        else if (op == Op::Shl)
            known = std::min(32, KnownZeroAbove(instruction.srcs[0]) + static_cast<int>(count));
        NoteZeroAbove(instruction.dst, known);
        return;
    }
    // Otherwise the shift is 64-bit with the count taken modulo 64, reading the value extended by its signedness (or
    // as unsigned for `lshr`), which is what every reader of a narrow result expects.
    const Reg value = instruction.op == LirOpcode::Shl
                        ? Value(instruction.srcs[0])
                        : Extended(instruction.srcs[0], false, instruction.op == LirOpcode::Lshr);
    if (const auto constant = integerConstants.find(instruction.srcs[1]); constant != integerConstants.end()) {
        const auto count = static_cast<std::int64_t>(constant->second & 63U);
        if (count == 0)
            EmitCopy(destination, value, RegClass::General);
        else
            Emit(op, 8, {Operand::Def(destination), Operand::Use(value), Operand::Imm(count)});
        const int sourceZero =
            op == Op::Shl ? KnownZeroAbove(instruction.srcs[0])
                          : (op == Op::Shr ? std::min(KnownZeroAbove(instruction.srcs[0]),
                                                      instruction.op == LirOpcode::Lshr || !instruction.type.IsSigned()
                                                          ? ScalarSize(instruction.type) * 8
                                                          : 64)
                                           : 64);
        if (op == Op::Shr)
            NoteZeroAbove(instruction.dst, std::max(0, sourceZero - static_cast<int>(count)));
        else if (op == Op::Shl)
            NoteZeroAbove(instruction.dst, sourceZero + static_cast<int>(count));
        return;
    }
    Emit(op, 8, {Operand::Def(destination), Operand::Use(value), Operand::Use(Value(instruction.srcs[1]))});
}

void Selector::SelectUnary(const LirInstr &instruction) {
    const Reg destination = Value(instruction.dst);
    const LirReg source = instruction.srcs[0];
    switch (instruction.op) {
    case LirOpcode::Neg:
        if (KindOf(instruction.type) == ValueKind::Fpr)
            Emit(Op::NegFp, static_cast<std::uint8_t>(Size(instruction.type)),
                 {Operand::Def(destination), Operand::Use(Value(source))});
        else
            Emit(Op::Neg, 8, {Operand::Def(destination), Operand::Use(Value(source))});
        return;
    case LirOpcode::Not: {
        const int size = ScalarSize(instruction.type);
        Emit(Op::Cmp, size == 8 ? 8 : 4,
             {Operand::Use(size < 4 ? Extended(source, false, true) : Value(source)), Operand::Imm(0)});
        Emit(Op::CSet, 4, {Operand::Def(destination)}).condition = static_cast<std::uint8_t>(A64Condition::Eq);
        NoteZeroAbove(instruction.dst, 1);
        return;
    }
    case LirOpcode::BitNot:
        if (instruction.type.IsBool())
            Emit(Op::Xor, 4, {Operand::Def(destination), Operand::Use(Value(source)), Operand::Imm(1)});
        else
            Emit(Op::Not, 8, {Operand::Def(destination), Operand::Use(Value(source))});
        return;
    case LirOpcode::Sqrt:
        Emit(Op::SqrtFp, static_cast<std::uint8_t>(Size(instruction.type)),
             {Operand::Def(destination), Operand::Use(Value(source))});
        return;
    default:
        return;
    }
}

A64Condition Selector::EmitCompare(const LirInstr &comparison) {
    LirReg left = comparison.srcs[0];
    LirReg right = comparison.srcs[1];
    const TypeRef &type = TypeOf(left);
    if (KindOf(type) == ValueKind::Fpr) {
        // After FCMP an unordered pair sets C and V: MI, LS, GT and GE are all false then, and only NE is true.
        A64Condition condition = A64Condition::Eq;
        switch (comparison.op) {
        case LirOpcode::CmpEq:
            condition = A64Condition::Eq;
            break;
        case LirOpcode::CmpNe:
            condition = A64Condition::Ne;
            break;
        case LirOpcode::CmpLt:
            condition = A64Condition::Mi;
            break;
        case LirOpcode::CmpLe:
            condition = A64Condition::Ls;
            break;
        case LirOpcode::CmpGt:
            condition = A64Condition::Gt;
            break;
        default:
            condition = A64Condition::Ge;
            break;
        }
        Emit(Op::CompareFp, static_cast<std::uint8_t>(Size(type)),
             {Operand::Use(Value(left)), Operand::Use(Value(right))});
        return condition;
    }
    // Both operands are read at the left operand's type. Narrower than a word they are extended by its signedness and
    // compared as 32-bit values.
    const int width = ScalarSize(type);
    const bool isSigned = type.IsSigned();
    A64Condition condition = IntegerCondition(comparison.op, isSigned);
    if (integerConstants.contains(left) && !integerConstants.contains(right)) {
        std::swap(left, right);
        condition = Swapped(condition);
    }
    const auto size = static_cast<std::uint8_t>(width == 8 ? 8 : 4);
    const auto operand = [&](const LirReg reg) { return width < 4 ? Extended(reg, isSigned, !isSigned) : Value(reg); };
    const Reg value = operand(left);
    if (const auto constant = integerConstants.find(right); constant != integerConstants.end()) {
        // The constant as the compared register holds it: extended like the operand, then cut to the comparison's
        // width.
        std::uint64_t bits = isSigned ? SignExtend(constant->second, width) : Truncate(constant->second, width);
        bits = Truncate(bits, size);
        const std::uint64_t negated = Truncate(0 - bits, size);
        if (TryEncodeArithImm12(bits)) {
            Emit(Op::Cmp, size, {Operand::Use(value), Operand::Imm(static_cast<std::int64_t>(bits))});
            return condition;
        }
        if (TryEncodeArithImm12(negated)) {
            Emit(Op::Cmp, size, {Operand::Use(value), Operand::Imm(-static_cast<std::int64_t>(negated))});
            return condition;
        }
    }
    Emit(Op::Cmp, size, {Operand::Use(value), Operand::Use(operand(right))});
    return condition;
}

void Selector::SelectComparison(const LirInstr &instruction) {
    const A64Condition condition = EmitCompare(instruction);
    Emit(Op::CSet, 4, {Operand::Def(Value(instruction.dst))}).condition = static_cast<std::uint8_t>(condition);
    NoteZeroAbove(instruction.dst, 1);
}

void Selector::SelectCast(const LirInstr &instruction) {
    const LirReg source = instruction.srcs[0];
    const TypeRef &from = TypeOf(source);
    const TypeRef &to = instruction.type;
    const Reg destination = Value(instruction.dst);
    const ValueKind sourceKind = KindOf(from);
    const ValueKind destinationKind = KindOf(to);
    if (sourceKind == ValueKind::Gpr && destinationKind == ValueKind::Gpr) {
        const int sourceSize = ScalarSize(from);
        const int destinationSize = ScalarSize(to);
        const bool isSigned = from.IsSigned();
        int known = KnownZeroAbove(source);
        Reg value = Value(source);
        if (destinationSize > sourceSize && !from.IsBool() && !AlreadyExtended(source, sourceSize, isSigned)) {
            value = ExtendRegister(value, sourceSize, isSigned);
            if (!isSigned)
                known = sourceSize * 8;
        }
        if (to.IsBool() && !from.IsBool() && destinationSize < 8 && known > destinationSize * 8) {
            Emit(Op::ZeroExtend, 8, {Operand::Def(destination), Operand::Use(value)}).sourceSize =
                static_cast<std::uint8_t>(destinationSize);
            known = destinationSize * 8;
        }
        else {
            EmitCopy(destination, value, RegClass::General);
        }
        NoteZeroAbove(instruction.dst, known);
        return;
    }
    if (sourceKind == ValueKind::Gpr) {
        const bool isSigned = from.IsSigned() && !from.IsBool();
        Emit(isSigned ? Op::IntToFp : Op::UIntToFp, static_cast<std::uint8_t>(Size(to)),
             {Operand::Def(destination), Operand::Use(Extended(source))})
            .sourceSize = 8;
        return;
    }
    if (destinationKind == ValueKind::Gpr) {
        const bool unsignedWide = to.IsInteger() && !to.IsSigned() && SizeOf(to) == 8;
        Emit(unsignedWide ? Op::FpToUInt : Op::FpToInt, 8, {Operand::Def(destination), Operand::Use(Value(source))})
            .sourceSize = static_cast<std::uint8_t>(Size(from));
        return;
    }
    const int sourceSize = Size(from);
    const int destinationSize = Size(to);
    if (sourceSize == destinationSize) {
        EmitCopy(destination, Value(source), RegClass::Vector);
        return;
    }
    Emit(Op::FpToFp, static_cast<std::uint8_t>(destinationSize),
         {Operand::Def(destination), Operand::Use(Value(source))})
        .sourceSize = static_cast<std::uint8_t>(sourceSize);
}
} // namespace Rux::AArch64Machine::Detail
