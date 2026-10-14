// Synthesized software-float unpack, round-and-pack and comparison helpers shared by every operation, and the builder
// they are written with.

#include "Lowering/HirToLir/SoftFloatBuilder.h"
#include "Numeric/FloatEncoding.h"
#include "Numeric/SoftwareFloat.h"

#include <format>
#include <utility>

namespace Rux::HirToLirDetail {
namespace {
[[nodiscard]] std::string HexText(const WideInteger &value) {
    std::string text;
    for (std::size_t word = (value.Width() + 63) / 64; word-- > 0;) {
        if (const std::uint64_t bits = value.Word64(word); bits != 0 || !text.empty()) {
            text += text.empty() ? std::format("{:x}", bits) : std::format("{:016x}", bits);
        }
    }
    return "0x" + (text.empty() ? std::string("0") : text);
}

/// A run-time shift amount made safe: whether it is below `type`'s width, and the amount itself, or zero when it is
/// not, at `type` so it can shift a value of that type.
struct ClampedAmount {
    SoftFloatValue inRange;
    SoftFloatValue amount;
};

[[nodiscard]] ClampedAmount ClampAmount(SoftFloatBuilder &builder, SoftFloatValue amount, const TypeRef &type) {
    const std::uint32_t amountBits = SoftFloatBitsOf(amount.type);
    if (amount.type.IsSigned() || amountBits < 16) {
        amount = builder.Resize(amount, SoftFloatUnsignedType(amountBits < 16 ? 16 : amountBits));
    }
    const SoftFloatValue inRange = builder.Lt(amount, builder.Constant(SoftFloatBitsOf(type), amount.type));
    const SoftFloatValue safe = builder.Select(inRange, amount, builder.Constant(0, amount.type));
    return {inRange, builder.Resize(safe, type)};
}

/// Leading zeros of a value of at most 64 bits, by binary search: each step tests the top half of what is left.
[[nodiscard]] SoftFloatValue CountLeadingZerosNarrow(SoftFloatBuilder &builder, const SoftFloatValue &value) {
    const std::uint32_t width = SoftFloatBitsOf(value.type);
    SoftFloatValue rest = value;
    SoftFloatValue count = builder.Unsigned(0, 32);
    for (std::uint32_t step = width / 2; step > 0; step /= 2) {
        const SoftFloatValue empty = builder.IsZero(builder.ShiftRight(rest, width - step));
        rest = builder.Select(empty, builder.ShiftLeft(rest, step), rest);
        count = builder.Select(empty, builder.Add(count, builder.Unsigned(step, 32)), count);
    }
    // The search stops one short for zero, which has no leading bit to find.
    return builder.Select(builder.IsZero(value), builder.Unsigned(width, 32), count);
}

[[nodiscard]] SoftFloatValue ClassCode(SoftFloatBuilder &builder, const FloatClass classification) {
    return builder.Constant(static_cast<std::int64_t>(classification), TypeRef::MakeUInt8());
}

/// `NormalizeFloat`'s shift of a significand and exponent, without its classification. A zero significand keeps its
/// exponent.
[[nodiscard]] std::pair<SoftFloatValue, SoftFloatValue> Normalized(SoftFloatBuilder &builder, const FloatFormat &format,
                                                                   const SoftFloatValue &significand,
                                                                   const SoftFloatValue &exponent) {
    const TypeRef exponentType = TypeRef::MakeInt32();
    const SoftFloatValue target =
        builder.Unsigned(SoftFloatBitsOf(significand.type) - format.precisionBits - FloatExtraPrecisionBits, 32);
    const SoftFloatValue zeros = builder.CountLeadingZeros(significand);
    const SoftFloatValue none = builder.Unsigned(0, 32);
    const SoftFloatValue left = builder.Select(builder.Gt(zeros, target), builder.Sub(zeros, target), none);
    const SoftFloatValue right = builder.Select(builder.Lt(zeros, target), builder.Sub(target, zeros), none);
    const SoftFloatValue shifted = builder.ShiftRightSticky(builder.ShiftLeftClamped(significand, left), right);
    const SoftFloatValue adjusted =
        builder.Add(builder.Sub(exponent, builder.Resize(left, exponentType)), builder.Resize(right, exponentType));
    return {shifted, builder.Select(builder.IsZero(significand), exponent, adjusted)};
}
} // namespace

// Types

TypeRef SoftFloatUnsignedType(const std::uint32_t bits) {
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

TypeRef SoftFloatFloatType(const FloatFormat &format) {
    switch (format.valueBits) {
    case 8:
        return TypeRef::MakePrimitive(TypeRef::Kind::Float8);
    case 16:
        return TypeRef::MakePrimitive(TypeRef::Kind::Float16);
    case 32:
        return TypeRef::MakePrimitive(TypeRef::Kind::Float32);
    case 64:
        return TypeRef::MakePrimitive(TypeRef::Kind::Float64);
    case 80:
        return TypeRef::MakePrimitive(TypeRef::Kind::Float80);
    case 128:
        return TypeRef::MakePrimitive(TypeRef::Kind::Float128);
    case 256:
        return TypeRef::MakePrimitive(TypeRef::Kind::Float256);
    default:
        return TypeRef::MakePrimitive(TypeRef::Kind::Float512);
    }
}

std::uint32_t SoftFloatBitsOf(const TypeRef &type) {
    return static_cast<std::uint32_t>(type.SizeInBytes().value_or(0) * 8);
}

// The builder

SoftFloatBuilder::SoftFloatBuilder(SoftFloatHelperSynthesis &inputSynthesis)
    : synthesis(&inputSynthesis) {
}

SoftFloatHelperSynthesis &SoftFloatBuilder::Synthesis() const noexcept {
    return *synthesis;
}

SoftFloatValue SoftFloatBuilder::Parameter(const std::size_t index) const {
    const LirParam &param = synthesis->Function().params.at(index);
    return {param.reg, param.type};
}

LirReg SoftFloatBuilder::Emit(const LirOpcode op, const TypeRef &type, std::vector<LirReg> sources,
                              std::string argument) {
    LirInstr instruction;
    instruction.dst = op == LirOpcode::Store ? LirNoReg : synthesis->NewReg();
    instruction.op = op;
    instruction.type = type;
    instruction.srcs = std::move(sources);
    instruction.strArg = std::move(argument);
    const LirReg dst = instruction.dst;
    synthesis->Emit(std::move(instruction));
    return dst;
}

SoftFloatValue SoftFloatBuilder::Constant(const std::int64_t value, const TypeRef &type) {
    if (type.IsSigned()) {
        return {Emit(LirOpcode::Const, type, {}, std::to_string(value)), type};
    }
    return Constant(
        WideInteger::FromUnsigned(static_cast<std::uint64_t>(value), 64).Extended(WideInteger::MaxBits, value < 0),
        type);
}

SoftFloatValue SoftFloatBuilder::Constant(const WideInteger &value, const TypeRef &type) {
    const std::uint32_t width = SoftFloatBitsOf(type);
    const WideInteger bits = value.Width() > width ? value.Truncated(width) : value.Extended(width, false);
    std::string text = HexText(bits);
    if (type.IsBool()) {
        text = bits.IsZero() ? "false" : "true";
    }
    else if (type.IsSigned() && bits.IsNegative()) {
        text = "-" + bits.Negated().ToDecimal();
    }
    return {Emit(LirOpcode::Const, type, {}, std::move(text)), type};
}

SoftFloatValue SoftFloatBuilder::Unsigned(const std::uint64_t value, const std::uint32_t bits) {
    return Constant(WideInteger::FromUnsigned(value, 64), SoftFloatUnsignedType(bits));
}

SoftFloatValue SoftFloatBuilder::Bool(const bool value) {
    return Constant(value ? 1 : 0, TypeRef::MakeBool());
}

SoftFloatValue SoftFloatBuilder::OneAt(const std::uint32_t bit, const TypeRef &type) {
    const std::uint32_t width = SoftFloatBitsOf(type);
    return Constant(bit >= width ? WideInteger::Zero(width) : WideInteger::FromUnsigned(1, width).ShiftedLeft(bit),
                    type);
}

SoftFloatValue SoftFloatBuilder::LowMask(const std::uint32_t bits, const TypeRef &type) {
    const std::uint32_t width = SoftFloatBitsOf(type);
    return Constant(bits == 0 ? WideInteger::Zero(width) : WideInteger::AllOnes(bits < width ? bits : width), type);
}

SoftFloatValue SoftFloatBuilder::Binary(const LirOpcode op, const SoftFloatValue &left, const SoftFloatValue &right) {
    if (left.type != right.type) {
        synthesis->Fail(
            std::format("'{}' of '{}' and '{}'", LirOpcodeName(op), left.type.ToString(), right.type.ToString()));
    }
    return {Emit(op, left.type, {left.reg, right.reg}), left.type};
}

SoftFloatValue SoftFloatBuilder::Compare(const LirOpcode op, const SoftFloatValue &left, const SoftFloatValue &right) {
    if (left.type != right.type) {
        synthesis->Fail(
            std::format("'{}' of '{}' and '{}'", LirOpcodeName(op), left.type.ToString(), right.type.ToString()));
    }
    return {Emit(op, TypeRef::MakeBool(), {left.reg, right.reg}), TypeRef::MakeBool()};
}

SoftFloatValue SoftFloatBuilder::Add(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Binary(LirOpcode::Add, left, right);
}

SoftFloatValue SoftFloatBuilder::Sub(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Binary(LirOpcode::Sub, left, right);
}

SoftFloatValue SoftFloatBuilder::Mul(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Binary(LirOpcode::Mul, left, right);
}

SoftFloatValue SoftFloatBuilder::Div(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Binary(LirOpcode::Div, left, right);
}

SoftFloatValue SoftFloatBuilder::Mod(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Binary(LirOpcode::Mod, left, right);
}

SoftFloatValue SoftFloatBuilder::And(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Binary(LirOpcode::And, left, right);
}

SoftFloatValue SoftFloatBuilder::Or(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Binary(LirOpcode::Or, left, right);
}

SoftFloatValue SoftFloatBuilder::Xor(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Binary(LirOpcode::Xor, left, right);
}

SoftFloatValue SoftFloatBuilder::BitNot(const SoftFloatValue &value) {
    return {Emit(LirOpcode::BitNot, value.type, {value.reg}), value.type};
}

SoftFloatValue SoftFloatBuilder::Not(const SoftFloatValue &value) {
    return {Emit(LirOpcode::Not, TypeRef::MakeBool(), {value.reg}), TypeRef::MakeBool()};
}

SoftFloatValue SoftFloatBuilder::Eq(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Compare(LirOpcode::CmpEq, left, right);
}

SoftFloatValue SoftFloatBuilder::Ne(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Compare(LirOpcode::CmpNe, left, right);
}

SoftFloatValue SoftFloatBuilder::Lt(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Compare(LirOpcode::CmpLt, left, right);
}

SoftFloatValue SoftFloatBuilder::Le(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Compare(LirOpcode::CmpLe, left, right);
}

SoftFloatValue SoftFloatBuilder::Gt(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Compare(LirOpcode::CmpGt, left, right);
}

SoftFloatValue SoftFloatBuilder::Ge(const SoftFloatValue &left, const SoftFloatValue &right) {
    return Compare(LirOpcode::CmpGe, left, right);
}

SoftFloatValue SoftFloatBuilder::IsZero(const SoftFloatValue &value) {
    return Eq(value, Constant(0, value.type));
}

SoftFloatValue SoftFloatBuilder::IsNonZero(const SoftFloatValue &value) {
    return Ne(value, Constant(0, value.type));
}

SoftFloatValue SoftFloatBuilder::BitSet(const SoftFloatValue &value, const std::uint32_t bit) {
    return IsNonZero(And(value, OneAt(bit, value.type)));
}

SoftFloatValue SoftFloatBuilder::Select(const SoftFloatValue &condition, const SoftFloatValue &whenTrue,
                                        const SoftFloatValue &whenFalse) {
    if (whenTrue.type.IsBool()) {
        return Or(And(condition, whenTrue), And(Not(condition), whenFalse));
    }
    // All ones when the condition holds, zero when it does not.
    const SoftFloatValue mask = Sub(Constant(0, whenTrue.type), Resize(condition, whenTrue.type));
    return Or(And(whenTrue, mask), And(whenFalse, BitNot(mask)));
}

SoftFloatValue SoftFloatBuilder::Resize(const SoftFloatValue &value, const TypeRef &type) {
    if (value.type == type) {
        return value;
    }
    return {Emit(LirOpcode::Cast, type, {value.reg}, value.type.ToString()), type};
}

SoftFloatValue SoftFloatBuilder::ShiftLeft(const SoftFloatValue &value, const std::uint32_t amount) {
    if (amount == 0) {
        return value;
    }
    if (amount >= SoftFloatBitsOf(value.type)) {
        return Constant(0, value.type);
    }
    return Binary(LirOpcode::Shl, value, Constant(amount, value.type));
}

SoftFloatValue SoftFloatBuilder::ShiftRight(const SoftFloatValue &value, const std::uint32_t amount) {
    if (amount == 0) {
        return value;
    }
    if (amount >= SoftFloatBitsOf(value.type)) {
        return Constant(0, value.type);
    }
    return Binary(LirOpcode::Lshr, value, Constant(amount, value.type));
}

SoftFloatValue SoftFloatBuilder::ShiftLeftClamped(const SoftFloatValue &value, const SoftFloatValue &amount) {
    const ClampedAmount clamped = ClampAmount(*this, amount, value.type);
    return Select(clamped.inRange, Binary(LirOpcode::Shl, value, clamped.amount), Constant(0, value.type));
}

SoftFloatValue SoftFloatBuilder::ShiftRightClamped(const SoftFloatValue &value, const SoftFloatValue &amount) {
    const ClampedAmount clamped = ClampAmount(*this, amount, value.type);
    return Select(clamped.inRange, Binary(LirOpcode::Lshr, value, clamped.amount), Constant(0, value.type));
}

SoftFloatValue SoftFloatBuilder::ShiftRightSticky(const SoftFloatValue &value, const SoftFloatValue &amount) {
    const ClampedAmount clamped = ClampAmount(*this, amount, value.type);
    const SoftFloatValue zero = Constant(0, value.type);
    const SoftFloatValue shifted = Binary(LirOpcode::Lshr, value, clamped.amount);
    // Shifting the kept bits back shows whether any one bit fell off; past the width every bit did.
    const SoftFloatValue restored = Binary(LirOpcode::Shl, shifted, clamped.amount);
    const SoftFloatValue lost = Ne(value, Select(clamped.inRange, restored, zero));
    return Or(Select(clamped.inRange, shifted, zero), Resize(lost, value.type));
}

SoftFloatValue SoftFloatBuilder::CountLeadingZeros(const SoftFloatValue &value) {
    const std::uint32_t width = SoftFloatBitsOf(value.type);
    if (width <= 64) {
        return CountLeadingZerosNarrow(*this, value);
    }
    // Find the highest non-zero 64-bit word, scanning upward so a higher one replaces a lower; a zero value leaves
    // the bottom word, whose 64 leading zeros complete the count.
    const std::uint32_t words = width / 64;
    const TypeRef word = SoftFloatUnsignedType(64);
    SoftFloatValue top = Constant(0, word);
    SoftFloatValue above = Unsigned((words - 1) * 64, 32);
    for (std::uint32_t index = 0; index < words; ++index) {
        const SoftFloatValue current = Resize(ShiftRight(value, index * 64), word);
        const SoftFloatValue present = IsNonZero(current);
        top = Select(present, current, top);
        above = Select(present, Unsigned((words - 1 - index) * 64, 32), above);
    }
    return Add(above, CountLeadingZerosNarrow(*this, top));
}

SoftFloatValue SoftFloatBuilder::BitsOfFloat(const SoftFloatValue &value) {
    const TypeRef bitsType = SoftFloatUnsignedType(SoftFloatBitsOf(value.type));
    const LirReg slot = Emit(LirOpcode::Alloca, bitsType, {});
    Emit(LirOpcode::Store, value.type, {value.reg, slot});
    const SoftFloatValue bits{Emit(LirOpcode::Load, bitsType, {slot}), bitsType};
    if (value.type->kind == TypeRef::Kind::Float80) {
        return And(bits, LowMask(80, bitsType));
    }
    return bits;
}

SoftFloatValue SoftFloatBuilder::FloatOfBits(const SoftFloatValue &bits, const TypeRef &type) {
    const SoftFloatValue stored = Resize(bits, SoftFloatUnsignedType(SoftFloatBitsOf(type)));
    const LirReg slot = Emit(LirOpcode::Alloca, stored.type, {});
    Emit(LirOpcode::Store, stored.type, {stored.reg, slot});
    return {Emit(LirOpcode::Load, type, {slot}), type};
}

SoftFloatVariable SoftFloatBuilder::Variable(const TypeRef &type) {
    return {Emit(LirOpcode::Alloca, type, {}), type};
}

SoftFloatVariable SoftFloatBuilder::Variable(const SoftFloatValue &initial) {
    const SoftFloatVariable variable = Variable(initial.type);
    Set(variable, initial);
    return variable;
}

SoftFloatValue SoftFloatBuilder::Get(const SoftFloatVariable &variable) {
    return {Emit(LirOpcode::Load, variable.type, {variable.slot}), variable.type};
}

void SoftFloatBuilder::Set(const SoftFloatVariable &variable, const SoftFloatValue &value) {
    if (variable.type != value.type) {
        synthesis->Fail(
            std::format("store of '{}' into a '{}' variable", value.type.ToString(), variable.type.ToString()));
    }
    Emit(LirOpcode::Store, variable.type, {value.reg, variable.slot});
}

std::uint32_t SoftFloatBuilder::Block(const char *label) {
    return synthesis->Builder().CreateBlock(label);
}

void SoftFloatBuilder::Enter(const std::uint32_t block) {
    if (!synthesis->Builder().SelectBlock(block)) {
        synthesis->Fail(std::string(synthesis->Builder().FailureReason()));
    }
}

void SoftFloatBuilder::Jump(const std::uint32_t target) {
    if (synthesis->Builder().IsTerminated()) {
        return;
    }
    LirTerminator jump;
    jump.kind = LirTermKind::Jump;
    jump.trueTarget = target;
    synthesis->Terminate(std::move(jump));
}

void SoftFloatBuilder::If(const SoftFloatValue &condition, const std::function<void()> &then) {
    IfElse(condition, then, [] {});
}

void SoftFloatBuilder::IfElse(const SoftFloatValue &condition, const std::function<void()> &then,
                              const std::function<void()> &otherwise) {
    const std::uint32_t thenBlock = Block("sf.then");
    const std::uint32_t elseBlock = Block("sf.else");
    const std::uint32_t join = Block("sf.join");
    LirTerminator branch;
    branch.kind = LirTermKind::Branch;
    branch.cond = condition.reg;
    branch.trueTarget = thenBlock;
    branch.falseTarget = elseBlock;
    synthesis->Terminate(std::move(branch));
    Enter(thenBlock);
    then();
    Jump(join);
    Enter(elseBlock);
    otherwise();
    Jump(join);
    Enter(join);
}

void SoftFloatBuilder::While(const std::function<SoftFloatValue()> &condition, const std::function<void()> &body) {
    const std::uint32_t head = Block("sf.loop");
    const std::uint32_t bodyBlock = Block("sf.body");
    const std::uint32_t exit = Block("sf.exit");
    Jump(head);
    Enter(head);
    LirTerminator branch;
    branch.kind = LirTermKind::Branch;
    branch.cond = condition().reg;
    branch.trueTarget = bodyBlock;
    branch.falseTarget = exit;
    synthesis->Terminate(std::move(branch));
    Enter(bodyBlock);
    body();
    Jump(head);
    Enter(exit);
}

SoftFloatValue SoftFloatBuilder::Call(const std::string &symbol, const TypeRef &result,
                                      const std::vector<SoftFloatValue> &arguments) {
    std::vector<LirReg> sources;
    for (const SoftFloatValue &argument : arguments) {
        sources.push_back(argument.reg);
    }
    return {Emit(LirOpcode::Call, result, std::move(sources), symbol), result};
}

void SoftFloatBuilder::Return(const SoftFloatValue &value) {
    LirTerminator ret;
    ret.kind = LirTermKind::Return;
    ret.retVal = value.reg;
    ret.retType = value.type;
    synthesis->Terminate(std::move(ret));
}

void SoftFloatBuilder::Finish() {
    if (!synthesis->Builder().IsTerminated()) {
        LirTerminator unreachable;
        unreachable.kind = LirTermKind::Unreachable;
        synthesis->Terminate(std::move(unreachable));
    }
}

// Unpack and pack

std::uint32_t SoftFloatWorkingBits(const FloatFormat &format) noexcept {
    switch (format.valueBits) {
    case 8:
    case 16:
        return 32;
    case 32:
        return 64;
    case 64:
    case 80:
        return 128;
    case 128:
        return 256;
    default:
        return 512;
    }
}

TypeRef SoftFloatWorkingType(const FloatFormat &format) {
    return SoftFloatUnsignedType(SoftFloatWorkingBits(format));
}

SoftFloatUnpacked UnpackSoftFloat(SoftFloatBuilder &builder, const FloatFormat &format, const SoftFloatValue &value) {
    return UnpackSoftFloatBits(builder, format, value.type.IsFloat() ? builder.BitsOfFloat(value) : value);
}

SoftFloatUnpacked UnpackSoftFloatBits(SoftFloatBuilder &builder, const FloatFormat &format,
                                      const SoftFloatValue &bits) {
    SoftFloatBuilder &b = builder;
    const TypeRef work = SoftFloatWorkingType(format);
    const TypeRef exponentType = TypeRef::MakeInt32();
    const std::uint32_t fractionBits = format.FractionBits();
    const SoftFloatValue raw = b.And(b.Resize(bits, work), b.LowMask(format.valueBits, work));
    const SoftFloatValue exponentField =
        b.Resize(b.And(b.ShiftRight(raw, format.SignificandFieldBits()), b.LowMask(format.exponentBits, work)),
                 TypeRef::MakeUInt32());
    const SoftFloatValue significandField = b.And(raw, b.LowMask(format.SignificandFieldBits(), work));
    const SoftFloatValue fraction = b.And(raw, b.LowMask(fractionBits, work));
    const SoftFloatValue integerBit = format.explicitIntegerBit ? b.BitSet(raw, fractionBits) : b.Bool(true);
    const SoftFloatValue exponentZero = b.IsZero(exponentField);

    // `FloatEncoding::Classify`, except that a subnormal or a float80 pseudo-denormal is already `Normal`, which
    // normalizing it makes it.
    const SoftFloatValue nan = b.Select(b.BitSet(raw, fractionBits - 1), ClassCode(b, FloatClass::QuietNaN),
                                        ClassCode(b, FloatClass::SignalingNaN));
    const SoftFloatValue atMaximum =
        b.Select(integerBit, b.Select(b.IsZero(fraction), ClassCode(b, FloatClass::Infinity), nan),
                 ClassCode(b, FloatClass::Invalid));
    const SoftFloatValue between =
        b.Select(integerBit, ClassCode(b, FloatClass::Normal), ClassCode(b, FloatClass::Invalid));
    const SoftFloatValue atZero =
        b.Select(b.IsZero(significandField), ClassCode(b, FloatClass::Zero), ClassCode(b, FloatClass::Normal));
    const SoftFloatValue classification =
        b.Select(exponentZero, atZero,
                 b.Select(b.Eq(exponentField, b.Unsigned(format.MaxExponentField(), 32)), atMaximum, between));
    const SoftFloatValue finite = b.Eq(classification, ClassCode(b, FloatClass::Normal));
    const SoftFloatValue isNaN = b.Or(b.Eq(classification, ClassCode(b, FloatClass::QuietNaN)),
                                      b.Eq(classification, ClassCode(b, FloatClass::SignalingNaN)));

    const SoftFloatValue zero = b.Constant(0, work);
    SoftFloatValue significand = significandField;
    if (!format.explicitIntegerBit) {
        significand = b.Or(significand, b.Select(exponentZero, zero, b.OneAt(fractionBits, work)));
    }
    const SoftFloatValue exponent =
        b.Select(exponentZero, b.Constant(1 - format.exponentBias, exponentType),
                 b.Sub(b.Resize(exponentField, exponentType), b.Constant(format.exponentBias, exponentType)));
    significand = b.Select(finite, b.ShiftLeft(significand, FloatExtraPrecisionBits), zero);
    const auto [normalized, normalizedExponent] = Normalized(b, format, significand, exponent);
    return {classification, b.BitSet(raw, format.valueBits - 1),
            b.Select(finite, normalizedExponent, b.Constant(0, exponentType)), normalized,
            b.Select(isNaN, fraction, zero)};
}

void NormalizeSoftFloat(SoftFloatBuilder &builder, const FloatFormat &format, SoftFloatUnpacked &value) {
    const SoftFloatValue zero = builder.IsZero(value.significand);
    const auto [significand, exponent] = Normalized(builder, format, value.significand, value.exponent);
    value.significand = significand;
    value.exponent = exponent;
    value.classification =
        builder.Select(zero, ClassCode(builder, FloatClass::Zero), ClassCode(builder, FloatClass::Normal));
}

SoftFloatValue PackSoftFloat(SoftFloatBuilder &builder, const FloatFormat &format, const SoftFloatUnpacked &value) {
    return builder.FloatOfBits(PackSoftFloatBits(builder, format, value), SoftFloatFloatType(format));
}

SoftFloatValue PackSoftFloatBits(SoftFloatBuilder &builder, const FloatFormat &format, const SoftFloatUnpacked &value) {
    // Every outcome is computed and the class picks one, so the result needs no branch. Each step mirrors one of
    // `PackFloat`'s, and the overrides at the end are its early returns, latest first.
    SoftFloatBuilder &b = builder;
    const TypeRef work = SoftFloatWorkingType(format);
    const TypeRef exponentType = TypeRef::MakeInt32();
    const std::uint32_t fractionBits = format.FractionBits();
    const auto is = [&](const FloatClass classification) {
        return b.Eq(value.classification, ClassCode(b, classification));
    };
    const SoftFloatValue zero = b.Constant(0, work);
    const SoftFloatValue sign = b.Select(value.negative, b.OneAt(format.valueBits - 1, work), zero);
    const SoftFloatValue maximumExponent =
        b.Constant(WideInteger::FromUnsigned(format.MaxExponentField(), WideInteger::MaxBits)
                       .ShiftedLeft(format.SignificandFieldBits()),
                   work);
    const SoftFloatValue integerBit = format.explicitIntegerBit ? b.OneAt(fractionBits, work) : zero;
    const SoftFloatValue infinity = b.Or(b.Or(sign, maximumExponent), integerBit);
    const SoftFloatValue quietBit = b.OneAt(fractionBits - 1, work);

    // A NaN keeps its payload, quieted or kept signaling with a non-zero fraction.
    const SoftFloatValue payload = b.And(value.nanPayload, b.LowMask(fractionBits, work));
    const SoftFloatValue signaling = b.And(payload, b.BitNot(quietBit));
    const SoftFloatValue nanFraction = b.Select(is(FloatClass::QuietNaN), b.Or(payload, quietBit),
                                                b.Select(b.IsZero(signaling), b.Constant(1, work), signaling));

    // A finite value: normalize, denormalize below the normal range, then round to nearest, ties to even.
    const auto [normalized, normalizedExponent] = Normalized(b, format, value.significand, value.exponent);
    const SoftFloatValue minimumNormal = b.Constant(1 - format.exponentBias, exponentType);
    const SoftFloatValue maximumNormal =
        b.Constant(static_cast<std::int64_t>(format.MaxExponentField()) - 1 - format.exponentBias, exponentType);
    const SoftFloatValue below = b.Lt(normalizedExponent, minimumNormal);
    const SoftFloatValue distance = b.Resize(
        b.Select(below, b.Sub(minimumNormal, normalizedExponent), b.Constant(0, exponentType)), TypeRef::MakeUInt32());
    const SoftFloatValue working = b.ShiftRightSticky(normalized, distance);
    SoftFloatValue exponent = b.Select(below, minimumNormal, normalizedExponent);
    const SoftFloatValue extra = b.And(working, b.Constant(7, work));
    SoftFloatValue rounded = b.ShiftRight(working, FloatExtraPrecisionBits);
    const SoftFloatValue roundUp =
        b.Or(b.Gt(extra, b.Constant(4, work)),
             b.And(b.Eq(extra, b.Constant(4, work)), b.IsNonZero(b.And(rounded, b.Constant(1, work)))));
    rounded = b.Add(rounded, b.Resize(roundUp, work));
    const SoftFloatValue carried = b.BitSet(rounded, format.precisionBits);
    rounded = b.Select(carried, b.ShiftRight(rounded, 1), rounded);
    exponent = b.Select(carried, b.Add(exponent, b.Constant(1, exponentType)), exponent);
    const SoftFloatValue normal = b.Or(b.Gt(exponent, minimumNormal), b.BitSet(rounded, format.precisionBits - 1));
    const SoftFloatValue biased = b.ShiftLeft(
        b.Resize(b.Add(exponent, b.Constant(format.exponentBias, exponentType)), work), format.SignificandFieldBits());
    // Truncating to the significand field drops an implicit leading bit, as `Compose` does.
    const SoftFloatValue finite = b.Or(b.Or(sign, b.Select(normal, biased, zero)),
                                       b.And(rounded, b.LowMask(format.SignificandFieldBits(), work)));

    SoftFloatValue result = b.Select(b.IsZero(rounded), sign, finite);
    result = b.Select(b.Gt(exponent, maximumNormal), infinity, result);
    result = b.Select(b.Or(is(FloatClass::Zero), b.IsZero(value.significand)), sign, result);
    result = b.Select(b.Or(is(FloatClass::QuietNaN), is(FloatClass::SignalingNaN)),
                      b.Or(b.Or(sign, maximumExponent), b.Or(integerBit, nanFraction)), result);
    result = b.Select(is(FloatClass::Invalid), b.Or(infinity, quietBit), result);
    return b.Select(is(FloatClass::Infinity), infinity, result);
}

// The comparison helper

void SynthesizeSoftFloatComparison(SoftFloatHelperSynthesis &synthesis) {
    // `CompareFloat`: its tests in order, the first that applies deciding, here applied last.
    SoftFloatBuilder b(synthesis);
    const FloatFormat &format = synthesis.Format();
    const SoftFloatUnpacked left = UnpackSoftFloat(b, format, b.Parameter(0));
    const SoftFloatUnpacked right = UnpackSoftFloat(b, format, b.Parameter(1));
    const auto is = [&](const SoftFloatUnpacked &value, const FloatClass classification) {
        return b.Eq(value.classification, ClassCode(b, classification));
    };
    const auto ordering = [&](const SoftFloatOrdering code) {
        return b.Constant(static_cast<std::int64_t>(code), TypeRef::MakeUInt8());
    };
    const SoftFloatValue less = ordering(SoftFloatOrdering::Less);
    const SoftFloatValue greater = ordering(SoftFloatOrdering::Greater);
    const SoftFloatValue equal = ordering(SoftFloatOrdering::Equal);

    // Both values normalize to one leading bit, so exponent then significand orders two finite magnitudes, and an
    // infinity is above every one of them.
    const SoftFloatValue leftInfinite = is(left, FloatClass::Infinity);
    const SoftFloatValue rightInfinite = is(right, FloatClass::Infinity);
    const SoftFloatValue anyInfinite = b.Or(leftInfinite, rightInfinite);
    const SoftFloatValue sameExponent = b.Eq(left.exponent, right.exponent);
    const SoftFloatValue magnitudeLess = b.Select(
        anyInfinite, b.And(rightInfinite, b.Not(leftInfinite)),
        b.Or(b.Lt(left.exponent, right.exponent), b.And(sameExponent, b.Lt(left.significand, right.significand))));
    const SoftFloatValue magnitudeEqual = b.Select(anyInfinite, b.And(leftInfinite, rightInfinite),
                                                   b.And(sameExponent, b.Eq(left.significand, right.significand)));
    // With one sign on both sides, a negative pair orders the other way round. `larger` is also the answer for a
    // negative left against a non-negative right, or a non-zero left against zero.
    const SoftFloatValue smaller = b.Select(left.negative, greater, less);
    const SoftFloatValue larger = b.Select(left.negative, less, greater);
    SoftFloatValue code = b.Select(magnitudeEqual, equal, b.Select(magnitudeLess, smaller, larger));

    const SoftFloatValue leftZero = is(left, FloatClass::Zero);
    const SoftFloatValue rightZero = is(right, FloatClass::Zero);
    code = b.Select(b.Ne(left.negative, right.negative), larger, code);
    code = b.Select(rightZero, larger, code);
    code = b.Select(leftZero, b.Select(right.negative, greater, less), code);
    code = b.Select(b.And(leftZero, rightZero), equal, code);
    const auto unordered = [&](const SoftFloatUnpacked &value) {
        return b.Or(b.Or(is(value, FloatClass::QuietNaN), is(value, FloatClass::SignalingNaN)),
                    is(value, FloatClass::Invalid));
    };
    code = b.Select(b.Or(unordered(left), unordered(right)), ordering(SoftFloatOrdering::Unordered), code);
    b.Return(code);
    b.Finish();
}
} // namespace Rux::HirToLirDetail
