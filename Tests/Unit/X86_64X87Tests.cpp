// x86-64 x87 encoding and float80 lowering.
//
// Every expected byte string follows the Intel SDM opcode tables and was cross-checked against clang's integrated
// assembler in Intel syntax with the `{disp32}` prefix, which forces the disp32 forms this encoder always emits.

#include "CodeGen/X86_64/Encoder.h"

#include <cstdint>
#include <doctest.h>
#include <vector>

using namespace Rux;

namespace {
using Bytes = std::vector<std::uint8_t>;
using Gpr = X64Enc::Gpr;

template <typename Emit>
[[nodiscard]] Bytes Encode(Emit emit) {
    Bytes data;
    const X64Enc encoder(data);
    emit(encoder);
    return data;
}
} // namespace

TEST_CASE("x87 loads and stores address rbp-relative frame slots with a disp32") {
    // [rbp - 32]: ModRM mod=10 r/m=101 with the /digit in reg, then E0 FF FF FF.
    CHECK(Encode([](const X64Enc &e) { e.FldTbyte(-32); }) == Bytes{0xDB, 0xAD, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FstpTbyte(-32); }) == Bytes{0xDB, 0xBD, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FldQword(-32); }) == Bytes{0xDD, 0x85, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FstpQword(-32); }) == Bytes{0xDD, 0x9D, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FldDword(-32); }) == Bytes{0xD9, 0x85, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FstpDword(-32); }) == Bytes{0xD9, 0x9D, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FildQword(-32); }) == Bytes{0xDF, 0xAD, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FildDword(-32); }) == Bytes{0xDB, 0x85, 0xE0, 0xFF, 0xFF, 0xFF});
}

TEST_CASE("x87 rbp forms are the base-register forms with rbp as the base") {
    CHECK(Encode([](const X64Enc &e) { e.FldTbyte(0x120); }) ==
          Encode([](const X64Enc &e) { e.FldTbyteAt(Gpr::Rbp, 0x120); }));
    CHECK(Encode([](const X64Enc &e) { e.FstpQword(-8); }) ==
          Encode([](const X64Enc &e) { e.FstpQwordAt(Gpr::Rbp, -8); }));
    CHECK(Encode([](const X64Enc &e) { e.FildDword(4); }) ==
          Encode([](const X64Enc &e) { e.FildDwordAt(Gpr::Rbp, 4); }));
}

TEST_CASE("x87 base-register forms add REX.B for r8-r15 and a SIB byte for rsp and r12") {
    // fld tbyte [r10 + 16]
    CHECK(Encode([](const X64Enc &e) { e.FldTbyteAt(Gpr::R10, 16); }) ==
          Bytes{0x41, 0xDB, 0xAA, 0x10, 0x00, 0x00, 0x00});
    // fstp tbyte [rsp + 8]
    CHECK(Encode([](const X64Enc &e) { e.FstpTbyteAt(Gpr::Rsp, 8); }) ==
          Bytes{0xDB, 0xBC, 0x24, 0x08, 0x00, 0x00, 0x00});
    // fstp tbyte [r11 - 8]
    CHECK(Encode([](const X64Enc &e) { e.FstpTbyteAt(Gpr::R11, -8); }) ==
          Bytes{0x41, 0xDB, 0xBB, 0xF8, 0xFF, 0xFF, 0xFF});
    // fld tbyte [r12 + 0x100]: r12 shares rsp's r/m encoding, so it needs the SIB byte too.
    CHECK(Encode([](const X64Enc &e) { e.FldTbyteAt(Gpr::R12, 0x100); }) ==
          Bytes{0x41, 0xDB, 0xAC, 0x24, 0x00, 0x01, 0x00, 0x00});
    // fld tbyte [r13 + 0x100]: r13 shares rbp's r/m encoding, which mod=10 makes an ordinary base.
    CHECK(Encode([](const X64Enc &e) { e.FldTbyteAt(Gpr::R13, 0x100); }) ==
          Bytes{0x41, 0xDB, 0xAD, 0x00, 0x01, 0x00, 0x00});
    // fld qword [rax + 0x12345678]
    CHECK(Encode([](const X64Enc &e) { e.FldQwordAt(Gpr::Rax, 0x12345678); }) ==
          Bytes{0xDD, 0x80, 0x78, 0x56, 0x34, 0x12});
    // fstp qword [r15 + 24]
    CHECK(Encode([](const X64Enc &e) { e.FstpQwordAt(Gpr::R15, 24); }) ==
          Bytes{0x41, 0xDD, 0x9F, 0x18, 0x00, 0x00, 0x00});
    // fld dword [rdi + 4]
    CHECK(Encode([](const X64Enc &e) { e.FldDwordAt(Gpr::Rdi, 4); }) == Bytes{0xD9, 0x87, 0x04, 0x00, 0x00, 0x00});
    // fstp dword [rbx + 0]
    CHECK(Encode([](const X64Enc &e) { e.FstpDwordAt(Gpr::Rbx, 0); }) == Bytes{0xD9, 0x9B, 0x00, 0x00, 0x00, 0x00});
    // fild qword [rsp + 16]
    CHECK(Encode([](const X64Enc &e) { e.FildQwordAt(Gpr::Rsp, 16); }) ==
          Bytes{0xDF, 0xAC, 0x24, 0x10, 0x00, 0x00, 0x00});
    // fild dword [r9 - 4]
    CHECK(Encode([](const X64Enc &e) { e.FildDwordAt(Gpr::R9, -4); }) ==
          Bytes{0x41, 0xDB, 0x81, 0xFC, 0xFF, 0xFF, 0xFF});
}

TEST_CASE("x87 popping arithmetic uses the Intel operand order") {
    // Intel FSUBP ST(1), ST(0) is DE E9 and computes ST(1) - ST(0). AT&T `fsubp %st, %st(1)` names DE E1 instead.
    CHECK(Encode([](const X64Enc &e) { e.FaddpSt1(); }) == Bytes{0xDE, 0xC1});
    CHECK(Encode([](const X64Enc &e) { e.FsubpSt1(); }) == Bytes{0xDE, 0xE9});
    CHECK(Encode([](const X64Enc &e) { e.FsubrpSt1(); }) == Bytes{0xDE, 0xE1});
    CHECK(Encode([](const X64Enc &e) { e.FmulpSt1(); }) == Bytes{0xDE, 0xC9});
    CHECK(Encode([](const X64Enc &e) { e.FdivpSt1(); }) == Bytes{0xDE, 0xF9});
    CHECK(Encode([](const X64Enc &e) { e.FdivrpSt1(); }) == Bytes{0xDE, 0xF1});
    CHECK(Encode([](const X64Enc &e) { e.Fprem(); }) == Bytes{0xD9, 0xF8});
    CHECK(Encode([](const X64Enc &e) { e.Fchs(); }) == Bytes{0xD9, 0xE0});
}

TEST_CASE("x87 status, comparison and stack-register forms") {
    CHECK(Encode([](const X64Enc &e) { e.FnstswAx(); }) == Bytes{0xDF, 0xE0});
    CHECK(Encode([](const X64Enc &e) { e.FucomipSt(1); }) == Bytes{0xDF, 0xE9});
    CHECK(Encode([](const X64Enc &e) { e.FucomipSt(3); }) == Bytes{0xDF, 0xEB});
    CHECK(Encode([](const X64Enc &e) { e.FstpSt(0); }) == Bytes{0xDD, 0xD8});
    CHECK(Encode([](const X64Enc &e) { e.FstpSt(1); }) == Bytes{0xDD, 0xD9});
    CHECK(Encode([](const X64Enc &e) { e.FstpSt(7); }) == Bytes{0xDD, 0xDF});
    CHECK(Encode([](const X64Enc &e) { e.FldSt(0); }) == Bytes{0xD9, 0xC0});
    CHECK(Encode([](const X64Enc &e) { e.FldSt(1); }) == Bytes{0xD9, 0xC1});
    CHECK(Encode([](const X64Enc &e) { e.FxchSt(1); }) == Bytes{0xD9, 0xC9});
    CHECK(Encode([](const X64Enc &e) { e.FxchSt(2); }) == Bytes{0xD9, 0xCA});
}

TEST_CASE("x87 control word and the integer helpers around it") {
    CHECK(Encode([](const X64Enc &e) { e.Fnstcw(-32); }) == Bytes{0xD9, 0xBD, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.Fldcw(-30); }) == Bytes{0xD9, 0xAD, 0xE2, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.MovWordStoreImm16(-32, 0x037F); }) ==
          Bytes{0x66, 0xC7, 0x85, 0xE0, 0xFF, 0xFF, 0xFF, 0x7F, 0x03});
    CHECK(Encode([](const X64Enc &e) { e.MovDwordStoreImm32(-20, 0); }) ==
          Bytes{0xC7, 0x85, 0xEC, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00});
    CHECK(Encode([](const X64Enc &e) { e.MovDwordStoreImm32(-20, -2); }) ==
          Bytes{0xC7, 0x85, 0xEC, 0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.TestAhImm8(0x04); }) == Bytes{0xF6, 0xC4, 0x04});
}

TEST_CASE("x87 encodings compose into a precision-pinned subtraction and an fprem loop") {
    // fnstcw [rbp-48]; mov word [rbp-46], 0x037F; fldcw [rbp-46]; fld a; fld b; fsubp (a - b); fstp result;
    // zero the padding; fldcw [rbp-48].
    const Bytes subtraction = Encode([](const X64Enc &e) {
        e.Fnstcw(-48);
        e.MovWordStoreImm16(-46, 0x037F);
        e.Fldcw(-46);
        e.FldTbyte(-16);
        e.FldTbyte(-32);
        e.FsubpSt1();
        e.FstpTbyte(-64);
        e.MovWordStoreImm16(-54, 0);
        e.MovDwordStoreImm32(-52, 0);
        e.Fldcw(-48);
    });
    const Bytes expected{0xD9, 0xBD, 0xD0, 0xFF, 0xFF, 0xFF,                         // fnstcw [rbp-48]
                         0x66, 0xC7, 0x85, 0xD2, 0xFF, 0xFF, 0xFF, 0x7F, 0x03,       // mov word [rbp-46], 0x037F
                         0xD9, 0xAD, 0xD2, 0xFF, 0xFF, 0xFF,                         // fldcw [rbp-46]
                         0xDB, 0xAD, 0xF0, 0xFF, 0xFF, 0xFF,                         // fld tbyte [rbp-16]
                         0xDB, 0xAD, 0xE0, 0xFF, 0xFF, 0xFF,                         // fld tbyte [rbp-32]
                         0xDE, 0xE9,                                                 // fsubp st(1), st
                         0xDB, 0xBD, 0xC0, 0xFF, 0xFF, 0xFF,                         // fstp tbyte [rbp-64]
                         0x66, 0xC7, 0x85, 0xCA, 0xFF, 0xFF, 0xFF, 0x00, 0x00,       // mov word [rbp-54], 0
                         0xC7, 0x85, 0xCC, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, // mov dword [rbp-52], 0
                         0xD9, 0xAD, 0xD0, 0xFF, 0xFF, 0xFF};                        // fldcw [rbp-48]
    CHECK(subtraction == expected);

    // loop: fprem; fnstsw ax; test ah, 4; jnz loop. The backward rel32 is patched like any other branch.
    Bytes loop;
    const X64Enc encoder(loop);
    const std::uint32_t top = encoder.Size();
    encoder.Fprem();
    encoder.FnstswAx();
    encoder.TestAhImm8(0x04);
    std::uint32_t patch = 0;
    encoder.Jnz(patch);
    encoder.Patch32(patch, static_cast<std::int32_t>(top) - static_cast<std::int32_t>(patch + 4));
    encoder.FstpSt(1);
    CHECK(loop == Bytes{0xD9, 0xF8, 0xDF, 0xE0, 0xF6, 0xC4, 0x04, 0x0F, 0x85, 0xF3, 0xFF, 0xFF, 0xFF, 0xDD, 0xD9});
}
