// x86-64 instruction encoding: the byte-level emission and patching primitives
// every form is built on, and the integer and control-flow instructions. Memory
// and floating-point forms live in the sibling Encoder*.cpp files.

#include "CodeGen/X86_64/Encoder.h"

namespace Rux {

uint32_t X64Enc::Size() const {
    return static_cast<uint32_t>(out.size());
}

void X64Enc::Byte(uint8_t b) const {
    out.push_back(b);
}

void X64Enc::Dword(uint32_t d) const {
    out.push_back(d & 0xFF);
    out.push_back((d >> 8) & 0xFF);
    out.push_back((d >> 16) & 0xFF);
    out.push_back((d >> 24) & 0xFF);
}

void X64Enc::Qword(uint64_t q) const {
    for (int i = 0; i < 8; ++i) {
        out.push_back(q & 0xFF);
        q >>= 8;
    }
}

void X64Enc::Patch32(uint32_t off, int32_t v) const {
    out[off] = v & 0xFF;
    out[off + 1] = (v >> 8) & 0xFF;
    out[off + 2] = (v >> 16) & 0xFF;
    out[off + 3] = (v >> 24) & 0xFF;
}

void X64Enc::PushRbp() const {
    Byte(0x55);
}

void X64Enc::MovRbpRsp() const {
    Byte(0x48);
    Byte(0x89);
    Byte(0xE5);
}

void X64Enc::SubRspImm32(int32_t n) const {
    Byte(0x48);
    Byte(0x81);
    Byte(0xEC);
    Dword(static_cast<uint32_t>(n));
}

void X64Enc::TouchRsp() const {
    Byte(0x48);
    Byte(0x85);
    Byte(0x04);
    Byte(0x24); // test qword [rsp], rax
}

void X64Enc::AddRspImm32(int32_t n) const {
    Byte(0x48);
    Byte(0x81);
    Byte(0xC4);
    Dword(static_cast<uint32_t>(n));
}

void X64Enc::Leave() const {
    Byte(0xC9);
}

void X64Enc::Ret() const {
    Byte(0xC3);
}

void X64Enc::Ud2() const {
    Byte(0x0F);
    Byte(0x0B);
}

void X64Enc::MovRaxImm64(int64_t v) const {
    Byte(0x48);
    Byte(0xB8);
    Qword(static_cast<uint64_t>(v));
}

void X64Enc::MovEaxImm32(int32_t v) const {
    Byte(0xB8);
    Dword(static_cast<uint32_t>(v));
}

void X64Enc::MovEdiImm32(const int32_t v) const {
    Byte(0xBF);
    Dword(static_cast<uint32_t>(v));
}

void X64Enc::MovEdxImm32(const int32_t v) const {
    Byte(0xBA);
    Dword(static_cast<uint32_t>(v));
}

void X64Enc::MovRsiRax() const {
    Byte(0x48);
    Byte(0x89);
    Byte(0xC6);
}

void X64Enc::Syscall() const {
    Byte(0x0F);
    Byte(0x05);
}

void X64Enc::LeaRaxRip(uint32_t &relocOff) const {
    Byte(0x48);
    Byte(0x8D);
    Byte(0x05);
    relocOff = Size();
    Dword(0);
}

void X64Enc::MovRaxRip(uint32_t &relocOff) const {
    Byte(0x48);
    Byte(0x8B);
    Byte(0x05);
    relocOff = Size();
    Dword(0);
}

void X64Enc::LeaRaxStack(int32_t d) const {
    Byte(0x48);
    Byte(0x8D);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovRaxR10() const {
    Byte(0x4C);
    Byte(0x89);
    Byte(0xD0);
} // mov rax, r10

void X64Enc::MovRcxR11() const {
    Byte(0x4C);
    Byte(0x89);
    Byte(0xD9);
} // mov rcx, r11

void X64Enc::MovRaxRdx() const {
    Byte(0x48);
    Byte(0x8B);
    Byte(0xC2);
} // mov rax, rdx

void X64Enc::AddRaxR10() const {
    Byte(0x4C);
    Byte(0x01);
    Byte(0xD0);
}

void X64Enc::SubRaxR10() const {
    Byte(0x4C);
    Byte(0x29);
    Byte(0xD0);
}

void X64Enc::AndRaxR10() const {
    Byte(0x4C);
    Byte(0x21);
    Byte(0xD0);
}

void X64Enc::OrRaxR10() const {
    Byte(0x4C);
    Byte(0x09);
    Byte(0xD0);
}

void X64Enc::XorRaxR10() const {
    Byte(0x4C);
    Byte(0x31);
    Byte(0xD0);
}

void X64Enc::ImulRaxR10() const {
    Byte(0x49);
    Byte(0x0F);
    Byte(0xAF);
    Byte(0xC2);
}

void X64Enc::NegRax() const {
    Byte(0x48);
    Byte(0xF7);
    Byte(0xD8);
}

void X64Enc::NotRax() const {
    Byte(0x48);
    Byte(0xF7);
    Byte(0xD0);
}

void X64Enc::XorRaxImmediate(std::int32_t imm) const {
    Byte(0x48);
    Byte(0x81);
    Byte(0xF0);
    Dword(static_cast<std::uint32_t>(imm));
}

void X64Enc::Cqo() const {
    Byte(0x48);
    Byte(0x99);
}

void X64Enc::XorRdxRdx() const {
    Byte(0x48);
    Byte(0x31);
    Byte(0xD2);
}

void X64Enc::IdivR10() const {
    Byte(0x49);
    Byte(0xF7);
    Byte(0xFA);
}

void X64Enc::DivR10() const {
    Byte(0x49);
    Byte(0xF7);
    Byte(0xF2);
}

void X64Enc::ShlRaxCl() const {
    Byte(0x48);
    Byte(0xD3);
    Byte(0xE0);
}

void X64Enc::ShrRaxCl() const {
    Byte(0x48);
    Byte(0xD3);
    Byte(0xE8);
}

void X64Enc::SarRaxCl() const {
    Byte(0x48);
    Byte(0xD3);
    Byte(0xF8);
}

void X64Enc::TestRaxRax() const {
    Byte(0x48);
    Byte(0x85);
    Byte(0xC0);
}

void X64Enc::CmpRaxR10() const {
    Byte(0x4C);
    Byte(0x39);
    Byte(0xD0);
}

void X64Enc::CmpRaxImm32(int32_t v) const {
    Byte(0x48);
    Byte(0x81);
    Byte(0xF8);
    Dword(u(v));
}

void X64Enc::SeteAl() const {
    Byte(0x0F);
    Byte(0x94);
    Byte(0xC0); // sete al
}

void X64Enc::SetneAl() const {
    Byte(0x0F);
    Byte(0x95);
    Byte(0xC0); // setne al
}

void X64Enc::SetnpDl() const {
    Byte(0x0F);
    Byte(0x9B);
    Byte(0xC2); // setnp dl
}

void X64Enc::SetlAl() const {
    Byte(0x0F);
    Byte(0x9C);
    Byte(0xC0);
}

void X64Enc::SetleAl() const {
    Byte(0x0F);
    Byte(0x9E);
    Byte(0xC0);
}

void X64Enc::SetgAl() const {
    Byte(0x0F);
    Byte(0x9F);
    Byte(0xC0);
}

void X64Enc::SetgeAl() const {
    Byte(0x0F);
    Byte(0x9D);
    Byte(0xC0);
}

void X64Enc::SetbAl() const {
    Byte(0x0F);
    Byte(0x92);
    Byte(0xC0);
}

void X64Enc::SetbeAl() const {
    Byte(0x0F);
    Byte(0x96);
    Byte(0xC0);
}

void X64Enc::SetaAl() const {
    Byte(0x0F);
    Byte(0x97);
    Byte(0xC0);
}

void X64Enc::SetaeAl() const {
    Byte(0x0F);
    Byte(0x93);
    Byte(0xC0);
}

void X64Enc::MovzxRaxAl() const {
    Byte(0x48);
    Byte(0x0F);
    Byte(0xB6);
    Byte(0xC0);
}

void X64Enc::MovzxRaxAx() const {
    Byte(0x48);
    Byte(0x0F);
    Byte(0xB7);
    Byte(0xC0);
}

void X64Enc::MovsxdRaxEax() const {
    Byte(0x48);
    Byte(0x63);
    Byte(0xC0);
}

void X64Enc::MovsxRaxAl() const {
    Byte(0x48);
    Byte(0x0F);
    Byte(0xBE);
    Byte(0xC0);
}

void X64Enc::MovsxRaxAx() const {
    Byte(0x48);
    Byte(0x0F);
    Byte(0xBF);
    Byte(0xC0);
}

void X64Enc::MovEaxEax() const {
    Byte(0x89);
    Byte(0xC0);
}

void X64Enc::MovzxR10r10b() const {
    Byte(0x4D);
    Byte(0x0F);
    Byte(0xB6);
    Byte(0xD2);
}

void X64Enc::MovzxR10r10w() const {
    Byte(0x4D);
    Byte(0x0F);
    Byte(0xB7);
    Byte(0xD2);
}

void X64Enc::MovsxdR10r10d() const {
    Byte(0x4D);
    Byte(0x63);
    Byte(0xD2);
}

void X64Enc::MovsxR10r10b() const {
    Byte(0x4D);
    Byte(0x0F);
    Byte(0xBE);
    Byte(0xD2);
}

void X64Enc::MovsxR10r10w() const {
    Byte(0x4D);
    Byte(0x0F);
    Byte(0xBF);
    Byte(0xD2);
}

void X64Enc::MovR10dR10d() const {
    Byte(0x45);
    Byte(0x89);
    Byte(0xD2);
}

void X64Enc::SetpDl() const {
    Byte(0x0F);
    Byte(0x9A);
    Byte(0xC2); // setp dl
}

void X64Enc::AndAlDl() const {
    Byte(0x20);
    Byte(0xD0); // and al, dl
}

void X64Enc::OrAlDl() const {
    Byte(0x08);
    Byte(0xD0); // or al, dl
}

void X64Enc::Jmp(uint32_t &patchOff) const {
    Byte(0xE9);
    patchOff = Size();
    Dword(0);
}

void X64Enc::Jz(uint32_t &patchOff) const {
    Byte(0x0F);
    Byte(0x84);
    patchOff = Size();
    Dword(0);
}

void X64Enc::Jnz(uint32_t &patchOff) const {
    Byte(0x0F);
    Byte(0x85);
    patchOff = Size();
    Dword(0);
}

void X64Enc::Je(uint32_t &patchOff) const {
    Byte(0x0F);
    Byte(0x84);
    patchOff = Size();
    Dword(0);
}

void X64Enc::Call(uint32_t &relocOff) const {
    Byte(0xE8);
    relocOff = Size();
    Dword(0);
}

void X64Enc::CallR10() const {
    Byte(0x41);
    Byte(0xFF);
    Byte(0xD2);
}

void X64Enc::ImulR11R10Imm32(int32_t v) const {
    Byte(0x4D);
    Byte(0x69);
    Byte(0xDA);
    Dword(u(v));
}

void X64Enc::AddRaxR11() const {
    Byte(0x4C);
    Byte(0x01);
    Byte(0xD8);
}

void X64Enc::LeaRaxRaxDisp(int32_t v) const {
    Byte(0x48);
    Byte(0x8D);
    Byte(0x80);
    Dword(u(v));
}

void X64Enc::PopRbp() const {
    Byte(0x5D);
}

void X64Enc::PushRbx() const {
    Byte(0x53);
}

void X64Enc::PopRbx() const {
    Byte(0x5B);
}

void X64Enc::PushR12() const {
    Byte(0x41);
    Byte(0x54);
}

void X64Enc::PopR12() const {
    Byte(0x41);
    Byte(0x5C);
}

void X64Enc::PushR13() const {
    Byte(0x41);
    Byte(0x55);
}

void X64Enc::PopR13() const {
    Byte(0x41);
    Byte(0x5D);
}

void X64Enc::PushR14() const {
    Byte(0x41);
    Byte(0x56);
}

void X64Enc::PopR14() const {
    Byte(0x41);
    Byte(0x5E);
}

void X64Enc::PushR15() const {
    Byte(0x41);
    Byte(0x57);
}

void X64Enc::PopR15() const {
    Byte(0x41);
    Byte(0x5F);
}

void X64Enc::PushReg(int rIdx) const {
    if (rIdx == 0)
        PushRbx();
    else if (rIdx == 1)
        PushR12();
    else if (rIdx == 2)
        PushR13();
    else if (rIdx == 3)
        PushR14();
    else if (rIdx == 4)
        PushR15();
}

void X64Enc::PopReg(int rIdx) const {
    if (rIdx == 0)
        PopRbx();
    else if (rIdx == 1)
        PopR12();
    else if (rIdx == 2)
        PopR13();
    else if (rIdx == 3)
        PopR14();
    else if (rIdx == 4)
        PopR15();
}

void X64Enc::MovRaxPhysReg(int rIdx) const {
    if (rIdx == 0) {
        // rbx
        Byte(0x48);
        Byte(0x89);
        Byte(0xD8); // mov rax, rbx
    }
    else if (rIdx == 1) {
        // r12
        Byte(0x4C);
        Byte(0x89);
        Byte(0xE0); // mov rax, r12
    }
    else if (rIdx == 2) {
        // r13
        Byte(0x4C);
        Byte(0x89);
        Byte(0xE8); // mov rax, r13
    }
    else if (rIdx == 3) {
        // r14
        Byte(0x4C);
        Byte(0x89);
        Byte(0xF0); // mov rax, r14
    }
    else if (rIdx == 4) {
        // r15
        Byte(0x4C);
        Byte(0x89);
        Byte(0xF8); // mov rax, r15
    }
}

void X64Enc::MovR10PhysReg(int rIdx) const {
    if (rIdx == 0) {
        // rbx
        Byte(0x49);
        Byte(0x89);
        Byte(0xDA); // mov r10, rbx
    }
    else if (rIdx == 1) {
        // r12
        Byte(0x4D);
        Byte(0x89);
        Byte(0xE2); // mov r10, r12
    }
    else if (rIdx == 2) {
        // r13
        Byte(0x4D);
        Byte(0x89);
        Byte(0xEA); // mov r10, r13
    }
    else if (rIdx == 3) {
        // r14
        Byte(0x4D);
        Byte(0x89);
        Byte(0xF2); // mov r10, r14
    }
    else if (rIdx == 4) {
        // r15
        Byte(0x4D);
        Byte(0x89);
        Byte(0xFA); // mov r10, r15
    }
}

void X64Enc::MovR11PhysReg(int rIdx) const {
    if (rIdx == 0) {
        // rbx
        Byte(0x49);
        Byte(0x89);
        Byte(0xDB); // mov r11, rbx
    }
    else if (rIdx == 1) {
        // r12
        Byte(0x4D);
        Byte(0x89);
        Byte(0xE3); // mov r11, r12
    }
    else if (rIdx == 2) {
        // r13
        Byte(0x4D);
        Byte(0x89);
        Byte(0xEB); // mov r11, r13
    }
    else if (rIdx == 3) {
        // r14
        Byte(0x4D);
        Byte(0x89);
        Byte(0xF3); // mov r11, r14
    }
    else if (rIdx == 4) {
        // r15
        Byte(0x4D);
        Byte(0x89);
        Byte(0xFB); // mov r11, r15
    }
}

void X64Enc::MovPhysRegRax(int rIdx) const {
    if (rIdx == 0) {
        // rbx
        Byte(0x48);
        Byte(0x89);
        Byte(0xC3); // mov rbx, rax
    }
    else if (rIdx == 1) {
        // r12
        Byte(0x49);
        Byte(0x89);
        Byte(0xC4); // mov r12, rax
    }
    else if (rIdx == 2) {
        // r13
        Byte(0x49);
        Byte(0x89);
        Byte(0xC5); // mov r13, rax
    }
    else if (rIdx == 3) {
        // r14
        Byte(0x49);
        Byte(0x89);
        Byte(0xC6); // mov r14, rax
    }
    else if (rIdx == 4) {
        // r15
        Byte(0x49);
        Byte(0x89);
        Byte(0xC7); // mov r15, rax
    }
}

uint32_t X64Enc::u(const int32_t v) {
    return static_cast<uint32_t>(v);
}

} // namespace Rux
