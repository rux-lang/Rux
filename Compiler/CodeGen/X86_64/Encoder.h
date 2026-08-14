#pragma once

// x86-64 instruction encoder used by the RCU object-code generator.

#include <cstdint>
#include <vector>

namespace Rux {
// x86-64 binary encoder
// All accesses to stack slots use [rbp + disp] where disp is negative.
// disp = -slotMap[vreg]  (i.e., pass negative displacement directly).
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

    // Registers loaded from [r10 + disp32].
    void MovRdxR10Load(int32_t d = 0) const;

    void MovR8R10Load(int32_t d = 0) const;

    void MovRsiR10Load(int32_t d = 0) const;

    // RCX ↔ stack (for shift count)
    void MovRcxLoad(int32_t d) const;

    // ABI arg regs ↔ [RBP + disp32]
    // argIdx: 0=RDI,1=RSI,2=RDX,3=RCX,4=R8,5=R9
    void MovArgLoad(int idx, int32_t d) const;

    void MovArgStore(int idx, int32_t d) const;

    void MovArgRax(int idx) const;

    // Win64 ABI arg regs ↔ [RBP + disp32]
    // argIdx: 0=RCX,1=RDX,2=R8,3=R9
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
    // MOVSS xmmN, [rbp + d]
    void MovssXmmNLoad(int n, int32_t d) const;

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

    // XOR RAX, imm32 (sign-extended). Used for `~` on bools to fold
    // the value back to 0 or 1 (logical NOT), matching the docs at
    // Web/src/docs/types/bool.md (issue #95).
    void XorRaxImmediate(std::int32_t imm) const;

    // Division
    void Cqo() const;

    void XorRdxRdx() const;

    void IdivR10() const;

    void DivR10() const;

    // Shifts
    void ShlRaxCl() const;

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

    void FmodssXmm01() const;

    void AddsdXmm01() const;

    void SubsdXmm01() const;

    void MulsdXmm01() const;

    void DivsdXmm01() const;

    void FmodsdXmm01() const;

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

private:
    std::vector<uint8_t> &out;

    static uint32_t u(const int32_t v);
};
} // namespace Rux
