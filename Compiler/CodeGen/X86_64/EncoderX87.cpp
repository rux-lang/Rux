// x86-64 x87 encoding: the float80 loads, stores, arithmetic, comparison and control-word instructions.
//
// Opcodes and operand order follow the Intel SDM (Intel syntax). See the x87 block in Encoder.h for the AT&T naming
// trap around fsubp/fdivp.

#include "CodeGen/X86_64/Encoder.h"

#include <cassert>

namespace Rux {
namespace {
/// Emits `opcode /extension` with the memory operand [base + disp32]: REX.B for r8-r15, ModRM mod=10, and a SIB byte
/// (no index) when the base is rsp or r12, whose r/m encoding 100 means "SIB follows".
void EmitX87Memory(const X64Enc &enc, const uint8_t opcode, const uint8_t extension, const X64Enc::Gpr base,
                   const int32_t d) {
    const auto number = static_cast<uint8_t>(base);
    if (number >= 8)
        enc.Byte(0x41);
    enc.Byte(opcode);
    enc.Byte(static_cast<uint8_t>(0x80 | (extension << 3) | (number & 7)));
    if ((number & 7) == 4)
        enc.Byte(0x24);
    enc.Dword(static_cast<uint32_t>(d));
}

/// Emits a two-byte register form whose second byte is `base + i` for ST(i).
void EmitX87Register(const X64Enc &enc, const uint8_t opcode, const uint8_t base, const int i) {
    assert(i >= 0 && i < 8 && "x87 register index out of range");
    enc.Byte(opcode);
    enc.Byte(static_cast<uint8_t>(base + (i & 7)));
}
} // namespace

// Memory loads and stores, [rbp + d]

void X64Enc::FldTbyte(const int32_t d) const {
    FldTbyteAt(Gpr::Rbp, d);
}

void X64Enc::FstpTbyte(const int32_t d) const {
    FstpTbyteAt(Gpr::Rbp, d);
}

void X64Enc::FldQword(const int32_t d) const {
    FldQwordAt(Gpr::Rbp, d);
}

void X64Enc::FstpQword(const int32_t d) const {
    FstpQwordAt(Gpr::Rbp, d);
}

void X64Enc::FldDword(const int32_t d) const {
    FldDwordAt(Gpr::Rbp, d);
}

void X64Enc::FstpDword(const int32_t d) const {
    FstpDwordAt(Gpr::Rbp, d);
}

void X64Enc::FildQword(const int32_t d) const {
    FildQwordAt(Gpr::Rbp, d);
}

void X64Enc::FildDword(const int32_t d) const {
    FildDwordAt(Gpr::Rbp, d);
}

// Memory loads and stores, [base + d]

void X64Enc::FldTbyteAt(const Gpr base, const int32_t d) const {
    EmitX87Memory(*this, 0xDB, 5, base, d);
}

void X64Enc::FstpTbyteAt(const Gpr base, const int32_t d) const {
    EmitX87Memory(*this, 0xDB, 7, base, d);
}

void X64Enc::FldQwordAt(const Gpr base, const int32_t d) const {
    EmitX87Memory(*this, 0xDD, 0, base, d);
}

void X64Enc::FstpQwordAt(const Gpr base, const int32_t d) const {
    EmitX87Memory(*this, 0xDD, 3, base, d);
}

void X64Enc::FldDwordAt(const Gpr base, const int32_t d) const {
    EmitX87Memory(*this, 0xD9, 0, base, d);
}

void X64Enc::FstpDwordAt(const Gpr base, const int32_t d) const {
    EmitX87Memory(*this, 0xD9, 3, base, d);
}

void X64Enc::FildQwordAt(const Gpr base, const int32_t d) const {
    EmitX87Memory(*this, 0xDF, 5, base, d);
}

void X64Enc::FildDwordAt(const Gpr base, const int32_t d) const {
    EmitX87Memory(*this, 0xDB, 0, base, d);
}

// Arithmetic on ST(1) and ST(0), popping. DE E8+i is Intel's FSUBP ST(i), ST(0), which computes ST(i) - ST(0);
// DE E0+i is FSUBRP, which computes ST(0) - ST(i). Division mirrors this with F8+i and F0+i.

void X64Enc::FaddpSt1() const {
    Byte(0xDE);
    Byte(0xC1);
}

void X64Enc::FsubpSt1() const {
    Byte(0xDE);
    Byte(0xE9);
}

void X64Enc::FsubrpSt1() const {
    Byte(0xDE);
    Byte(0xE1);
}

void X64Enc::FmulpSt1() const {
    Byte(0xDE);
    Byte(0xC9);
}

void X64Enc::FdivpSt1() const {
    Byte(0xDE);
    Byte(0xF9);
}

void X64Enc::FdivrpSt1() const {
    Byte(0xDE);
    Byte(0xF1);
}

void X64Enc::Fprem() const {
    Byte(0xD9);
    Byte(0xF8);
}

void X64Enc::Fchs() const {
    Byte(0xD9);
    Byte(0xE0);
}

// Status, comparison and stack management

void X64Enc::FnstswAx() const {
    Byte(0xDF);
    Byte(0xE0);
}

void X64Enc::FucomipSt(const int i) const {
    EmitX87Register(*this, 0xDF, 0xE8, i);
}

void X64Enc::FstpSt(const int i) const {
    EmitX87Register(*this, 0xDD, 0xD8, i);
}

void X64Enc::FldSt(const int i) const {
    EmitX87Register(*this, 0xD9, 0xC0, i);
}

void X64Enc::FxchSt(const int i) const {
    EmitX87Register(*this, 0xD9, 0xC8, i);
}

// Control word and the integer helpers around it

void X64Enc::Fnstcw(const int32_t d) const {
    EmitX87Memory(*this, 0xD9, 7, Gpr::Rbp, d);
}

void X64Enc::Fldcw(const int32_t d) const {
    EmitX87Memory(*this, 0xD9, 5, Gpr::Rbp, d);
}

void X64Enc::MovWordStoreImm16(const int32_t d, const uint16_t v) const {
    Byte(0x66);
    Byte(0xC7);
    Byte(0x85);
    Dword(u(d));
    Byte(static_cast<uint8_t>(v & 0xFF));
    Byte(static_cast<uint8_t>(v >> 8));
}

void X64Enc::MovDwordStoreImm32(const int32_t d, const int32_t v) const {
    Byte(0xC7);
    Byte(0x85);
    Dword(u(d));
    Dword(u(v));
}

void X64Enc::TestAhImm8(const uint8_t v) const {
    Byte(0xF6);
    Byte(0xC4);
    Byte(v);
}
} // namespace Rux
