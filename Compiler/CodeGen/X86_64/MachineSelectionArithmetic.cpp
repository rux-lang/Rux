// Integer and floating-point arithmetic, comparisons and conversions.

#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/X86_64/MachineSelector.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Rux::X86_64Machine::Detail {
using namespace Layout;

namespace {
[[nodiscard]] Op IntegerOp(const LirOpcode op) {
    switch (op) {
    case LirOpcode::Add:
        return Op::Add;
    case LirOpcode::Sub:
        return Op::Sub;
    case LirOpcode::Mul:
        return Op::Imul;
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

[[nodiscard]] Cond IntegerCondition(const LirOpcode op, const bool isSigned) {
    switch (op) {
    case LirOpcode::CmpEq:
        return Cond::Equal;
    case LirOpcode::CmpNe:
        return Cond::NotEqual;
    case LirOpcode::CmpLt:
        return isSigned ? Cond::Less : Cond::Below;
    case LirOpcode::CmpLe:
        return isSigned ? Cond::LessEqual : Cond::BelowEqual;
    case LirOpcode::CmpGt:
        return isSigned ? Cond::Greater : Cond::Above;
    default:
        return isSigned ? Cond::GreaterEqual : Cond::AboveEqual;
    }
}
} // namespace

void Selector::SelectBinary(const LirInstr &instruction) {
    const Reg destination = Value(instruction.dst);
    LirReg left = instruction.srcs[0];
    LirReg right = instruction.srcs[1];
    if (KindOf(instruction.type) == ValueKind::Xmm) {
        if (Commutative(instruction.op) && !IsKilledAt(instruction, left) && IsKilledAt(instruction, right))
            std::swap(left, right);
        const auto size = static_cast<std::uint8_t>(Size(instruction.type));
        EmitCopy(destination, Value(left), RegClass::Vector);
        Emit(FloatOp(instruction.op), size, {Operand::UseDef(destination), Operand::Use(Value(right))});
        return;
    }
    const int width = ScalarSize(instruction.type) == 8 ? 8 : 4;
    if (Commutative(instruction.op) && integerConstants.contains(left) && !integerConstants.contains(right))
        std::swap(left, right);
    const auto immediate = Immediate(right, width);
    const auto size = static_cast<std::uint8_t>(width);
    {
        // A 32-bit operation clears the upper half of its register; the operands' own zero bits bound the rest.
        const int leftZero = KnownZeroAbove(left);
        const int rightZero =
            immediate
                ? (*immediate >= 0 ? static_cast<int>(std::bit_width(static_cast<std::uint64_t>(*immediate))) : 64)
                : KnownZeroAbove(right);
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
    if (instruction.op == LirOpcode::Mul && immediate) {
        Emit(Op::ImulImm, size, {Operand::Def(destination), Operand::Use(Value(left)), Operand::Imm(*immediate)});
        return;
    }
    const bool leftLives = !IsKilledAt(instruction, left);
    // A three-operand add keeps both inputs, which a two-address add would only manage with an extra copy.
    if (instruction.op == LirOpcode::Add && leftLives && (immediate || !IsKilledAt(instruction, right))) {
        Address address;
        address.base = Value(left);
        if (immediate)
            address.displacement = static_cast<std::int32_t>(*immediate);
        else
            address.index = Value(right);
        Emit(Op::Lea, size, {Operand::Def(destination), Operand::Mem(address)});
        return;
    }
    if (instruction.op == LirOpcode::Sub && leftLives && immediate &&
        *immediate != std::numeric_limits<std::int32_t>::min()) {
        Address address;
        address.base = Value(left);
        address.displacement = static_cast<std::int32_t>(-*immediate);
        Emit(Op::Lea, size, {Operand::Def(destination), Operand::Mem(address)});
        return;
    }
    if (!immediate && Commutative(instruction.op) && leftLives && IsKilledAt(instruction, right))
        std::swap(left, right);
    EmitCopy(destination, Value(left), RegClass::General);
    Emit(IntegerOp(instruction.op), size,
         {Operand::UseDef(destination), immediate ? Operand::Imm(*immediate) : Operand::Use(Value(right))});
}

void Selector::SelectDivision(const LirInstr &instruction) {
    const bool isSigned = instruction.type.IsSigned();
    const Reg dividend = Extended(instruction.srcs[0]);
    const Reg divisor = Extended(instruction.srcs[1]);
    EmitCopy(Rax, dividend, RegClass::General);
    if (isSigned) {
        auto &extend = Emit(Op::SignExtendAccumulator, 8, {});
        extend.implicitUses = Machine::RegisterBit(Rax);
        extend.implicitDefs = Machine::RegisterBit(Rdx);
    }
    else {
        Emit(Op::MovImm, 8, {Operand::Def(Rdx), Operand::Imm(0)});
    }
    auto &divide = Emit(isSigned ? Op::Idiv : Op::Div, 8, {Operand::Use(divisor)});
    divide.implicitUses = Machine::RegisterBit(Rax) | Machine::RegisterBit(Rdx);
    divide.implicitDefs = divide.implicitUses;
    EmitCopy(Value(instruction.dst), instruction.op == LirOpcode::Mod ? Rdx : Rax, RegClass::General);
}

void Selector::SelectShift(const LirInstr &instruction) {
    const Reg destination = Value(instruction.dst);
    if (instruction.op == LirOpcode::RotateRight) {
        const auto amount = ParseIntegerLiteralBits(instruction.strArg).value_or(0);
        const std::uint8_t size = instruction.type.kind == TypeRef::Kind::UInt64 ? 8 : 4;
        EmitCopy(destination, Value(instruction.srcs[0]), RegClass::General);
        if (amount % (size * 8) != 0) {
            Emit(Op::Ror, size, {Operand::UseDef(destination), Operand::Imm(static_cast<std::int64_t>(amount))});
            if (size == 4)
                NoteZeroAbove(instruction.dst, 32);
        }
        return;
    }
    // A 32-bit value shifted by a constant below 32 is shifted in place: the 32-bit instruction reads exactly the
    // value's bits and its result clears the upper half.
    if (const auto constant = integerConstants.find(instruction.srcs[1]);
        constant != integerConstants.end() && ScalarSize(instruction.type) == 4 && (constant->second & 63U) < 32) {
        const auto count = static_cast<std::int64_t>(constant->second & 63U);
        const Op op = instruction.op == LirOpcode::Shl                                ? Op::Shl
                    : instruction.op == LirOpcode::Shr && instruction.type.IsSigned() ? Op::Sar
                                                                                      : Op::Shr;
        EmitCopy(destination, Value(instruction.srcs[0]), RegClass::General);
        if (count != 0)
            Emit(op, 4, {Operand::UseDef(destination), Operand::Imm(count)});
        int known = 32;
        if (op == Op::Shr)
            known = static_cast<int>(32 - count);
        else if (op == Op::Shl)
            known = std::min(32, KnownZeroAbove(instruction.srcs[0]) + static_cast<int>(count));
        NoteZeroAbove(instruction.dst, known);
        return;
    }
    // A right shift reads the value at its full width, extended by its signedness (or as unsigned for `lshr`); the
    // shift itself is always 64-bit with the count masked to six bits, which is what every reader of a narrow result
    // expects.
    const Reg value = instruction.op == LirOpcode::Shl
                        ? Value(instruction.srcs[0])
                        : Extended(instruction.srcs[0], false, instruction.op == LirOpcode::Lshr);
    const Op op = instruction.op == LirOpcode::Shl                                ? Op::Shl
                : instruction.op == LirOpcode::Shr && instruction.type.IsSigned() ? Op::Sar
                                                                                  : Op::Shr;
    if (const auto constant = integerConstants.find(instruction.srcs[1]); constant != integerConstants.end()) {
        const auto count = static_cast<std::int64_t>(constant->second & 63U);
        EmitCopy(destination, value, RegClass::General);
        if (count != 0)
            Emit(op, 8, {Operand::UseDef(destination), Operand::Imm(count)});
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
    EmitCopy(Rcx, Value(instruction.srcs[1]), RegClass::General);
    EmitCopy(destination, value, RegClass::General);
    auto &shift = Emit(op, 8, {Operand::UseDef(destination)});
    shift.implicitUses = Machine::RegisterBit(Rcx);
}

void Selector::SelectUnary(const LirInstr &instruction) {
    const Reg destination = Value(instruction.dst);
    const LirReg source = instruction.srcs[0];
    switch (instruction.op) {
    case LirOpcode::Neg:
        if (KindOf(instruction.type) == ValueKind::Xmm) {
            const bool single = instruction.type.kind == TypeRef::Kind::Float32;
            Address mask;
            mask.symbol = Symbol({single ? SymbolRef::Kind::Float32SignMask : SymbolRef::Kind::Float64SignMask, {}});
            const Reg maskRegister = NewXmm();
            Emit(Op::LoadFp, single ? 4 : 8, {Operand::Def(maskRegister), Operand::Mem(mask)}).rematerializable = true;
            EmitCopy(destination, Value(source), RegClass::Vector);
            Emit(Op::XorFp, 8, {Operand::UseDef(destination), Operand::Use(maskRegister)});
            return;
        }
        EmitCopy(destination, Value(source), RegClass::General);
        Emit(Op::Neg, 8, {Operand::UseDef(destination)});
        return;
    case LirOpcode::Not: {
        const auto size = static_cast<std::uint8_t>(ScalarSize(instruction.type));
        const Reg value = Value(source);
        Emit(Op::Test, size, {Operand::Use(value), Operand::Use(value)});
        const Reg flag = NewGpr();
        Emit(Op::SetCc, 1, {Operand::Def(flag)}).condition = static_cast<std::uint8_t>(Cond::Equal);
        Emit(Op::MovZx, 4, {Operand::Def(destination), Operand::Use(flag)}).sourceSize = 1;
        NoteZeroAbove(instruction.dst, 1);
        return;
    }
    case LirOpcode::BitNot:
        EmitCopy(destination, Value(source), RegClass::General);
        if (instruction.type.IsBool())
            Emit(Op::Xor, 4, {Operand::UseDef(destination), Operand::Imm(1)});
        else
            Emit(Op::Not, 8, {Operand::UseDef(destination)});
        return;
    case LirOpcode::Sqrt:
        Emit(Op::SqrtFp, static_cast<std::uint8_t>(Size(instruction.type)),
             {Operand::Def(destination), Operand::Use(Value(source))});
        return;
    default:
        return;
    }
}

Cond Selector::EmitCompare(const LirInstr &comparison) {
    LirReg left = comparison.srcs[0];
    LirReg right = comparison.srcs[1];
    const TypeRef &type = TypeOf(left);
    if (KindOf(type) == ValueKind::Xmm) {
        const auto size = static_cast<std::uint8_t>(Size(type));
        Cond condition = Cond::Equal;
        switch (comparison.op) {
        case LirOpcode::CmpEq:
            condition = Cond::Equal;
            break;
        case LirOpcode::CmpNe:
            condition = Cond::NotEqual;
            break;
        case LirOpcode::CmpGt:
            condition = Cond::Above;
            break;
        case LirOpcode::CmpGe:
            condition = Cond::AboveEqual;
            break;
        case LirOpcode::CmpLt:
            std::swap(left, right);
            condition = Cond::Above;
            break;
        default:
            std::swap(left, right);
            condition = Cond::AboveEqual;
            break;
        }
        // Above and above-or-equal are false for an unordered comparison, so only equality needs the parity flag.
        Emit(Op::CompareFp, size, {Operand::Use(Value(left)), Operand::Use(Value(right))});
        return condition;
    }
    // Both operands are read at the left operand's type, as the direct emitter reads them: lowering compares an enum
    // with a constant of its storage's wider literal type, and only the enum's own width is meaningful.
    const int width = ScalarSize(type);
    Cond condition = IntegerCondition(comparison.op, type.IsSigned());
    if (integerConstants.contains(left) && !integerConstants.contains(right)) {
        std::swap(left, right);
        condition = Swap(condition);
    }
    const auto operand = [&](const LirReg reg) { return ScalarSize(TypeOf(reg)) < width ? Extended(reg) : Value(reg); };
    const Reg value = operand(left);
    const auto size = static_cast<std::uint8_t>(width);
    if (const auto immediate = Immediate(right, width)) {
        if (*immediate == 0)
            Emit(Op::Test, size, {Operand::Use(value), Operand::Use(value)});
        else
            Emit(Op::Cmp, size, {Operand::Use(value), Operand::Imm(*immediate)});
        return condition;
    }
    Emit(Op::Cmp, size, {Operand::Use(value), Operand::Use(operand(right))});
    return condition;
}

void Selector::SelectComparison(const LirInstr &instruction) {
    const Reg destination = Value(instruction.dst);
    const bool floating = KindOfReg(instruction.srcs[0]) == ValueKind::Xmm;
    const Cond condition = EmitCompare(instruction);
    const auto set = [&](const Cond flagCondition) {
        const Reg flag = NewGpr();
        Emit(Op::SetCc, 1, {Operand::Def(flag)}).condition = static_cast<std::uint8_t>(flagCondition);
        return flag;
    };
    if (floating && (instruction.op == LirOpcode::CmpEq || instruction.op == LirOpcode::CmpNe)) {
        const bool equal = instruction.op == LirOpcode::CmpEq;
        const Reg first = set(condition);
        const Reg second = set(equal ? Cond::NoParity : Cond::Parity);
        Emit(Op::MovZx, 4, {Operand::Def(destination), Operand::Use(first)}).sourceSize = 1;
        const Reg widened = NewGpr();
        Emit(Op::MovZx, 4, {Operand::Def(widened), Operand::Use(second)}).sourceSize = 1;
        Emit(equal ? Op::And : Op::Or, 4, {Operand::UseDef(destination), Operand::Use(widened)});
        return;
    }
    const Reg flag = set(condition);
    Emit(Op::MovZx, 4, {Operand::Def(destination), Operand::Use(flag)}).sourceSize = 1;
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
        if (destinationSize > sourceSize && !from.IsBool() && !AlreadyExtended(source, sourceSize, isSigned)) {
            auto &extend = Emit(isSigned ? Op::MovSx : Op::MovZx, isSigned ? 8 : 4,
                                {Operand::Def(destination), Operand::Use(Value(source))});
            extend.sourceSize = static_cast<std::uint8_t>(sourceSize);
            if (!isSigned)
                known = sourceSize * 8;
        }
        else {
            EmitCopy(destination, Value(source), RegClass::General);
        }
        if (to.IsBool() && !from.IsBool() && destinationSize < 8 && known > destinationSize * 8) {
            Emit(Op::MovZx, 4, {Operand::Def(destination), Operand::Use(destination)}).sourceSize =
                static_cast<std::uint8_t>(destinationSize);
            known = destinationSize * 8;
        }
        NoteZeroAbove(instruction.dst, known);
        return;
    }
    if (sourceKind == ValueKind::Gpr) {
        Emit(Op::IntToFp, static_cast<std::uint8_t>(Size(to)),
             {Operand::Def(destination), Operand::Use(Extended(source))})
            .sourceSize = 8;
        return;
    }
    if (destinationKind == ValueKind::Gpr) {
        const bool single = from.kind == TypeRef::Kind::Float32;
        if (to.IsInteger() && !to.IsSigned() && SizeOf(to) == 8) {
            const Reg scratch = NewXmm();
            const Reg clobbered = NewXmm();
            EmitCopy(clobbered, Value(source), RegClass::Vector);
            const auto bound =
                Symbol({single ? SymbolRef::Kind::Float32 : SymbolRef::Kind::Float64, "9223372036854775808.0"});
            auto &convert = Emit(Op::FpToUInt64, 8,
                                 {Operand::Def(destination), Operand::UseDef(clobbered), Operand::Def(scratch),
                                  Operand::SymbolTarget(bound)});
            convert.sourceSize = single ? 4 : 8;
            return;
        }
        Emit(Op::FpToInt, 8, {Operand::Def(destination), Operand::Use(Value(source))}).sourceSize =
            static_cast<std::uint8_t>(single ? 4 : 8);
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
} // namespace Rux::X86_64Machine::Detail
