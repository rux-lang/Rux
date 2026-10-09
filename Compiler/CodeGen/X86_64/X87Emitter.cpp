// x86-64 x87 lowering for float80, and for the float32 and float64 remainder.
//
// The x87 unit computes in the 80-bit extended format float80 is stored in, so on x86-64 float80 arithmetic,
// comparisons and conversions to and from the machine floats run there instead of through the software helpers. SSE
// has no remainder instruction, so float32 and float64 `%` run the same exact `fprem` loop as float80's.
//
// Every instruction here starts and ends with an empty x87 register stack: its operands are pushed from their frame
// slots, combined, and the one result is popped straight back into the destination slot. Nothing stays on the stack
// across instructions, so calls, the SSE paths and the register allocator never see x87 state.
//
// The Win64 default control word selects 53-bit precision, and Win64 treats the control word as callee-saved, so every
// operation that rounds saves the caller's control word into the first wide temporary, loads 0x037F (64-bit precision,
// round to nearest, every exception masked) from the two bytes after it, and restores the caller's word afterwards.
// Loads, `fild`, `fprem` and the comparisons are exact under any precision control and are left unwrapped.

#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/FunctionEmitter.h"
#include "Target/FloatLowering.h"

namespace Rux {
using namespace Layout;

namespace {
/// 64-bit precision (PC = 11), round to nearest (RC = 00), every exception masked.
constexpr std::uint16_t ExtendedPrecisionControlWord = 0x037F;

/// The bytes `fstp tbyte` leaves untouched at the end of a 16-byte float80 slot.
constexpr std::int32_t Float80PaddingOffset = 10;

[[nodiscard]] bool IsFloat80(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Float80;
}

/// A signed integer `fild` converts exactly once it is widened to 64 bits.
[[nodiscard]] bool IsFildSource(const TypeRef &type) {
    return type.IsInteger() && type.IsSigned() && SizeOf(type) <= 8;
}

/// Moves values between frame slots and the x87 register stack for one instruction.
class X87Stack {
public:
    X87Stack(const X64Enc &encoder, const X86_64FramePlan &framePlan, X86_64FunctionEmitterHooks &hooks)
        : encoder(encoder)
        , framePlan(framePlan)
        , hooks(hooks)
        , controlWords(-framePlan.WideTemporaryOffset(0))
        , spill(-framePlan.WideTemporaryOffset(1)) {
    }

    /// Saves the caller's control word and selects 64-bit precision with round to nearest.
    void BeginRounding() const {
        encoder.Fnstcw(controlWords);
        encoder.MovWordStoreImm16(controlWords + 2, ExtendedPrecisionControlWord);
        encoder.Fldcw(controlWords + 2);
    }

    /// Restores the control word `BeginRounding` saved.
    void EndRounding() const {
        encoder.Fldcw(controlWords);
    }

    /// Pushes `reg` onto the x87 stack: a float80 as it is, a float32 or float64 widened exactly, and a signed integer
    /// through `fild` after sign-extending it to 64 bits in the spill slot.
    void Push(const LirReg reg, const TypeRef &type) const {
        const bool physical = framePlan.PhysicalRegisters().contains(reg);
        if (IsFloat80(type)) {
            encoder.FldTbyte(Slot(reg));
            return;
        }
        if (IsFloat(type)) {
            // A machine float only reaches a general-purpose register by its bits; `fld` needs it in memory.
            const std::int32_t address = physical ? Spill(reg, type) : Slot(reg);
            type.kind == TypeRef::Kind::Float32 ? encoder.FldDword(address) : encoder.FldQword(address);
            return;
        }
        encoder.FildQword(!physical && SizeOf(type) == 8 ? Slot(reg) : Spill(reg, type));
    }

    /// Pops the x87 top into `reg`, rounding to `type` under the current control word. A float80 destination gets its
    /// six padding bytes zeroed, so the slot holds the canonical storage `SoftwareFloat` produces.
    void Pop(const LirReg reg, const TypeRef &type) const {
        if (IsFloat80(type)) {
            const std::int32_t slot = Slot(reg);
            encoder.FstpTbyte(slot);
            encoder.MovWordStoreImm16(slot + Float80PaddingOffset, 0);
            encoder.MovDwordStoreImm32(slot + Float80PaddingOffset + 2, 0);
            return;
        }
        const bool float32 = type.kind == TypeRef::Kind::Float32;
        if (!framePlan.PhysicalRegisters().contains(reg)) {
            float32 ? encoder.FstpDword(Slot(reg)) : encoder.FstpQword(Slot(reg));
            return;
        }
        float32 ? encoder.FstpDword(spill) : encoder.FstpQword(spill);
        float32 ? encoder.MovEaxLoad(spill) : encoder.MovRaxLoad(spill);
        hooks.StoreA(reg, type);
    }

private:
    [[nodiscard]] std::int32_t Slot(const LirReg reg) const {
        return -framePlan.SlotOffsets().at(reg);
    }

    /// Writes `reg` to the spill slot as a 64-bit value and returns the slot. An integer is sign-extended; a float
    /// keeps its raw bits in the low bytes.
    [[nodiscard]] std::int32_t Spill(const LirReg reg, const TypeRef &type) const {
        hooks.LoadA(reg, type);
        encoder.MovRaxStore(spill);
        return spill;
    }

    const X64Enc &encoder;
    const X86_64FramePlan &framePlan;
    X86_64FunctionEmitterHooks &hooks;
    std::int32_t controlWords;
    std::int32_t spill;
};

/// Turns the flags `fucomip` left into a 0/1 in AL. They read like UCOMISD's, so these are the SSE comparison
/// patterns: an unordered result sets PF, which makes every comparison but `!=` false.
void SetComparison(const X64Enc &encoder, const LirOpcode op) {
    switch (op) {
    case LirOpcode::CmpEq:
        encoder.SeteAl();
        break;
    case LirOpcode::CmpNe:
        encoder.SetneAl();
        encoder.SetpDl();
        encoder.OrAlDl();
        return;
    case LirOpcode::CmpLt:
        encoder.SetbAl();
        break;
    case LirOpcode::CmpLe:
        encoder.SetbeAl();
        break;
    case LirOpcode::CmpGt:
        encoder.SetaAl();
        break;
    default:
        encoder.SetaeAl();
        break;
    }
    encoder.SetnpDl();
    encoder.AndAlDl();
}

/// Whether `instruction` is a float32 or float64 remainder, which x86-64 computes with `fprem` whether or not float80
/// is lowered to x87: SSE has no remainder instruction, and `x - trunc(x / y) * y` rounds twice.
[[nodiscard]] bool IsNativeFloatRemainder(const LirInstr &instruction) {
    return instruction.op == LirOpcode::Mod && IsFloat(instruction.type) && instruction.srcs.size() == 2;
}

/// `dst = srcs[0] % srcs[1]`, truncated like C fmod. ST(1) = divisor, ST(0) = dividend. Each `fprem` reduces the
/// exponent gap by at most 63 and leaves C2 set while more remains. The final remainder is exact, and it is
/// representable in the operands' own format, so neither the precision control nor the store rounds it.
void EmitRemainder(const X64Enc &encoder, const X87Stack &x87, const LirInstr &instruction) {
    x87.Push(instruction.srcs[1], instruction.type);
    x87.Push(instruction.srcs[0], instruction.type);
    const std::uint32_t loop = encoder.Size();
    encoder.Fprem();
    encoder.FnstswAx();
    encoder.TestAhImm8(0x04);
    std::uint32_t repeat = 0;
    encoder.Jnz(repeat);
    encoder.Patch32(repeat, static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(repeat + 4));
    encoder.FstpSt(1); // drop the divisor, keeping the remainder
    x87.Pop(instruction.dst, instruction.type);
}
} // namespace

bool X86_64FunctionEmitter::EmitX87(const LirInstr &instruction) {
    if (instruction.dst == LirNoReg || instruction.srcs.empty()) {
        return false;
    }
    const X87Stack x87(encoder, framePlan, hooks);
    if (IsNativeFloatRemainder(instruction)) {
        EmitRemainder(encoder, x87, instruction);
        return true;
    }
    if (Target::FloatLoweringFor(TypeRef::Kind::Float80, Target::Arch::X86_64) != Target::FloatLowering::X87) {
        return false;
    }
    const auto &types = framePlan.RegisterTypes();
    const TypeRef sourceType = types.contains(instruction.srcs[0]) ? types.at(instruction.srcs[0]) : instruction.type;
    const TypeRef &type = instruction.type;

    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::Div:
        if (!IsFloat80(type) || instruction.srcs.size() != 2) {
            return false;
        }
        x87.BeginRounding();
        // ST(1) = left, ST(0) = right; each popping form computes ST(1) op ST(0).
        x87.Push(instruction.srcs[0], type);
        x87.Push(instruction.srcs[1], type);
        if (instruction.op == LirOpcode::Add) {
            encoder.FaddpSt1();
        }
        else if (instruction.op == LirOpcode::Sub) {
            encoder.FsubpSt1();
        }
        else if (instruction.op == LirOpcode::Mul) {
            encoder.FmulpSt1();
        }
        else {
            encoder.FdivpSt1();
        }
        x87.Pop(instruction.dst, type);
        x87.EndRounding();
        return true;
    case LirOpcode::Mod:
        if (!IsFloat80(type) || instruction.srcs.size() != 2) {
            return false;
        }
        EmitRemainder(encoder, x87, instruction);
        return true;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        if (!IsFloat80(sourceType) || instruction.srcs.size() != 2) {
            return false;
        }
        // ST(0) = left, ST(1) = right, so the flags describe `left op right` as UCOMISD xmm0, xmm1 would.
        x87.Push(instruction.srcs[1], sourceType);
        x87.Push(instruction.srcs[0], sourceType);
        encoder.FucomipSt(1);
        encoder.FstpSt(0);
        SetComparison(encoder, instruction.op);
        encoder.MovzxRaxAl();
        hooks.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    case LirOpcode::Cast:
        if (IsFloat80(type) && (IsFloat(sourceType) || IsFildSource(sourceType))) {
            // Every float32, float64 and 64-bit signed integer is exact in float80: nothing rounds.
            x87.Push(instruction.srcs[0], sourceType);
            x87.Pop(instruction.dst, type);
            return true;
        }
        if (IsFloat80(sourceType) && IsFloat(type)) {
            x87.BeginRounding();
            x87.Push(instruction.srcs[0], sourceType);
            x87.Pop(instruction.dst, type);
            x87.EndRounding();
            return true;
        }
        return false;
    default:
        return false;
    }
}
} // namespace Rux
