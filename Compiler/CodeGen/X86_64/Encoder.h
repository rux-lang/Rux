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

    [[nodiscard]] uint32_t Size() const {
        return static_cast<uint32_t>(out.size());
    }

    void Byte(uint8_t b) const {
        out.push_back(b);
    }

    void Dword(uint32_t d) const {
        out.push_back(d & 0xFF);
        out.push_back((d >> 8) & 0xFF);
        out.push_back((d >> 16) & 0xFF);
        out.push_back((d >> 24) & 0xFF);
    }

    void Qword(uint64_t q) const {
        for (int i = 0; i < 8; ++i) {
            out.push_back(q & 0xFF);
            q >>= 8;
        }
    }

    void Patch32(uint32_t off, int32_t v) const {
        out[off] = v & 0xFF;
        out[off + 1] = (v >> 8) & 0xFF;
        out[off + 2] = (v >> 16) & 0xFF;
        out[off + 3] = (v >> 24) & 0xFF;
    }

    // Prologue / Epilogue
    void PushRbp() const {
        Byte(0x55);
    }

    void MovRbpRsp() const {
        Byte(0x48);
        Byte(0x89);
        Byte(0xE5);
    }

    void SubRspImm32(int32_t n) const {
        Byte(0x48);
        Byte(0x81);
        Byte(0xEC);
        Dword(static_cast<uint32_t>(n));
    }

    void TouchRsp() const {
        Byte(0x48);
        Byte(0x85);
        Byte(0x04);
        Byte(0x24); // test qword [rsp], rax
    }

    void AddRspImm32(int32_t n) const {
        Byte(0x48);
        Byte(0x81);
        Byte(0xC4);
        Dword(static_cast<uint32_t>(n));
    }

    void Leave() const {
        Byte(0xC9);
    }

    void Ret() const {
        Byte(0xC3);
    }

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
    void MovRaxImm64(int64_t v) const {
        Byte(0x48);
        Byte(0xB8);
        Qword(static_cast<uint64_t>(v));
    }

    void MovEaxImm32(int32_t v) const {
        Byte(0xB8);
        Dword(static_cast<uint32_t>(v));
    }

    // LEA / MOV rax, [rip + rel32]
    void LeaRaxRip(uint32_t &relocOff) const {
        Byte(0x48);
        Byte(0x8D);
        Byte(0x05);
        relocOff = Size();
        Dword(0);
    }

    void MovRaxRip(uint32_t &relocOff) const {
        Byte(0x48);
        Byte(0x8B);
        Byte(0x05);
        relocOff = Size();
        Dword(0);
    }

    void LeaRaxStack(int32_t d) const {
        Byte(0x48);
        Byte(0x8D);
        Byte(0x85);
        Dword(u(d));
    }

    // Register-to-register
    void MovRaxR10() const {
        Byte(0x4C);
        Byte(0x89);
        Byte(0xD0);
    } // mov rax, r10

    void MovRcxR11() const {
        Byte(0x4C);
        Byte(0x89);
        Byte(0xD9);
    } // mov rcx, r11

    void MovRaxRdx() const {
        Byte(0x48);
        Byte(0x8B);
        Byte(0xC2);
    } // mov rax, rdx

    // Integer arithmetic (RAX op R10 → RAX)
    void AddRaxR10() const {
        Byte(0x4C);
        Byte(0x01);
        Byte(0xD0);
    }

    void SubRaxR10() const {
        Byte(0x4C);
        Byte(0x29);
        Byte(0xD0);
    }

    void AndRaxR10() const {
        Byte(0x4C);
        Byte(0x21);
        Byte(0xD0);
    }

    void OrRaxR10() const {
        Byte(0x4C);
        Byte(0x09);
        Byte(0xD0);
    }

    void XorRaxR10() const {
        Byte(0x4C);
        Byte(0x31);
        Byte(0xD0);
    }

    void ImulRaxR10() const {
        Byte(0x49);
        Byte(0x0F);
        Byte(0xAF);
        Byte(0xC2);
    }

    void NegRax() const {
        Byte(0x48);
        Byte(0xF7);
        Byte(0xD8);
    }

    void NotRax() const {
        Byte(0x48);
        Byte(0xF7);
        Byte(0xD0);
    }

    // XOR RAX, imm32 (sign-extended). Used for `~` on bools to fold
    // the value back to 0 or 1 (logical NOT), matching the docs at
    // Web/src/docs/types/bool.md (issue #95).
    void XorRaxImmediate(std::int32_t imm) const {
        Byte(0x48);
        Byte(0x81);
        Byte(0xF0);
        Dword(static_cast<std::uint32_t>(imm));
    }

    // Division
    void Cqo() const {
        Byte(0x48);
        Byte(0x99);
    }

    void XorRdxRdx() const {
        Byte(0x48);
        Byte(0x31);
        Byte(0xD2);
    }

    void IdivR10() const {
        Byte(0x49);
        Byte(0xF7);
        Byte(0xFA);
    }

    void DivR10() const {
        Byte(0x49);
        Byte(0xF7);
        Byte(0xF2);
    }

    // Shifts
    void ShlRaxCl() const {
        Byte(0x48);
        Byte(0xD3);
        Byte(0xE0);
    }

    void ShrRaxCl() const {
        Byte(0x48);
        Byte(0xD3);
        Byte(0xE8);
    }

    void SarRaxCl() const {
        Byte(0x48);
        Byte(0xD3);
        Byte(0xF8);
    }

    // Comparisons
    void TestRaxRax() const {
        Byte(0x48);
        Byte(0x85);
        Byte(0xC0);
    }

    void CmpRaxR10() const {
        Byte(0x4C);
        Byte(0x39);
        Byte(0xD0);
    }

    void CmpRaxImm32(int32_t v) const {
        Byte(0x48);
        Byte(0x81);
        Byte(0xF8);
        Dword(u(v));
    }

    void SeteAl() const {
        Byte(0x0F);
        Byte(0x94);
        Byte(0xC0); // sete al
    }

    void SetneAl() const {
        Byte(0x0F);
        Byte(0x95);
        Byte(0xC0); // setne al
    }

    void SetnpDl() const {
        Byte(0x0F);
        Byte(0x9B);
        Byte(0xC2); // setnp dl
    }

    void SetlAl() const {
        Byte(0x0F);
        Byte(0x9C);
        Byte(0xC0);
    }

    void SetleAl() const {
        Byte(0x0F);
        Byte(0x9E);
        Byte(0xC0);
    }

    void SetgAl() const {
        Byte(0x0F);
        Byte(0x9F);
        Byte(0xC0);
    }

    void SetgeAl() const {
        Byte(0x0F);
        Byte(0x9D);
        Byte(0xC0);
    }

    void SetbAl() const {
        Byte(0x0F);
        Byte(0x92);
        Byte(0xC0);
    }

    void SetbeAl() const {
        Byte(0x0F);
        Byte(0x96);
        Byte(0xC0);
    }

    void SetaAl() const {
        Byte(0x0F);
        Byte(0x97);
        Byte(0xC0);
    }

    void SetaeAl() const {
        Byte(0x0F);
        Byte(0x93);
        Byte(0xC0);
    }

    void MovzxRaxAl() const {
        Byte(0x48);
        Byte(0x0F);
        Byte(0xB6);
        Byte(0xC0);
    }

    void MovzxRaxAx() const {
        Byte(0x48);
        Byte(0x0F);
        Byte(0xB7);
        Byte(0xC0);
    }

    void MovsxdRaxEax() const {
        Byte(0x48);
        Byte(0x63);
        Byte(0xC0);
    }

    void MovsxRaxAl() const {
        Byte(0x48);
        Byte(0x0F);
        Byte(0xBE);
        Byte(0xC0);
    }

    void MovsxRaxAx() const {
        Byte(0x48);
        Byte(0x0F);
        Byte(0xBF);
        Byte(0xC0);
    }

    void MovEaxEax() const {
        Byte(0x89);
        Byte(0xC0);
    }

    void MovzxR10r10b() const {
        Byte(0x4D);
        Byte(0x0F);
        Byte(0xB6);
        Byte(0xD2);
    }

    void MovzxR10r10w() const {
        Byte(0x4D);
        Byte(0x0F);
        Byte(0xB7);
        Byte(0xD2);
    }

    void MovsxdR10r10d() const {
        Byte(0x4D);
        Byte(0x63);
        Byte(0xD2);
    }

    void MovsxR10r10b() const {
        Byte(0x4D);
        Byte(0x0F);
        Byte(0xBE);
        Byte(0xD2);
    }

    void MovsxR10r10w() const {
        Byte(0x4D);
        Byte(0x0F);
        Byte(0xBF);
        Byte(0xD2);
    }

    void MovR10dR10d() const {
        Byte(0x45);
        Byte(0x89);
        Byte(0xD2);
    }

    void SetpDl() const {
        Byte(0x0F);
        Byte(0x9A);
        Byte(0xC2); // setp dl
    }

    void AndAlDl() const {
        Byte(0x20);
        Byte(0xD0); // and al, dl
    }

    void OrAlDl() const {
        Byte(0x08);
        Byte(0xD0); // or al, dl
    }

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
    void Jmp(uint32_t &patchOff) const {
        Byte(0xE9);
        patchOff = Size();
        Dword(0);
    }

    void Jz(uint32_t &patchOff) const {
        Byte(0x0F);
        Byte(0x84);
        patchOff = Size();
        Dword(0);
    }

    void Jnz(uint32_t &patchOff) const {
        Byte(0x0F);
        Byte(0x85);
        patchOff = Size();
        Dword(0);
    }

    void Je(uint32_t &patchOff) const {
        Byte(0x0F);
        Byte(0x84);
        patchOff = Size();
        Dword(0);
    }

    void Call(uint32_t &relocOff) const {
        Byte(0xE8);
        relocOff = Size();
        Dword(0);
    }

    void CallR10() const {
        Byte(0x41);
        Byte(0xFF);
        Byte(0xD2);
    }

    // Aggregate helpers
    void ImulR11R10Imm32(int32_t v) const {
        Byte(0x4D);
        Byte(0x69);
        Byte(0xDA);
        Dword(u(v));
    }

    void AddRaxR11() const {
        Byte(0x4C);
        Byte(0x01);
        Byte(0xD8);
    }

    void LeaRaxRaxDisp(int32_t v) const {
        Byte(0x48);
        Byte(0x8D);
        Byte(0x80);
        Dword(u(v));
    }

    void PopRbp() const {
        Byte(0x5D);
    }

    // Callee-saved registers push / pop / moves
    void PushRbx() const { Byte(0x53); }
    void PopRbx() const { Byte(0x5B); }
    void PushR12() const { Byte(0x41); Byte(0x54); }
    void PopR12() const { Byte(0x41); Byte(0x5C); }
    void PushR13() const { Byte(0x41); Byte(0x55); }
    void PopR13() const { Byte(0x41); Byte(0x5D); }
    void PushR14() const { Byte(0x41); Byte(0x56); }
    void PopR14() const { Byte(0x41); Byte(0x5E); }
    void PushR15() const { Byte(0x41); Byte(0x57); }
    void PopR15() const { Byte(0x41); Byte(0x5F); }

    void PushReg(int rIdx) const {
        if (rIdx == 0) PushRbx();
        else if (rIdx == 1) PushR12();
        else if (rIdx == 2) PushR13();
        else if (rIdx == 3) PushR14();
        else if (rIdx == 4) PushR15();
    }

    void PopReg(int rIdx) const {
        if (rIdx == 0) PopRbx();
        else if (rIdx == 1) PopR12();
        else if (rIdx == 2) PopR13();
        else if (rIdx == 3) PopR14();
        else if (rIdx == 4) PopR15();
    }

    // Move RAX from physical register (used by LoadA)
    void MovRaxPhysReg(int rIdx) const {
        if (rIdx == 0) { // rbx
            Byte(0x48); Byte(0x89); Byte(0xD8); // mov rax, rbx
        } else if (rIdx == 1) { // r12
            Byte(0x4C); Byte(0x89); Byte(0xE0); // mov rax, r12
        } else if (rIdx == 2) { // r13
            Byte(0x4C); Byte(0x89); Byte(0xE8); // mov rax, r13
        } else if (rIdx == 3) { // r14
            Byte(0x4C); Byte(0x89); Byte(0xF0); // mov rax, r14
        } else if (rIdx == 4) { // r15
            Byte(0x4C); Byte(0x89); Byte(0xF8); // mov rax, r15
        }
    }

    // Move R10 from physical register (used by LoadB)
    void MovR10PhysReg(int rIdx) const {
        if (rIdx == 0) { // rbx
            Byte(0x49); Byte(0x89); Byte(0xDA); // mov r10, rbx
        } else if (rIdx == 1) { // r12
            Byte(0x4D); Byte(0x89); Byte(0xE2); // mov r10, r12
        } else if (rIdx == 2) { // r13
            Byte(0x4D); Byte(0x89); Byte(0xEA); // mov r10, r13
        } else if (rIdx == 3) { // r14
            Byte(0x4D); Byte(0x89); Byte(0xF2); // mov r10, r14
        } else if (rIdx == 4) { // r15
            Byte(0x4D); Byte(0x89); Byte(0xFA); // mov r10, r15
        }
    }

    // Move R11 from physical register (used by Store and other instructions)
    void MovR11PhysReg(int rIdx) const {
        if (rIdx == 0) { // rbx
            Byte(0x49); Byte(0x89); Byte(0xDB); // mov r11, rbx
        } else if (rIdx == 1) { // r12
            Byte(0x4D); Byte(0x89); Byte(0xE3); // mov r11, r12
        } else if (rIdx == 2) { // r13
            Byte(0x4D); Byte(0x89); Byte(0xEB); // mov r11, r13
        } else if (rIdx == 3) { // r14
            Byte(0x4D); Byte(0x89); Byte(0xF3); // mov r11, r14
        } else if (rIdx == 4) { // r15
            Byte(0x4D); Byte(0x89); Byte(0xFB); // mov r11, r15
        }
    }

    // Move physical register from RAX (used by StoreA)
    void MovPhysRegRax(int rIdx) const {
        if (rIdx == 0) { // rbx
            Byte(0x48); Byte(0x89); Byte(0xC3); // mov rbx, rax
        } else if (rIdx == 1) { // r12
            Byte(0x49); Byte(0x89); Byte(0xC4); // mov r12, rax
        } else if (rIdx == 2) { // r13
            Byte(0x49); Byte(0x89); Byte(0xC5); // mov r13, rax
        } else if (rIdx == 3) { // r14
            Byte(0x49); Byte(0x89); Byte(0xC6); // mov r14, rax
        } else if (rIdx == 4) { // r15
            Byte(0x49); Byte(0x89); Byte(0xC7); // mov r15, rax
        }
    }

private:
    std::vector<uint8_t> &out;

    static uint32_t u(const int32_t v) {
        return static_cast<uint32_t>(v);
    }
};

} // namespace Rux
