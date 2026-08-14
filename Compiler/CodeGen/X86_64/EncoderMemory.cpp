#include "CodeGen/X86_64/Encoder.h"

namespace Rux {

void X64Enc::MovRaxLoad(const int32_t d) const {
    Byte(0x48);
    Byte(0x8B);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovRaxStore(const int32_t d) const {
    Byte(0x48);
    Byte(0x89);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovRaxStoreRsp(const int32_t d) const {
    Byte(0x48);
    Byte(0x89);
    Byte(0x84);
    Byte(0x24);
    Dword(u(d));
}

void X64Enc::MovEaxLoad(const int32_t d) const {
    Byte(0x8B);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovEaxStore(const int32_t d) const {
    Byte(0x89);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovzxRaxWord(const int32_t d) const {
    Byte(0x48);
    Byte(0x0F);
    Byte(0xB7);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovzxRaxByte(const int32_t d) const {
    Byte(0x48);
    Byte(0x0F);
    Byte(0xB6);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovsxdRaxDword(const int32_t d) const {
    Byte(0x48);
    Byte(0x63);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovsxRaxWord(const int32_t d) const {
    Byte(0x48);
    Byte(0x0F);
    Byte(0xBF);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovsxRaxByte(const int32_t d) const {
    Byte(0x48);
    Byte(0x0F);
    Byte(0xBE);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovAxStore(const int32_t d) const {
    Byte(0x66);
    Byte(0x89);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovAlStore(const int32_t d) const {
    Byte(0x88);
    Byte(0x85);
    Dword(u(d));
}

void X64Enc::MovR10Load(const int32_t d) const {
    Byte(0x4C);
    Byte(0x8B);
    Byte(0x95);
    Dword(u(d));
}

void X64Enc::MovR10Store(const int32_t d) const {
    Byte(0x4C);
    Byte(0x89);
    Byte(0x95);
    Dword(u(d));
}

void X64Enc::MovR10Rax() const {
    Byte(0x49);
    Byte(0x89);
    Byte(0xC2);
}

void X64Enc::MovzxR10Word(const int32_t d) const {
    Byte(0x4C);
    Byte(0x0F);
    Byte(0xB7);
    Byte(0x95);
    Dword(u(d));
}

void X64Enc::MovzxR10Byte(const int32_t d) const {
    Byte(0x4C);
    Byte(0x0F);
    Byte(0xB6);
    Byte(0x95);
    Dword(u(d));
}

void X64Enc::MovsxdR10Dword(const int32_t d) const {
    Byte(0x4C);
    Byte(0x63);
    Byte(0x95);
    Dword(u(d));
}

void X64Enc::MovsxR10Word(const int32_t d) const {
    Byte(0x4C);
    Byte(0x0F);
    Byte(0xBF);
    Byte(0x95);
    Dword(u(d));
}

void X64Enc::MovsxR10Byte(const int32_t d) const {
    Byte(0x4C);
    Byte(0x0F);
    Byte(0xBE);
    Byte(0x95);
    Dword(u(d));
}

void X64Enc::MovR10dLoad(const int32_t d) const {
    Byte(0x44);
    Byte(0x8B);
    Byte(0x95);
    Dword(u(d));
}

void X64Enc::MovR11Load(const int32_t d) const {
    Byte(0x4C);
    Byte(0x8B);
    Byte(0x9D);
    Dword(u(d));
}

void X64Enc::MovR11Store(const int32_t d) const {
    Byte(0x4C);
    Byte(0x89);
    Byte(0x9D);
    Dword(u(d));
}

void X64Enc::MovRdxR10Load(const int32_t d) const {
    Byte(0x49);
    Byte(0x8B);
    Byte(0x92);
    Dword(u(d));
}

void X64Enc::MovR8R10Load(const int32_t d) const {
    Byte(0x4D);
    Byte(0x8B);
    Byte(0x82);
    Dword(u(d));
}

void X64Enc::MovRsiR10Load(const int32_t d) const {
    Byte(0x49);
    Byte(0x8B);
    Byte(0xB2);
    Dword(u(d));
}

void X64Enc::MovRcxLoad(const int32_t d) const {
    Byte(0x48);
    Byte(0x8B);
    Byte(0x8D);
    Dword(u(d));
}

void X64Enc::MovArgLoad(const int idx, const int32_t d) const {
    static const uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x4C, 0x4C};
    static const uint8_t modrm[] = {0xBD, 0xB5, 0x95, 0x8D, 0x85, 0x8D};
    Byte(rex[idx]);
    Byte(0x8B);
    Byte(modrm[idx]);
    Dword(u(d));
}

void X64Enc::MovArgStore(const int idx, const int32_t d) const {
    static const uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x4C, 0x4C};
    static const uint8_t modrm[] = {0xBD, 0xB5, 0x95, 0x8D, 0x85, 0x8D};
    Byte(rex[idx]);
    Byte(0x89);
    Byte(modrm[idx]);
    Dword(u(d));
}

void X64Enc::MovArgRax(const int idx) const {
    static constexpr uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x49, 0x49};
    static constexpr uint8_t modrm[] = {0xC7, 0xC6, 0xC2, 0xC1, 0xC0, 0xC1};
    Byte(rex[idx]);
    Byte(0x89);
    Byte(modrm[idx]);
}

void X64Enc::MovArgLoadWin64(const int idx, const int32_t d) const {
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

void X64Enc::MovArgStoreWin64(const int idx, const int32_t d) const {
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

void X64Enc::MovRaxArgWin64(const int idx) const {
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

void X64Enc::MovArgWin64Rax(const int idx) const {
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

void X64Enc::LeaArgStackWin64(const int idx, const int32_t d) const {
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

void X64Enc::LeaR9Rsp(const int32_t d) const {
    Byte(0x4C);
    Byte(0x8D);
    Byte(0x8C);
    Byte(0x24);
    Dword(u(d));
}

void X64Enc::MovQwordRspImm32(const int32_t d, const int32_t value) const {
    Byte(0x48);
    Byte(0xC7);
    Byte(0x84);
    Byte(0x24);
    Dword(u(d));
    Dword(u(value));
}

void X64Enc::MovR10ArgWin64(const int idx) const {
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

void X64Enc::SubRspShadow() const {
    Byte(0x48);
    Byte(0x83);
    Byte(0xEC);
    Byte(0x20);
}

void X64Enc::AddRspShadow() const {
    Byte(0x48);
    Byte(0x83);
    Byte(0xC4);
    Byte(0x20);
}

} // namespace Rux
