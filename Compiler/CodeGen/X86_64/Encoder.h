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
    void MovRaxLoad(const int32_t d) const {
        Byte(0x48);
        Byte(0x8B);
        Byte(0x85);
        Dword(u(d));
    }

    void MovRaxStore(const int32_t d) const {
        Byte(0x48);
        Byte(0x89);
        Byte(0x85);
        Dword(u(d));
    }

    void MovRaxStoreRsp(const int32_t d) const {
        Byte(0x48);
        Byte(0x89);
        Byte(0x84);
        Byte(0x24);
        Dword(u(d));
    }

    void MovEaxLoad(const int32_t d) const {
        Byte(0x8B);
        Byte(0x85);
        Dword(u(d));
    }

    void MovEaxStore(const int32_t d) const {
        Byte(0x89);
        Byte(0x85);
        Dword(u(d));
    }

    void MovzxRaxWord(const int32_t d) const {
        Byte(0x48);
        Byte(0x0F);
        Byte(0xB7);
        Byte(0x85);
        Dword(u(d));
    }

    void MovzxRaxByte(const int32_t d) const {
        Byte(0x48);
        Byte(0x0F);
        Byte(0xB6);
        Byte(0x85);
        Dword(u(d));
    }

    void MovsxdRaxDword(const int32_t d) const {
        Byte(0x48);
        Byte(0x63);
        Byte(0x85);
        Dword(u(d));
    }

    void MovsxRaxWord(const int32_t d) const {
        Byte(0x48);
        Byte(0x0F);
        Byte(0xBF);
        Byte(0x85);
        Dword(u(d));
    }

    void MovsxRaxByte(const int32_t d) const {
        Byte(0x48);
        Byte(0x0F);
        Byte(0xBE);
        Byte(0x85);
        Dword(u(d));
    }

    void MovAxStore(const int32_t d) const {
        Byte(0x66);
        Byte(0x89);
        Byte(0x85);
        Dword(u(d));
    }

    void MovAlStore(const int32_t d) const {
        Byte(0x88);
        Byte(0x85);
        Dword(u(d));
    }

    // R10 ↔ [RBP + disp32]
    void MovR10Load(const int32_t d) const {
        Byte(0x4C);
        Byte(0x8B);
        Byte(0x95);
        Dword(u(d));
    }

    void MovR10Store(const int32_t d) const {
        Byte(0x4C);
        Byte(0x89);
        Byte(0x95);
        Dword(u(d));
    }

    void MovR10Rax() const {
        Byte(0x49);
        Byte(0x89);
        Byte(0xC2);
    }

    void MovzxR10Word(const int32_t d) const {
        Byte(0x4C);
        Byte(0x0F);
        Byte(0xB7);
        Byte(0x95);
        Dword(u(d));
    }

    void MovzxR10Byte(const int32_t d) const {
        Byte(0x4C);
        Byte(0x0F);
        Byte(0xB6);
        Byte(0x95);
        Dword(u(d));
    }

    void MovsxdR10Dword(const int32_t d) const {
        Byte(0x4C);
        Byte(0x63);
        Byte(0x95);
        Dword(u(d));
    }

    void MovsxR10Word(const int32_t d) const {
        Byte(0x4C);
        Byte(0x0F);
        Byte(0xBF);
        Byte(0x95);
        Dword(u(d));
    }

    void MovsxR10Byte(const int32_t d) const {
        Byte(0x4C);
        Byte(0x0F);
        Byte(0xBE);
        Byte(0x95);
        Dword(u(d));
    }

    void MovR10dLoad(const int32_t d) const {
        Byte(0x44);
        Byte(0x8B);
        Byte(0x95);
        Dword(u(d));
    }

    // R11 ↔ [RBP + disp32]
    void MovR11Load(const int32_t d) const {
        Byte(0x4C);
        Byte(0x8B);
        Byte(0x9D);
        Dword(u(d));
    }

    void MovR11Store(const int32_t d) const {
        Byte(0x4C);
        Byte(0x89);
        Byte(0x9D);
        Dword(u(d));
    }

    // Registers loaded from [r10 + disp32].
    void MovRdxR10Load(const int32_t d = 0) const {
        Byte(0x49);
        Byte(0x8B);
        Byte(0x92);
        Dword(u(d));
    }

    void MovR8R10Load(const int32_t d = 0) const {
        Byte(0x4D);
        Byte(0x8B);
        Byte(0x82);
        Dword(u(d));
    }

    void MovRsiR10Load(const int32_t d = 0) const {
        Byte(0x49);
        Byte(0x8B);
        Byte(0xB2);
        Dword(u(d));
    }

    // RCX ↔ stack (for shift count)
    void MovRcxLoad(const int32_t d) const {
        Byte(0x48);
        Byte(0x8B);
        Byte(0x8D);
        Dword(u(d));
    }

    // ABI arg regs ↔ [RBP + disp32]
    // argIdx: 0=RDI,1=RSI,2=RDX,3=RCX,4=R8,5=R9
    void MovArgLoad(const int idx, int32_t d) const {
        static const uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x4C, 0x4C};
        static const uint8_t modrm[] = {0xBD, 0xB5, 0x95, 0x8D, 0x85, 0x8D};
        Byte(rex[idx]);
        Byte(0x8B);
        Byte(modrm[idx]);
        Dword(u(d));
    }

    void MovArgStore(const int idx, int32_t d) const {
        static const uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x4C, 0x4C};
        static const uint8_t modrm[] = {0xBD, 0xB5, 0x95, 0x8D, 0x85, 0x8D};
        Byte(rex[idx]);
        Byte(0x89);
        Byte(modrm[idx]);
        Dword(u(d));
    }

    void MovArgRax(const int idx) const {
        static constexpr uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x49, 0x49};
        static constexpr uint8_t modrm[] = {0xC7, 0xC6, 0xC2, 0xC1, 0xC0, 0xC1};
        Byte(rex[idx]);
        Byte(0x89);
        Byte(modrm[idx]);
    }

    // Win64 ABI arg regs ↔ [RBP + disp32]
    // argIdx: 0=RCX,1=RDX,2=R8,3=R9
    void MovArgLoadWin64(const int idx, const int32_t d) const {
        static constexpr uint8_t rex[] = {0x48, 0x48, 0x4C, 0x4C};
        static constexpr uint8_t modrm[] = {0x8D, 0x95, 0x85, 0x8D};
        if (idx >= 4) {
            return;
        }
        Byte(rex[idx]);
        Byte(0x8B);
        Byte(modrm[idx]);
        Dword(u(d));
    }

    void MovArgStoreWin64(const int idx, const int32_t d) const {
        static constexpr uint8_t rex[] = {0x48, 0x48, 0x4C, 0x4C};
        static constexpr uint8_t modrm[] = {0x8D, 0x95, 0x85, 0x8D};
        if (idx >= 4) {
            return;
        }
        Byte(rex[idx]);
        Byte(0x89);
        Byte(modrm[idx]);
        Dword(u(d));
    }

    void MovRaxArgWin64(const int idx) const {
        switch (idx) {
        case 0:
            Byte(0x48);
            Byte(0x89);
            Byte(0xC8); // mov rax, rcx
            break;
        case 1:
            Byte(0x48);
            Byte(0x89);
            Byte(0xD0); // mov rax, rdx
            break;
        case 2:
            Byte(0x4C);
            Byte(0x89);
            Byte(0xC0); // mov rax, r8
            break;
        case 3:
            Byte(0x4C);
            Byte(0x89);
            Byte(0xC8); // mov rax, r9
            break;
        default:
            break;
        }
    }

    void MovArgWin64Rax(const int idx) const {
        switch (idx) {
        case 0:
            Byte(0x48);
            Byte(0x89);
            Byte(0xC1); // mov rcx, rax
            break;
        case 1:
            Byte(0x48);
            Byte(0x89);
            Byte(0xC2); // mov rdx, rax
            break;
        case 2:
            Byte(0x49);
            Byte(0x89);
            Byte(0xC0); // mov r8, rax
            break;
        case 3:
            Byte(0x49);
            Byte(0x89);
            Byte(0xC1); // mov r9, rax
            break;
        default:
            break;
        }
    }

    void LeaArgStackWin64(const int idx, const int32_t d) const {
        static constexpr uint8_t rex[] = {0x48, 0x48, 0x4C, 0x4C};
        static constexpr uint8_t modrm[] = {0x8D, 0x95, 0x85, 0x8D};
        if (idx >= 4) {
            return;
        }
        Byte(rex[idx]);
        Byte(0x8D);
        Byte(modrm[idx]);
        Dword(u(d));
    }

    void LeaR9Rsp(const int32_t d) const {
        Byte(0x4C);
        Byte(0x8D);
        Byte(0x8C);
        Byte(0x24);
        Dword(u(d));
    }

    void MovQwordRspImm32(const int32_t d, const int32_t value) const {
        Byte(0x48);
        Byte(0xC7);
        Byte(0x84);
        Byte(0x24);
        Dword(u(d));
        Dword(u(value));
    }

    void MovR10ArgWin64(const int idx) const {
        switch (idx) {
        case 0:
            Byte(0x49);
            Byte(0x89);
            Byte(0xCA);
            break; // mov r10, rcx
        case 1:
            Byte(0x49);
            Byte(0x89);
            Byte(0xD2);
            break; // mov r10, rdx
        case 2:
            Byte(0x4D);
            Byte(0x89);
            Byte(0xC2);
            break; // mov r10, r8
        case 3:
            Byte(0x4D);
            Byte(0x89);
            Byte(0xCA);
            break; // mov r10, r9
        default:
            break;
        }
    }

    void SubRspShadow() const {
        Byte(0x48);
        Byte(0x83);
        Byte(0xEC);
        Byte(0x20);
    }

    void AddRspShadow() const {
        Byte(0x48);
        Byte(0x83);
        Byte(0xC4);
        Byte(0x20);
    }

    // XMM arg regs ↔ [RBP + disp32] (N = 0..7)
    // MOVSS xmmN, [rbp + d]
    void MovssXmmNLoad(int n, int32_t d) const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x10);
        Byte(static_cast<uint8_t>(0x80 | (n << 3) | 5));
        Dword(u(d));
    }

    // MOVSD xmmN, [rbp + d]
    void MovsdXmmNLoad(int n, int32_t d) const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x10);
        Byte(static_cast<uint8_t>(0x80 | (n << 3) | 5));
        Dword(u(d));
    }

    void MovssXmm0StoreRsp(const int32_t d) const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x11);
        Byte(0x84);
        Byte(0x24);
        Dword(u(d));
    }

    void MovsdXmm0StoreRsp(const int32_t d) const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x11);
        Byte(0x84);
        Byte(0x24);
        Dword(u(d));
    }

    // XMM0 / XMM1 ↔ [RBP + disp32]
    void MovssXmm0Load(int32_t d) const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x85);
        Dword(u(d));
    }

    void MovsdXmm0Load(int32_t d) const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x85);
        Dword(u(d));
    }

    void MovssXmm1Load(int32_t d) const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x8D);
        Dword(u(d));
    }

    void MovsdXmm1Load(int32_t d) const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x8D);
        Dword(u(d));
    }

    void MovssXmm0Store(int32_t d) const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x11);
        Byte(0x85);
        Dword(u(d));
    }

    void MovsdXmm0Store(int32_t d) const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x11);
        Byte(0x85);
        Dword(u(d));
    }

    void MovssXmm1Store(int32_t d) const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x11);
        Byte(0x8D);
        Dword(u(d));
    }

    void MovsdXmm1Store(int32_t d) const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x11);
        Byte(0x8D);
        Dword(u(d));
    }

    // XMM0 / XMM1, [RIP + rel32] (RIP-relative rodata load)
    void MovssXmm0Rip(uint32_t &relocOff) const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x05);
        relocOff = Size();
        Dword(0);
    }

    void MovsdXmm0Rip(uint32_t &relocOff) const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x05);
        relocOff = Size();
        Dword(0);
    }

    void MovssXmm1Rip(uint32_t &relocOff) const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x0D);
        relocOff = Size();
        Dword(0);
    }

    void MovsdXmm1Rip(uint32_t &relocOff) const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x0D);
        relocOff = Size();
        Dword(0);
    }

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
    void AddssXmm01() const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x58);
        Byte(0xC1);
    }

    void SubssXmm01() const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x5C);
        Byte(0xC1);
    }

    void MulssXmm01() const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x59);
        Byte(0xC1);
    }

    void DivssXmm01() const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x5E);
        Byte(0xC1);
    }

    void FmodssXmm01() const {
        // xmm2 = x
        Byte(0x0F);
        Byte(0x28);
        Byte(0xD0); // movaps xmm2, xmm0

        // xmm3 = y
        Byte(0x0F);
        Byte(0x28);
        Byte(0xD9); // movaps xmm3, xmm1

        // xmm0 = x / y
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x5E);
        Byte(0xC1); // divss xmm0, xmm1

        // eax = trunc(x / y)
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x2C);
        Byte(0xC0); // cvttss2si eax, xmm0

        // xmm0 = float(eax)
        Byte(0x66);
        Byte(0x0F);
        Byte(0xEF);
        Byte(0xC0); // pxor xmm0, xmm0

        Byte(0xF3);
        Byte(0x0F);
        Byte(0x2A);
        Byte(0xC0); // cvtsi2ss xmm0, eax

        // xmm0 *= y
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x59);
        Byte(0xC3); // mulss xmm0, xmm3

        // xmm2 -= xmm0
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x5C);
        Byte(0xD0); // subss xmm2, xmm0

        // result -> xmm0
        Byte(0x0F);
        Byte(0x28);
        Byte(0xC2); // movaps xmm0, xmm2
    }

    void AddsdXmm01() const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x58);
        Byte(0xC1);
    }

    void SubsdXmm01() const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x5C);
        Byte(0xC1);
    }

    void MulsdXmm01() const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x59);
        Byte(0xC1);
    }

    void DivsdXmm01() const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x5E);
        Byte(0xC1);
    }

    void FmodsdXmm01() const {
        // xmm2 = x
        Byte(0x66);
        Byte(0x0F);
        Byte(0x28);
        Byte(0xD0); // movapd xmm2, xmm0

        // xmm3 = y
        Byte(0x66);
        Byte(0x0F);
        Byte(0x28);
        Byte(0xD9); // movapd xmm3, xmm1

        // xmm0 = x / y
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x5E);
        Byte(0xC1); // divsd xmm0, xmm1

        // rax = trunc(x / y)
        Byte(0xF2);
        Byte(0x48);
        Byte(0x0F);
        Byte(0x2C);
        Byte(0xC0); // cvttsd2si rax, xmm0

        // xmm0 = double(rax)
        Byte(0x66);
        Byte(0x0F);
        Byte(0xEF);
        Byte(0xC0); // pxor xmm0, xmm0

        Byte(0xF2);
        Byte(0x48);
        Byte(0x0F);
        Byte(0x2A);
        Byte(0xC0); // cvtsi2sd xmm0, rax

        // xmm0 *= y
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x59);
        Byte(0xC3); // mulsd xmm0, xmm3

        // xmm2 -= xmm0
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x5C);
        Byte(0xD0); // subsd xmm2, xmm0

        // result -> xmm0
        Byte(0x66);
        Byte(0x0F);
        Byte(0x28);
        Byte(0xC2); // movapd xmm0, xmm2
    }

    // Float compare
    void UcomissXmm01() const {
        Byte(0x0F);
        Byte(0x2E);
        Byte(0xC1);
    }

    void UcomisdXmm01() const {
        Byte(0x66);
        Byte(0x0F);
        Byte(0x2E);
        Byte(0xC1);
    }

    // Float sign negate (XOR with mask)
    void XorpsXmm01() const {
        Byte(0x0F);
        Byte(0x57);
        Byte(0xC1);
    }

    void XorpdXmm01() const {
        Byte(0x66);
        Byte(0x0F);
        Byte(0x57);
        Byte(0xC1);
    }

    // Float conversions
    void Cvtsi2ssXmm0Rax() const {
        Byte(0xF3);
        Byte(0x48);
        Byte(0x0F);
        Byte(0x2A);
        Byte(0xC0);
    }

    void Cvtsi2sdXmm0Rax() const {
        Byte(0xF2);
        Byte(0x48);
        Byte(0x0F);
        Byte(0x2A);
        Byte(0xC0);
    }

    void CvttsssiRaxXmm0() const {
        Byte(0xF3);
        Byte(0x48);
        Byte(0x0F);
        Byte(0x2C);
        Byte(0xC0);
    }

    void CvttsdsiRaxXmm0() const {
        Byte(0xF2);
        Byte(0x48);
        Byte(0x0F);
        Byte(0x2C);
        Byte(0xC0);
    }

    void CvtsssdXmm0() const {
        Byte(0xF3);
        Byte(0x0F);
        Byte(0x5A);
        Byte(0xC0);
    }

    void CvtsdssXmm0() const {
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x5A);
        Byte(0xC0);
    }

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
