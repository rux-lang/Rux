// x86-64 SSE encoding: scalar floating-point moves, arithmetic, comparison and
// conversion.

#include "CodeGen/X86_64/Encoder.h"

namespace Rux {

void X64Enc::MovXmm(const int destination, const int source) const {
    if (destination >= 8 || source >= 8) {
        Byte(static_cast<std::uint8_t>(0x40 | (destination >= 8 ? 4 : 0) | (source >= 8 ? 1 : 0)));
    }
    Byte(0x0F);
    Byte(0x28); // movaps xmm, xmm; register copies require no memory alignment.
    Byte(static_cast<std::uint8_t>(0xC0 | ((destination & 7) << 3) | (source & 7)));
}

void X64Enc::MovXmmSave(const int reg, const std::int32_t displacement) const {
    Byte(0xF3);
    if (reg >= 8) {
        Byte(0x44);
    }
    Byte(0x0F);
    Byte(0x7F); // movdqu [rbp+disp32], xmm; preserve every nonvolatile bit.
    Byte(static_cast<std::uint8_t>(0x85 | ((reg & 7) << 3)));
    Dword(u(displacement));
}

void X64Enc::MovXmmRestore(const int reg, const std::int32_t displacement) const {
    Byte(0xF3);
    if (reg >= 8) {
        Byte(0x44);
    }
    Byte(0x0F);
    Byte(0x6F);
    Byte(static_cast<std::uint8_t>(0x85 | ((reg & 7) << 3)));
    Dword(u(displacement));
}

void X64Enc::MovFloatBits(const int destinationGpr, const int sourceXmm, const bool wide) const {
    Byte(0x66);
    if (wide || destinationGpr >= 8 || sourceXmm >= 8) {
        Byte(static_cast<std::uint8_t>(0x40 | (wide ? 8 : 0) | (sourceXmm >= 8 ? 4 : 0) |
                                       (destinationGpr >= 8 ? 1 : 0)));
    }
    Byte(0x0F);
    Byte(0x7E);
    Byte(static_cast<std::uint8_t>(0xC0 | ((sourceXmm & 7) << 3) | (destinationGpr & 7)));
}

void X64Enc::MovssXmmNLoad(const int n, const int32_t d) const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x10);
    Byte(static_cast<uint8_t>(0x80 | (n << 3) | 5));
    Dword(u(d));
}

void X64Enc::MovsdXmmNLoad(const int n, const int32_t d) const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x10);
    Byte(static_cast<uint8_t>(0x80 | (n << 3) | 5));
    Dword(u(d));
}

void X64Enc::MovssXmm0StoreRsp(const int32_t d) const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x11);
    Byte(0x84);
    Byte(0x24);
    Dword(u(d));
}

void X64Enc::MovsdXmm0StoreRsp(const int32_t d) const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x11);
    Byte(0x84);
    Byte(0x24);
    Dword(u(d));
}

void X64Enc::MovssXmm0Load(const int32_t d) const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x10);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovsdXmm0Load(const int32_t d) const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x10);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovssXmm1Load(const int32_t d) const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x10);
    Byte(0x8D);
    Dword(u(d));
}

void X64Enc::MovsdXmm1Load(const int32_t d) const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x10);
    Byte(0x8D);
    Dword(u(d));
}

void X64Enc::MovssXmm0Store(const int32_t d) const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x11);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovsdXmm0Store(const int32_t d) const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x11);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovssXmm1Store(const int32_t d) const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x11);
    Byte(0x8D);
    Dword(u(d));
}

void X64Enc::MovsdXmm1Store(const int32_t d) const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x11);
    Byte(0x8D);
    Dword(u(d));
}

void X64Enc::MovssXmm0Rip(uint32_t &relocOff) const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x10);
    Byte(0x05);
    relocOff = Size();
    Dword(0);
}

void X64Enc::MovsdXmm0Rip(uint32_t &relocOff) const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x10);
    Byte(0x05);
    relocOff = Size();
    Dword(0);
}

void X64Enc::MovssXmm1Rip(uint32_t &relocOff) const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x10);
    Byte(0x0D);
    relocOff = Size();
    Dword(0);
}

void X64Enc::MovsdXmm1Rip(uint32_t &relocOff) const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x10);
    Byte(0x0D);
    relocOff = Size();
    Dword(0);
}

void X64Enc::AddssXmm01() const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x58);
    Byte(0xC1);
}

void X64Enc::SubssXmm01() const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x5C);
    Byte(0xC1);
}

void X64Enc::MulssXmm01() const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x59);
    Byte(0xC1);
}

void X64Enc::DivssXmm01() const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x5E);
    Byte(0xC1);
}

void X64Enc::AddsdXmm01() const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x58);
    Byte(0xC1);
}

void X64Enc::SubsdXmm01() const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x5C);
    Byte(0xC1);
}

void X64Enc::MulsdXmm01() const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x59);
    Byte(0xC1);
}

void X64Enc::DivsdXmm01() const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x5E);
    Byte(0xC1);
}

void X64Enc::UcomissXmm01() const {
    Byte(0x0F);
    Byte(0x2E);
    Byte(0xC1);
}

void X64Enc::UcomisdXmm01() const {
    Byte(0x66);
    Byte(0x0F);
    Byte(0x2E);
    Byte(0xC1);
}

void X64Enc::XorpsXmm01() const {
    Byte(0x0F);
    Byte(0x57);
    Byte(0xC1);
}

void X64Enc::XorpdXmm01() const {
    Byte(0x66);
    Byte(0x0F);
    Byte(0x57);
    Byte(0xC1);
}

void X64Enc::Cvtsi2ssXmm0Rax() const {
    Byte(0xF3);
    Byte(0x48);
    Byte(0x0F);
    Byte(0x2A);
    Byte(0xC0);
}

void X64Enc::Cvtsi2sdXmm0Rax() const {
    Byte(0xF2);
    Byte(0x48);
    Byte(0x0F);
    Byte(0x2A);
    Byte(0xC0);
}

void X64Enc::CvttsssiRaxXmm0() const {
    Byte(0xF3);
    Byte(0x48);
    Byte(0x0F);
    Byte(0x2C);
    Byte(0xC0);
}

void X64Enc::CvttsdsiRaxXmm0() const {
    Byte(0xF2);
    Byte(0x48);
    Byte(0x0F);
    Byte(0x2C);
    Byte(0xC0);
}

void X64Enc::CvtsssdXmm0() const {
    Byte(0xF3);
    Byte(0x0F);
    Byte(0x5A);
    Byte(0xC0);
}

void X64Enc::CvtsdssXmm0() const {
    Byte(0xF2);
    Byte(0x0F);
    Byte(0x5A);
    Byte(0xC0);
}

} // namespace Rux
