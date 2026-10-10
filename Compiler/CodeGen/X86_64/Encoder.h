#pragma once

// x86-64 instruction encoder used by the RCU object-code generator.

#include <cstdint>
#include <vector>

namespace Rux {
struct X86ScalarMachineInstruction;

/// x86-64 binary encoder.
///
/// All accesses to stack slots use [rbp + disp] where disp is negative. disp = -slotMap[vreg]  (i.e., pass negative
/// displacement directly).
class X64Enc {
public:
    explicit X64Enc(std::vector<uint8_t> &buf)
        : out(buf) {
    }

    [[nodiscard]] uint32_t Size() const;

    void Byte(uint8_t b) const;

    void Dword(uint32_t d) const;

    void Qword(uint64_t q) const;

    void Patch32(uint32_t off, int32_t v) const;

    // Prologue / Epilogue
    void PushRbp() const;

    void MovRbpRsp() const;

    void SubRspImm32(int32_t n) const;

    void TouchRsp() const;

    void AddRspImm32(int32_t n) const;

    void Leave() const;

    void Ret() const;

    void Ud2() const;

    // RAX ↔ [RBP + disp32]
    void MovRaxLoad(int32_t d) const;

    void MovRaxStore(int32_t d) const;

    void MovRaxStoreRsp(int32_t d) const;

    void MovEaxLoad(int32_t d) const;

    void MovEaxStore(int32_t d) const;

    void MovzxRaxWord(int32_t d) const;

    void MovzxRaxByte(int32_t d) const;

    void MovsxdRaxDword(int32_t d) const;

    void MovsxRaxWord(int32_t d) const;

    void MovsxRaxByte(int32_t d) const;

    void MovAxStore(int32_t d) const;

    void MovAlStore(int32_t d) const;

    // R10 ↔ [RBP + disp32]
    void MovR10Load(int32_t d) const;

    void MovR10Store(int32_t d) const;

    void MovR10Rax() const;

    void MovzxR10Word(int32_t d) const;

    void MovzxR10Byte(int32_t d) const;

    void MovsxdR10Dword(int32_t d) const;

    void MovsxR10Word(int32_t d) const;

    void MovsxR10Byte(int32_t d) const;

    void MovR10dLoad(int32_t d) const;

    // R11 ↔ [RBP + disp32]
    void MovR11Load(int32_t d) const;

    void MovR11Store(int32_t d) const;

    /// Registers loaded from [r10 + disp32].
    void MovRdxR10Load(int32_t d = 0) const;

    void MovR8R10Load(int32_t d = 0) const;

    void MovRsiR10Load(int32_t d = 0) const;

    // RCX ↔ stack (for shift count)
    void MovRcxLoad(int32_t d) const;

    /// ABI arg regs ↔ [RBP + disp32] argIdx: 0=RDI,1=RSI,2=RDX,3=RCX,4=R8,5=R9
    void MovArgLoad(int idx, int32_t d) const;

    void MovArgStore(int idx, int32_t d) const;

    void MovArgRax(int idx) const;

    /// Win64 ABI arg regs ↔ [RBP + disp32] argIdx: 0=RCX,1=RDX,2=R8,3=R9
    void MovArgLoadWin64(int idx, int32_t d) const;

    void MovArgStoreWin64(int idx, int32_t d) const;

    void MovRaxArgWin64(int idx) const;

    void MovArgWin64Rax(int idx) const;

    void LeaArgStackWin64(int idx, int32_t d) const;

    void LeaR9Rsp(int32_t d) const;

    void MovQwordRspImm32(int32_t d, int32_t value) const;

    void MovR10ArgWin64(int idx) const;

    void SubRspShadow() const;

    void AddRspShadow() const;

    // XMM arg regs ↔ [RBP + disp32] (N = 0..7)

    /// MOVSS xmmN, [rbp + d]
    void MovssXmmNLoad(int n, int32_t d) const;
    void MovXmm(int destination, int source) const;
    void MovXmmSave(int reg, int32_t displacement) const;
    void MovXmmRestore(int reg, int32_t displacement) const;
    void MovFloatBits(int destinationGpr, int sourceXmm, bool wide) const;

    // MOVSD xmmN, [rbp + d]
    void MovsdXmmNLoad(int n, int32_t d) const;

    void MovssXmm0StoreRsp(int32_t d) const;

    void MovsdXmm0StoreRsp(int32_t d) const;

    // XMM0 / XMM1 ↔ [RBP + disp32]
    void MovssXmm0Load(int32_t d) const;

    void MovsdXmm0Load(int32_t d) const;

    void MovssXmm1Load(int32_t d) const;

    void MovsdXmm1Load(int32_t d) const;

    void MovssXmm0Store(int32_t d) const;

    void MovsdXmm0Store(int32_t d) const;

    void MovssXmm1Store(int32_t d) const;

    void MovsdXmm1Store(int32_t d) const;

    // XMM0 / XMM1, [RIP + rel32] (RIP-relative rodata load)
    void MovssXmm0Rip(uint32_t &relocOff) const;

    void MovsdXmm0Rip(uint32_t &relocOff) const;

    void MovssXmm1Rip(uint32_t &relocOff) const;

    void MovsdXmm1Rip(uint32_t &relocOff) const;

    // Immediate loads
    void MovRaxImm64(int64_t v) const;

    void MovEaxImm32(int32_t v) const;

    void MovEdiImm32(const int32_t v) const;

    void MovEdxImm32(const int32_t v) const;

    void MovRsiRax() const;

    void Syscall() const;

    // LEA / MOV rax, [rip + rel32]
    void LeaRaxRip(uint32_t &relocOff) const;

    void MovRaxRip(uint32_t &relocOff) const;

    void LeaRaxStack(int32_t d) const;

    // Register-to-register
    void MovRaxR10() const;

    void MovRcxR11() const;

    void MovRaxRdx() const;

    // Integer arithmetic (RAX op R10 → RAX)
    void AddRaxR10() const;

    void SubRaxR10() const;

    void AndRaxR10() const;

    void OrRaxR10() const;

    void XorRaxR10() const;

    void ImulRaxR10() const;

    void NegRax() const;

    void NotRax() const;

    /// XOR RAX, imm32 (sign-extended). Used for `~` on bools to fold the value back to 0 or 1 (logical NOT), matching
    /// the docs at Web/src/docs/types/bool.md (issue #95).
    void XorRaxImmediate(std::int32_t imm) const;

    // Division
    void Cqo() const;

    void XorRdxRdx() const;

    void IdivR10() const;

    void DivR10() const;

    // Shifts
    void ShlRaxCl() const;
    void ShiftRaxImmediate(unsigned operation, std::uint8_t amount) const;
    void RotateRaxImmediate(bool word64, std::uint8_t amount) const;

    void ShrRaxCl() const;

    void SarRaxCl() const;

    // Comparisons
    void TestRaxRax() const;

    void CmpRaxR10() const;

    void CmpRaxImm32(int32_t v) const;

    void SeteAl() const;

    void SetneAl() const;

    void SetnpDl() const;

    void SetlAl() const;

    void SetleAl() const;

    void SetgAl() const;

    void SetgeAl() const;

    void SetbAl() const;

    void SetbeAl() const;

    void SetaAl() const;

    void SetaeAl() const;

    void MovzxRaxAl() const;

    void MovzxRaxAx() const;

    void MovsxdRaxEax() const;

    void MovsxRaxAl() const;

    void MovsxRaxAx() const;

    void MovEaxEax() const;

    void MovzxR10r10b() const;

    void MovzxR10r10w() const;

    void MovsxdR10r10d() const;

    void MovsxR10r10b() const;

    void MovsxR10r10w() const;

    void MovR10dR10d() const;

    void SetpDl() const;

    void AndAlDl() const;

    void OrAlDl() const;

    // Float arithmetic (XMM0 op XMM1 → XMM0)
    void AddssXmm01() const;

    void SubssXmm01() const;

    void MulssXmm01() const;

    void DivssXmm01() const;

    void AddsdXmm01() const;

    void SubsdXmm01() const;

    void MulsdXmm01() const;

    void DivsdXmm01() const;

    // Float compare
    void UcomissXmm01() const;

    void UcomisdXmm01() const;

    // Float sign negate (XOR with mask)
    void XorpsXmm01() const;

    void XorpdXmm01() const;

    // Float conversions
    void Cvtsi2ssXmm0Rax() const;

    void Cvtsi2sdXmm0Rax() const;

    void CvttsssiRaxXmm0() const;

    void CvttsdsiRaxXmm0() const;

    void CvtsssdXmm0() const;

    void CvtsdssXmm0() const;

    // Control flow
    void Jmp(uint32_t &patchOff) const;

    void Jz(uint32_t &patchOff) const;

    void Jnz(uint32_t &patchOff) const;

    void Je(uint32_t &patchOff) const;

    void Call(uint32_t &relocOff) const;

    void CallR10() const;

    // Aggregate helpers
    void ImulR11R10Imm32(int32_t v) const;

    void AddRaxR11() const;

    void LeaRaxRaxDisp(int32_t v) const;
    /// rax = rax + r10 * 2^shift; shift must be in 0..3.
    void LeaRaxRaxR10(unsigned shift) const;
    void EmitScalarMachineInstruction(const X86ScalarMachineInstruction &instruction) const;

    void PopRbp() const;

    // Callee-saved registers push / pop / moves
    void PushRbx() const;

    void PopRbx() const;

    void PushR12() const;

    void PopR12() const;

    void PushR13() const;

    void PopR13() const;

    void PushR14() const;

    void PopR14() const;

    void PushR15() const;

    void PopR15() const;

    void PushReg(int rIdx) const;

    void PopReg(int rIdx) const;

    // Move RAX from physical register (used by LoadA)
    void MovRaxPhysReg(int rIdx) const;

    // Move R10 from physical register (used by LoadB)
    void MovR10PhysReg(int rIdx) const;

    // Move R11 from physical register (used by Store and other instructions)
    void MovR11PhysReg(int rIdx) const;

    // Move physical register from RAX (used by StoreA)
    void MovPhysRegRax(int rIdx) const;

    // ---- x87 (float80) ---------------------------------------------------------------------------------------------
    // Encoded in EncoderX87.cpp. Mnemonics follow Intel syntax and the Intel SDM, never AT&T: GNU as swaps the meaning
    // of fsub/fsubr and fdiv/fdivr in the register-register forms, so AT&T `fsubp %st, %st(1)` is DE E1, not DE E9.
    //
    // Operand order for X2: `fld a` then `fld b` leaves ST(0) = b and ST(1) = a. FaddpSt1, FsubpSt1, FmulpSt1 and
    // FdivpSt1 then compute `a op b` into ST(1) and pop, leaving only the result; FsubrpSt1 and FdivrpSt1 compute
    // `b op a`. Each memory form uses mod=10 with a disp32, even when the displacement would fit in 8 bits, like the
    // rest of this encoder.

    /// A general-purpose register in hardware encoding order, used as the base of an x87 memory operand. Distinct
    /// from the allocator indices `PushReg` and `MovRaxPhysReg` take.
    enum class Gpr : uint8_t {
        Rax,
        Rcx,
        Rdx,
        Rbx,
        Rsp,
        Rbp,
        Rsi,
        Rdi,
        R8,
        R9,
        R10,
        R11,
        R12,
        R13,
        R14,
        R15
    };

    /// FLD m80fp [rbp + d] (DB /5): push the 80-bit value.
    void FldTbyte(int32_t d) const;

    /// FSTP m80fp [rbp + d] (DB /7): store ST(0) as 10 bytes and pop. Bytes 10-15 of a float80 slot are not written.
    void FstpTbyte(int32_t d) const;

    /// FLD m64fp [rbp + d] (DD /0): push a float64, widened exactly.
    void FldQword(int32_t d) const;

    /// FSTP m64fp [rbp + d] (DD /3): round ST(0) to float64 under the control word, store and pop.
    void FstpQword(int32_t d) const;

    /// FLD m32fp [rbp + d] (D9 /0): push a float32, widened exactly.
    void FldDword(int32_t d) const;

    /// FSTP m32fp [rbp + d] (D9 /3): round ST(0) to float32 under the control word, store and pop.
    void FstpDword(int32_t d) const;

    /// FILD m64int [rbp + d] (DF /5): push a signed 64-bit integer, exactly.
    void FildQword(int32_t d) const;

    /// FILD m32int [rbp + d] (DB /0): push a signed 32-bit integer, exactly.
    void FildDword(int32_t d) const;

    /// The same eight memory forms addressed as [base + d], with a REX.B prefix for r8-r15 and a SIB byte for rsp and
    /// r12.
    void FldTbyteAt(Gpr base, int32_t d) const;

    void FstpTbyteAt(Gpr base, int32_t d) const;

    void FldQwordAt(Gpr base, int32_t d) const;

    void FstpQwordAt(Gpr base, int32_t d) const;

    void FldDwordAt(Gpr base, int32_t d) const;

    void FstpDwordAt(Gpr base, int32_t d) const;

    void FildQwordAt(Gpr base, int32_t d) const;

    void FildDwordAt(Gpr base, int32_t d) const;

    /// FADDP ST(1), ST(0) (DE C1): ST(1) = ST(1) + ST(0), then pop.
    void FaddpSt1() const;

    /// FSUBP ST(1), ST(0) (DE E9): ST(1) = ST(1) - ST(0), then pop. ST(0) is the subtrahend.
    void FsubpSt1() const;

    /// FSUBRP ST(1), ST(0) (DE E1): ST(1) = ST(0) - ST(1), then pop. ST(1) is the subtrahend.
    void FsubrpSt1() const;

    /// FMULP ST(1), ST(0) (DE C9): ST(1) = ST(1) * ST(0), then pop.
    void FmulpSt1() const;

    /// FDIVP ST(1), ST(0) (DE F9): ST(1) = ST(1) / ST(0), then pop. ST(0) is the divisor.
    void FdivpSt1() const;

    /// FDIVRP ST(1), ST(0) (DE F1): ST(1) = ST(0) / ST(1), then pop. ST(1) is the divisor.
    void FdivrpSt1() const;

    /// FPREM (D9 F8): ST(0) = partial remainder of ST(0) / ST(1), truncating like C fmod; nothing is popped. C2 in the
    /// status word stays set while the reduction is incomplete, so loop `Fprem; FnstswAx; TestAhImm8(0x04); Jnz`.
    void Fprem() const;

    /// FCHS (D9 E0): ST(0) = -ST(0), flipping only the sign bit.
    void Fchs() const;

    /// FNSTSW AX (DF E0): AX = the x87 status word, so AH bits 0, 2 and 6 hold C0, C2 and C3.
    void FnstswAx() const;

    /// FUCOMIP ST(0), ST(i) (DF E8+i): compare ST(0) with ST(i) into ZF/PF/CF like UCOMISD with ST(0) as the left
    /// operand (CF = ST(0) < ST(i), ZF = equal, PF/ZF/CF all set = unordered), then pop once. Quiet NaNs do not raise.
    void FucomipSt(int i) const;

    /// FSTP ST(i) (DD D8+i): copy ST(0) into ST(i), then pop. FSTP ST(0) just discards the top; FSTP ST(1) discards
    /// ST(1) and keeps ST(0).
    void FstpSt(int i) const;

    /// FLD ST(i) (D9 C0+i): push a copy of ST(i).
    void FldSt(int i) const;

    /// FXCH ST(i) (D9 C8+i): swap ST(0) and ST(i).
    void FxchSt(int i) const;

    /// FNSTCW m16 [rbp + d] (D9 /7): store the control word without checking pending exceptions.
    void Fnstcw(int32_t d) const;

    /// FLDCW m16 [rbp + d] (D9 /5): load the control word.
    void Fldcw(int32_t d) const;

    /// MOV word [rbp + d], imm16 (66 C7 /0): e.g. the 0x037F control word (64-bit precision, round to nearest, all
    /// exceptions masked), or zeroing bytes 10-11 of a float80 slot.
    void MovWordStoreImm16(int32_t d, uint16_t v) const;

    /// MOV dword [rbp + d], imm32 (C7 /0): e.g. zeroing bytes 12-15 of a float80 slot.
    void MovDwordStoreImm32(int32_t d, int32_t v) const;

    /// TEST AH, imm8 (F6 C4 ib): e.g. 0x04 tests C2 after FnstswAx. Never REX-prefixed, since REX makes AH unreachable.
    void TestAhImm8(uint8_t v) const;
    // ---- end x87 ---------------------------------------------------------------------------------------------------

private:
    std::vector<uint8_t> &out;

    static uint32_t u(const int32_t v);
};
} // namespace Rux
