#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"

#include <cstdint>
#include <doctest.h>
#include <vector>

using namespace Rux;

namespace {
// One encoder call and the word it must produce.
//
// The expected words come from the encoding diagrams in the ARM Architecture
// Reference Manual section named on each test case, and were cross-checked
// against `llvm-mc -triple=aarch64 -show-encoding` for the same mnemonic. The
// buffer is emptied after every vector so a refusal is visible as an empty
// buffer rather than as a leftover word.
struct A64Vectors {
    std::vector<std::uint8_t> code;
    A64Enc enc{code};

    void Encodes(const A64Status status, const std::uint32_t expected) {
        CHECK(status == A64Status::Ok);
        REQUIRE(code.size() == 4);
        CHECK(enc.WordAt(0) == expected);
        code.clear();
    }

    void Refuses(const A64Status status, const A64Status expected) {
        CHECK(status == expected);
        CHECK(code.empty());
    }
};
} // namespace

TEST_CASE("AArch64 encoder writes little-endian instruction words") {
    std::vector<std::uint8_t> code;
    const A64Enc enc(code);
    CHECK(enc.Size() == 0);

    enc.Word(0xD65F03C0); // RET
    CHECK(enc.Size() == 4);
    CHECK((code == std::vector<std::uint8_t>{0xC0, 0x03, 0x5F, 0xD6}));
    CHECK(enc.WordAt(0) == 0xD65F03C0);

    enc.Word(0);
    CHECK(enc.Size() == 8);
    enc.PatchWord(4, 0x14000000); // B .+0
    CHECK(enc.WordAt(4) == 0x14000000);
    CHECK(enc.WordAt(0) == 0xD65F03C0);
}

TEST_CASE("AArch64 encoder patches an instruction field without disturbing the rest") {
    std::vector<std::uint8_t> code;
    const A64Enc enc(code);
    enc.Word(0x14000000); // B, imm26 at bit 0

    enc.PatchField(0, 0, 26, 0x3FFFFFF);
    CHECK(enc.WordAt(0) == 0x17FFFFFF);
    enc.PatchField(0, 0, 26, 2);
    CHECK(enc.WordAt(0) == 0x14000002);

    // Bits above the field width are discarded rather than bleeding upwards.
    enc.PatchField(0, 0, 26, 0xFFFFFFFF);
    CHECK(enc.WordAt(0) == 0x17FFFFFF);

    // B.cond: imm19 at bit 5, cond in the low four bits.
    enc.Word(0x54000000);
    enc.PatchField(4, 5, 19, 0x7FFFF);
    enc.PatchField(4, 0, 4, static_cast<std::uint32_t>(A64Condition::Ne));
    CHECK(enc.WordAt(4) == 0x54FFFFE1);
}

TEST_CASE("AArch64 register model distinguishes the zero register from the stack pointer") {
    CHECK(A64::Xn(0).code == 0);
    CHECK(A64::Xn(30).code == 30);
    CHECK(A64::Xn(0).bits == 64);
    CHECK(A64::Wn(7).bits == 32);
    CHECK(A64::Xn(0).Sf() == 1);
    CHECK(A64::Wn(0).Sf() == 0);
    CHECK(A64::Gpr(3, 64) == A64::Xn(3));
    CHECK(A64::Gpr(3, 32) == A64::Wn(3));

    // Code 31 is XZR everywhere except where an encoder accepts SP.
    CHECK(A64::Xzr.code == 31);
    CHECK(A64::Sp.code == 31);
    CHECK(A64::Xzr != A64::Sp);
    CHECK(A64::Xzr.IsZeroReg());
    CHECK_FALSE(A64::Xzr.IsStackPointer());
    CHECK(A64::Sp.IsStackPointer());
    CHECK_FALSE(A64::Sp.IsZeroReg());
    CHECK(A64::Wzr.IsZeroReg());
    CHECK(A64::Wsp.IsStackPointer());
    CHECK(A64::Wsp.bits == 32);
    CHECK(A64::Xn(31) == A64::Xzr);

    // Reserved registers.
    CHECK(A64::Ip0 == A64::Xn(16));
    CHECK(A64::Ip1 == A64::Xn(17));
    CHECK(A64::PlatformReg == A64::Xn(18));
    CHECK(A64::Fp == A64::Xn(29));
    CHECK(A64::Lr == A64::Xn(30));
}

TEST_CASE("AArch64 register model gives every vector register five width views") {
    CHECK(A64::Bn(0).bits == 8);
    CHECK(A64::Hn(0).bits == 16);
    CHECK(A64::Sn(0).bits == 32);
    CHECK(A64::Dn(0).bits == 64);
    CHECK(A64::Qn(0).bits == 128);
    CHECK(A64::Vn(31, 64) == A64::Dn(31));
    CHECK(A64::Dn(5).code == 5);
    CHECK(A64::Dn(5).IsVector());
    CHECK_FALSE(A64::Dn(5).IsGeneral());
    CHECK_FALSE(A64::Xn(5).IsVector());

    // A vector register never reads as the zero register or the stack pointer,
    // whatever its code.
    CHECK_FALSE(A64::Qn(31).IsZeroReg());
    CHECK_FALSE(A64::Qn(31).IsStackPointer());
    CHECK(A64::Dn(0) != A64::Xn(0));
}

TEST_CASE("AArch64 condition codes invert in pairs") {
    CHECK(A64::InvertCondition(A64Condition::Eq) == A64Condition::Ne);
    CHECK(A64::InvertCondition(A64Condition::Ne) == A64Condition::Eq);
    CHECK(A64::InvertCondition(A64Condition::Hi) == A64Condition::Ls);
    CHECK(A64::InvertCondition(A64Condition::Ge) == A64Condition::Lt);
    CHECK(A64::InvertCondition(A64Condition::Gt) == A64Condition::Le);
    CHECK(A64::InvertCondition(A64Condition::Cs) == A64Condition::Cc);
    CHECK(A64::InvertCondition(A64Condition::Al) == A64Condition::Nv);
    CHECK(A64::Hs == A64Condition::Cs);
    CHECK(A64::Lo == A64Condition::Cc);

    // Inversion is its own inverse for all sixteen encodings.
    for (std::uint8_t i = 0; i < 16; ++i) {
        const auto cond = static_cast<A64Condition>(i);
        CHECK(A64::InvertCondition(A64::InvertCondition(cond)) == cond);
    }
}

TEST_CASE("AArch64 shift and extend kinds carry their encoding field values") {
    CHECK(static_cast<int>(A64ShiftKind::Lsl) == 0);
    CHECK(static_cast<int>(A64ShiftKind::Lsr) == 1);
    CHECK(static_cast<int>(A64ShiftKind::Asr) == 2);
    CHECK(static_cast<int>(A64ShiftKind::Ror) == 3);
    CHECK(static_cast<int>(A64ExtendKind::Uxtb) == 0);
    CHECK(static_cast<int>(A64ExtendKind::Uxtx) == 3);
    CHECK(static_cast<int>(A64ExtendKind::Sxtb) == 4);
    CHECK(static_cast<int>(A64ExtendKind::Sxtx) == 7);
}

TEST_CASE("AArch64 barrier options carry their CRm field values") {
    CHECK(static_cast<int>(A64Barrier::Oshld) == 1);
    CHECK(static_cast<int>(A64Barrier::Osh) == 3);
    CHECK(static_cast<int>(A64Barrier::Nsh) == 7);
    CHECK(static_cast<int>(A64Barrier::Ish) == 11);
    CHECK(static_cast<int>(A64Barrier::Ld) == 13);
    CHECK(static_cast<int>(A64Barrier::St) == 14);
    CHECK(static_cast<int>(A64Barrier::Sy) == 15);
}

// System register encodings from the ARM Architecture Reference Manual, D19.2:
// the 15-bit o0:op1:CRn:CRm:op2 an MRS or MSR names a register by.
TEST_CASE("AArch64 system registers encode as fifteen bits of field") {
    CHECK(A64::SysReg(3, 3, 4, 2, 0) == 0x5A10);
    CHECK(A64::Nzcv == 0x5A10);
    CHECK(A64::SysReg(3, 3, 13, 0, 2) == 0x5E82);
    CHECK(A64::TpidrEl0 == 0x5E82);

    // MIDR_EL1 is the one register of interest whose op1 is zero, and FPCR the
    // one whose CRm is not; both exercise a field the two named ones leave at a
    // constant.
    CHECK(A64::SysReg(3, 0, 0, 0, 0) == 0x4000);
    CHECK(A64::SysReg(3, 3, 4, 4, 0) == 0x5A20);

    // op0 is 2 or 3 for everything these instructions reach, so only its low
    // bit travels; every other field is masked to its own width.
    CHECK(A64::SysReg(2, 0, 0, 0, 0) == 0);
    CHECK(A64::SysReg(3, 7, 15, 15, 7) == 0x7FFF);
}

// Expected N:immr:imms triples cross-checked against the logical-immediate
// forms in the ARM Architecture Reference Manual, C4.1.93 (AND immediate) and
// the DecodeBitMasks pseudocode in J1.3.
TEST_CASE("AArch64 bitmask immediates encode replicated runs of ones") {
    auto encode = [](const std::uint64_t value, const bool is64) {
        const auto imm = TryEncodeBitmaskImm(value, is64);
        REQUIRE(imm.has_value());
        return *imm;
    };

    SUBCASE("64-bit elements") {
        const auto lowByte = encode(0xFF, true); // AND Xd, Xn, #0xff
        CHECK(lowByte.n == 1);
        CHECK(lowByte.immr == 0);
        CHECK(lowByte.imms == 7);

        const auto one = encode(1, true);
        CHECK(one.n == 1);
        CHECK(one.immr == 0);
        CHECK(one.imms == 0);

        // A run that wraps the top of the register: only the low bit is clear.
        const auto allButOne = encode(0xFFFFFFFFFFFFFFFEULL, true);
        CHECK(allButOne.n == 1);
        CHECK(allButOne.immr == 63);
        CHECK(allButOne.imms == 62);

        // A run sitting at the top of the register.
        const auto highNibble = encode(0xF000000000000000ULL, true);
        CHECK(highNibble.n == 1);
        CHECK(highNibble.immr == 4);
        CHECK(highNibble.imms == 3);
    }

    SUBCASE("narrower elements replicate") {
        // 0b01 repeated 32 times: a two-bit element, so the imms prefix is
        // 0b11110 and the N bit is clear.
        const auto alternating = encode(0x5555555555555555ULL, true);
        CHECK(alternating.n == 0);
        CHECK(alternating.immr == 0);
        CHECK(alternating.imms == 60);

        // The same pattern rotated by one.
        const auto alternatingHigh = encode(0xAAAAAAAAAAAAAAAAULL, true);
        CHECK(alternatingHigh.n == 0);
        CHECK(alternatingHigh.immr == 1);
        CHECK(alternatingHigh.imms == 60);

        // 0x00FF repeated in 16-bit elements: prefix 0b10, run of eight.
        const auto bytes = encode(0x00FF00FF00FF00FFULL, true);
        CHECK(bytes.n == 0);
        CHECK(bytes.immr == 0);
        CHECK(bytes.imms == 0x27);
    }

    SUBCASE("32-bit forms") {
        const auto lowByte = encode(0xFF, false); // AND Wd, Wn, #0xff
        CHECK(lowByte.n == 0);
        CHECK(lowByte.immr == 0);
        CHECK(lowByte.imms == 7);

        // The 32-bit instruction never sets N, whatever the element size.
        const auto alternating = encode(0x55555555U, false);
        CHECK(alternating.n == 0);
        CHECK(alternating.imms == 60);
    }
}

TEST_CASE("AArch64 bitmask immediates reject values that have no encoding") {
    CHECK_FALSE(TryEncodeBitmaskImm(0, true).has_value());
    CHECK_FALSE(TryEncodeBitmaskImm(0, false).has_value());
    CHECK_FALSE(TryEncodeBitmaskImm(0xFFFFFFFFFFFFFFFFULL, true).has_value());
    CHECK_FALSE(TryEncodeBitmaskImm(0xFFFFFFFFU, false).has_value());
    // Two separate runs of ones.
    CHECK_FALSE(TryEncodeBitmaskImm(0x1234, true).has_value());
    CHECK_FALSE(TryEncodeBitmaskImm(0x101, true).has_value());
    // Wider than the 32-bit instruction can reach.
    CHECK_FALSE(TryEncodeBitmaskImm(0x1FFFFFFFFULL, false).has_value());
    // A pattern that only repeats in 64-bit elements is still fine there.
    CHECK(TryEncodeBitmaskImm(0xFFFFFFFFULL, true).has_value());
}

TEST_CASE("AArch64 arithmetic immediates cover the two shift positions") {
    auto encode = [](const std::uint64_t value) {
        const auto imm = TryEncodeArithImm12(value);
        REQUIRE(imm.has_value());
        return *imm;
    };

    CHECK(encode(0).imm12 == 0);
    CHECK_FALSE(encode(0).shift12);
    CHECK(encode(0xFFF).imm12 == 0xFFF);
    CHECK_FALSE(encode(0xFFF).shift12);

    // Above the unshifted range the value must be a multiple of 4096.
    CHECK(encode(0x1000).imm12 == 1);
    CHECK(encode(0x1000).shift12);
    CHECK(encode(0xFFF000).imm12 == 0xFFF);
    CHECK(encode(0xFFF000).shift12);

    CHECK_FALSE(TryEncodeArithImm12(0x1001).has_value());
    CHECK_FALSE(TryEncodeArithImm12(0xFFFFFF).has_value());
    CHECK_FALSE(TryEncodeArithImm12(0x1000000).has_value());
}

TEST_CASE("AArch64 MOVZ / MOVN immediates select a halfword, a shift and an inversion") {
    auto encode = [](const std::uint64_t value, const bool is64) {
        const auto imm = TryEncodeMovwImm(value, is64);
        REQUIRE(imm.has_value());
        return *imm;
    };

    SUBCASE("MOVZ") {
        const auto zero = encode(0, true);
        CHECK(zero.imm16 == 0);
        CHECK(zero.hw == 0);
        CHECK_FALSE(zero.inverted);

        CHECK(encode(0xFFFF, true).hw == 0);
        CHECK(encode(0xFFFF, true).imm16 == 0xFFFF);
        CHECK(encode(0x12340000ULL, true).hw == 1);
        CHECK(encode(0x12340000ULL, true).imm16 == 0x1234);
        CHECK(encode(0xABCD00000000ULL, true).hw == 2);
        CHECK(encode(0x1234000000000000ULL, true).hw == 3);
        CHECK(encode(0x1234000000000000ULL, true).imm16 == 0x1234);
    }

    SUBCASE("MOVN") {
        // -1 is MOVN #0.
        const auto minusOne = encode(0xFFFFFFFFFFFFFFFFULL, true);
        CHECK(minusOne.imm16 == 0);
        CHECK(minusOne.hw == 0);
        CHECK(minusOne.inverted);

        const auto minusPattern = encode(~0x1234ULL, true);
        CHECK(minusPattern.imm16 == 0x1234);
        CHECK(minusPattern.hw == 0);
        CHECK(minusPattern.inverted);

        const auto high = encode(0x0000FFFFFFFFFFFFULL, true);
        CHECK(high.imm16 == 0xFFFF);
        CHECK(high.hw == 3);
        CHECK(high.inverted);
    }

    SUBCASE("32-bit forms") {
        CHECK(encode(0xFFFF0000U, false).hw == 1);
        CHECK_FALSE(encode(0xFFFF0000U, false).inverted);

        // The inverse is taken within 32 bits, so the upper half is not ones.
        const auto inverted = encode(0xFFFFFFF0U, false);
        CHECK(inverted.imm16 == 0xF);
        CHECK(inverted.hw == 0);
        CHECK(inverted.inverted);
    }
}

TEST_CASE("AArch64 MOVZ / MOVN immediates reject values needing a MOVK chain") {
    CHECK_FALSE(TryEncodeMovwImm(0x12345, true).has_value());
    CHECK_FALSE(TryEncodeMovwImm(0x1234000000005678ULL, true).has_value());
    CHECK_FALSE(TryEncodeMovwImm(0xFFFF0000FFFF0000ULL, true).has_value());
    // Inverting is enough when the ones are one halfword away from filling the
    // register, however high that halfword sits.
    CHECK(TryEncodeMovwImm(0xFFFFFFFF0000FFFFULL, true).has_value());
    // Wider than the 32-bit instruction can reach.
    CHECK_FALSE(TryEncodeMovwImm(0x100000000ULL, false).has_value());
    CHECK(TryEncodeMovwImm(0x100000000ULL, true).has_value());
}

// C4.1.92, Add/subtract (immediate).
TEST_CASE("AArch64 encodes add and subtract immediates") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);

    SUBCASE("64-bit") {
        v.Encodes(v.enc.AddImm(x3, x5, 0), 0x910000A3);
        v.Encodes(v.enc.AddImm(x3, x5, 1), 0x910004A3);
        v.Encodes(v.enc.AddsImm(x3, x5, 1), 0xB10004A3);
        v.Encodes(v.enc.SubImm(x3, x5, 1), 0xD10004A3);
        v.Encodes(v.enc.SubsImm(x3, x5, 1), 0xF10004A3);

        // The boundary of the unshifted field, and the first value above it
        // that the LSL #12 form can still reach.
        v.Encodes(v.enc.AddImm(x3, x5, 0xFFF), 0x913FFCA3);
        v.Encodes(v.enc.AddImm(x3, x5, 0x1000), 0x914004A3);
        v.Encodes(v.enc.SubImm(x3, x5, 0xFFF000), 0xD17FFCA3);
    }

    SUBCASE("32-bit") {
        v.Encodes(v.enc.AddImm(w3, w5, 1), 0x110004A3);
        v.Encodes(v.enc.AddsImm(w3, w5, 4095), 0x313FFCA3);
        v.Encodes(v.enc.SubImm(w3, w5, 4096), 0x514004A3);
        v.Encodes(v.enc.SubsImm(w3, w5, 0xFFF000), 0x717FFCA3);
    }

    SUBCASE("stack pointer and zero register") {
        v.Encodes(v.enc.SubImm(A64::Sp, A64::Sp, 16), 0xD10043FF);
        v.Encodes(v.enc.AddImm(A64::Sp, A64::Sp, 16), 0x910043FF);
        v.Encodes(v.enc.AddImm(A64::Sp, A64::Sp, 0x1000), 0x914007FF);
        v.Encodes(v.enc.AddImm(A64::Wsp, A64::Wsp, 1), 0x110007FF);
        // CMP Xn, #imm and CMN Xn, #imm: the flag-setting forms writing XZR.
        v.Encodes(v.enc.SubsImm(A64::Xzr, A64::Sp, 1), 0xF10007FF);
        v.Encodes(v.enc.AddsImm(A64::Xzr, x5, 1), 0xB10004BF);
        v.Encodes(v.enc.AddsImm(A64::Wzr, w5, 4095), 0x313FFCBF);
    }

    SUBCASE("refusals") {
        // Between the two shift positions there is nothing.
        v.Refuses(v.enc.AddImm(x3, x5, 0x1001), A64Status::InvalidImmediate);
        v.Refuses(v.enc.AddImm(x3, x5, 0x1000000), A64Status::InvalidImmediate);
        // ADDS and SUBS read the stack pointer but cannot write it, while ADD
        // and SUB cannot write the zero register.
        v.Refuses(v.enc.AddsImm(A64::Sp, x5, 0), A64Status::InvalidRegister);
        v.Refuses(v.enc.AddImm(A64::Xzr, x5, 0), A64Status::InvalidRegister);
        v.Refuses(v.enc.AddImm(x3, A64::Xzr, 0), A64Status::InvalidRegister);
        // Mixed widths and the wrong register file.
        v.Refuses(v.enc.AddImm(x3, w5, 0), A64Status::InvalidRegister);
        v.Refuses(v.enc.AddImm(A64::Dn(3), x5, 0), A64Status::InvalidRegister);
    }
}

// C4.1.93, Logical (immediate).
TEST_CASE("AArch64 encodes logical immediates") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);

    SUBCASE("64-bit") {
        v.Encodes(v.enc.AndImm(x3, x5, 0xFF), 0x92401CA3);
        v.Encodes(v.enc.OrrImm(x3, x5, 0xFF), 0xB2401CA3);
        v.Encodes(v.enc.EorImm(x3, x5, 0xFF), 0xD2401CA3);
        v.Encodes(v.enc.AndsImm(x3, x5, 0xFF), 0xF2401CA3);
        v.Encodes(v.enc.AndImm(x3, x5, 1), 0x924000A3);
        v.Encodes(v.enc.AndImm(x3, x5, 0xFFFFFFFF), 0x92407CA3);
        // A pattern that only encodes because it replicates in 2-bit elements.
        v.Encodes(v.enc.OrrImm(x3, x5, 0x5555555555555555ULL), 0xB200F0A3);
    }

    SUBCASE("32-bit") {
        v.Encodes(v.enc.AndImm(w3, w5, 0xFF), 0x12001CA3);
        v.Encodes(v.enc.OrrImm(w3, w5, 1), 0x320000A3);
        v.Encodes(v.enc.EorImm(w3, w5, 0xFFFFFF00U), 0x52185CA3);
        v.Encodes(v.enc.AndsImm(w3, w5, 0x3FFC), 0x721E2CA3);
        v.Encodes(v.enc.AndImm(w3, w5, 0x80000001U), 0x120104A3);
    }

    SUBCASE("stack pointer and zero register") {
        // AND, ORR and EOR write SP; ANDS writes XZR for its TST alias.
        v.Encodes(v.enc.AndImm(A64::Sp, x5, 0xFF), 0x92401CBF);
        v.Encodes(v.enc.AndImm(A64::Wsp, w5, 0xFF), 0x12001CBF);
        v.Encodes(v.enc.AndsImm(A64::Wzr, w5, 0x3FFC), 0x721E2CBF);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.AndImm(x3, x5, 0), A64Status::InvalidImmediate);
        v.Refuses(v.enc.AndImm(x3, x5, 0xFFFFFFFFFFFFFFFFULL), A64Status::InvalidImmediate);
        v.Refuses(v.enc.OrrImm(x3, x5, 0x1234), A64Status::InvalidImmediate);
        // Wider than the 32-bit instruction can reach.
        v.Refuses(v.enc.AndImm(w3, w5, 0x1FFFFFFFFULL), A64Status::InvalidImmediate);
        v.Refuses(v.enc.AndsImm(A64::Sp, x5, 0xFF), A64Status::InvalidRegister);
        v.Refuses(v.enc.AndImm(x3, A64::Sp, 0xFF), A64Status::InvalidRegister);
        v.Refuses(v.enc.AndImm(x3, w5, 0xFF), A64Status::InvalidRegister);
    }
}

// C4.1.94, Move wide (immediate).
TEST_CASE("AArch64 encodes the move-wide group at every halfword") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg w3 = A64::Wn(3);

    SUBCASE("all four halfwords") {
        v.Encodes(v.enc.Movz(x3, 1), 0xD2800023);
        v.Encodes(v.enc.Movz(x3, 0x1234, 16), 0xD2A24683);
        v.Encodes(v.enc.Movz(x3, 0x1234, 32), 0xD2C24683);
        v.Encodes(v.enc.Movz(x3, 0x1234, 48), 0xD2E24683);
        v.Encodes(v.enc.Movn(x3, 1), 0x92800023);
        v.Encodes(v.enc.Movn(x3, 0x1234, 48), 0x92E24683);
        v.Encodes(v.enc.Movk(x3, 1, 16), 0xF2A00023);
        v.Encodes(v.enc.Movk(x3, 0x1234, 32), 0xF2C24683);
    }

    SUBCASE("32-bit") {
        v.Encodes(v.enc.Movz(w3, 0), 0x52800003);
        v.Encodes(v.enc.Movz(w3, 0xFFFF), 0x529FFFE3);
        v.Encodes(v.enc.Movn(w3, 0x1234), 0x12824683);
        v.Encodes(v.enc.Movk(w3, 0x1234, 16), 0x72A24683);
    }

    SUBCASE("refusals") {
        // A W register has no third or fourth halfword.
        v.Refuses(v.enc.Movz(w3, 1, 32), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Movz(x3, 1, 64), A64Status::InvalidImmediate);
        // The shift names a halfword, not an arbitrary bit position.
        v.Refuses(v.enc.Movk(x3, 1, 8), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Movz(A64::Sp, 1), A64Status::InvalidRegister);
    }
}

// C4.1.91, PC-relative addressing.
TEST_CASE("AArch64 encodes ADR and ADRP with a split immediate") {
    A64Vectors v;
    const A64Reg x9 = A64::Xn(9);

    SUBCASE("ADR reaches a megabyte either way") {
        v.Encodes(v.enc.Adr(x9, 0), 0x10000009);
        v.Encodes(v.enc.Adr(x9, 4), 0x10000029);
        v.Encodes(v.enc.Adr(x9, -4), 0x10FFFFE9);
        v.Encodes(v.enc.Adr(x9, 1048572), 0x107FFFE9);
        v.Encodes(v.enc.Adr(x9, -1048576), 0x10800009);
    }

    SUBCASE("ADRP counts pages") {
        v.Encodes(v.enc.Adrp(x9, 0), 0x90000009);
        v.Encodes(v.enc.Adrp(x9, 4096), 0xB0000009);
        v.Encodes(v.enc.Adrp(x9, -4096), 0xF0FFFFE9);
        v.Encodes(v.enc.Adrp(x9, 4294963200), 0xF07FFFE9);
        v.Encodes(v.enc.Adrp(x9, -4294967296), 0x90800009);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.Adr(x9, 1048576), A64Status::OutOfRange);
        v.Refuses(v.enc.Adr(x9, -1048580), A64Status::OutOfRange);
        v.Refuses(v.enc.Adrp(x9, 2048), A64Status::Unaligned);
        v.Refuses(v.enc.Adrp(x9, 4294967296), A64Status::OutOfRange);
        v.Refuses(v.enc.Adr(A64::Wn(9), 0), A64Status::InvalidRegister);
        v.Refuses(v.enc.Adr(A64::Sp, 0), A64Status::InvalidRegister);
    }
}

// C4.1.95, Bitfield, and the aliases in C6.2.
TEST_CASE("AArch64 encodes bitfield moves and their aliases") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);

    SUBCASE("the three instructions") {
        v.Encodes(v.enc.Sbfm(x3, x5, 0, 7), 0x93401CA3);
        v.Encodes(v.enc.Ubfm(x3, x5, 0, 7), 0xD3401CA3);
        v.Encodes(v.enc.Bfm(x3, x5, 0, 7), 0xB3401CA3);
        v.Encodes(v.enc.Sbfm(x3, x5, 5, 63), 0x9345FCA3);
        v.Encodes(v.enc.Sbfm(w3, w5, 5, 7), 0x13051CA3);
        v.Encodes(v.enc.Ubfm(w3, w5, 0, 0), 0x530000A3);
    }

    SUBCASE("shifts") {
        // LSL rotates by the complement of its shift, so #0 is a plain move
        // and #1 sits at the far end of the immr field.
        v.Encodes(v.enc.Lsl(x3, x5, 0), 0xD340FCA3);
        v.Encodes(v.enc.Lsl(x3, x5, 1), 0xD37FF8A3);
        v.Encodes(v.enc.Lsl(x3, x5, 4), 0xD37CECA3);
        v.Encodes(v.enc.Lsl(x3, x5, 63), 0xD34100A3);
        v.Encodes(v.enc.Lsl(w3, w5, 31), 0x530100A3);
        v.Encodes(v.enc.Lsr(x3, x5, 1), 0xD341FCA3);
        v.Encodes(v.enc.Lsr(w3, w5, 3), 0x53037CA3);
        v.Encodes(v.enc.Asr(x3, x5, 1), 0x9341FCA3);
        v.Encodes(v.enc.Asr(w3, w5, 31), 0x131F7CA3);
    }

    SUBCASE("field extraction and insertion") {
        v.Encodes(v.enc.Ubfx(x3, x5, 6, 16), 0xD34654A3);
        v.Encodes(v.enc.Sbfx(x3, x5, 6, 16), 0x934654A3);
        v.Encodes(v.enc.Bfi(x3, x5, 6, 16), 0xB37A3CA3);
        v.Encodes(v.enc.Bfxil(x3, x5, 6, 16), 0xB34654A3);
        v.Encodes(v.enc.Ubfx(w3, w5, 3, 6), 0x530320A3);
        v.Encodes(v.enc.Sbfx(w3, w5, 3, 11), 0x130334A3);
        v.Encodes(v.enc.Bfi(w3, w5, 3, 6), 0x331D14A3);
        v.Encodes(v.enc.Bfxil(w3, w5, 3, 6), 0x330320A3);
        // A field at bit 0 makes BFI and BFXIL the same instruction.
        v.Encodes(v.enc.Bfi(x3, x5, 0, 1), 0xB34000A3);
        v.Encodes(v.enc.Bfxil(x3, x5, 0, 1), 0xB34000A3);
    }

    SUBCASE("extensions") {
        v.Encodes(v.enc.Sxtb(x3, w5), 0x93401CA3);
        v.Encodes(v.enc.Sxth(x3, w5), 0x93403CA3);
        v.Encodes(v.enc.Sxtw(x3, w5), 0x93407CA3);
        v.Encodes(v.enc.Sxtb(w3, w5), 0x13001CA3);
        v.Encodes(v.enc.Sxth(w3, w5), 0x13003CA3);
        v.Encodes(v.enc.Uxtb(w3, w5), 0x53001CA3);
        v.Encodes(v.enc.Uxth(w3, w5), 0x53003CA3);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.Sbfm(x3, x5, 64, 0), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Ubfm(w3, w5, 0, 32), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Lsl(x3, x5, 64), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Lsr(w3, w5, 32), A64Status::InvalidImmediate);
        // A field must have at least one bit and must fit the register.
        v.Refuses(v.enc.Ubfx(x3, x5, 0, 0), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Ubfx(x3, x5, 60, 8), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Bfi(w3, w5, 30, 4), A64Status::InvalidImmediate);
        // The source of an extension is always a W register, and UXTB and UXTH
        // have no 64-bit form.
        v.Refuses(v.enc.Sxtb(x3, x5), A64Status::InvalidRegister);
        v.Refuses(v.enc.Sxtw(w3, w5), A64Status::InvalidRegister);
        v.Refuses(v.enc.Uxtb(x3, w5), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ubfm(A64::Sp, x5, 0, 7), A64Status::InvalidRegister);
    }
}

// C4.1.96, Extract.
TEST_CASE("AArch64 encodes EXTR and its ROR alias") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg x7 = A64::Xn(7);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);
    const A64Reg w7 = A64::Wn(7);

    v.Encodes(v.enc.Extr(x3, x5, x7, 1), 0x93C704A3);
    v.Encodes(v.enc.Extr(x3, x5, x7, 63), 0x93C7FCA3);
    v.Encodes(v.enc.Extr(w3, w5, w7, 31), 0x13877CA3);
    // ROR is the form that reads one register twice.
    v.Encodes(v.enc.Ror(x3, x5, 4), 0x93C510A3);
    v.Encodes(v.enc.Ror(w3, w5, 3), 0x13850CA3);

    v.Refuses(v.enc.Extr(x3, x5, x7, 64), A64Status::InvalidImmediate);
    v.Refuses(v.enc.Extr(w3, w5, w7, 32), A64Status::InvalidImmediate);
    v.Refuses(v.enc.Extr(x3, x5, w7, 0), A64Status::InvalidRegister);
    v.Refuses(v.enc.Ror(A64::Sp, x5, 1), A64Status::InvalidRegister);
}

// Data Processing — Register: Add/subtract (shifted register).
TEST_CASE("AArch64 encodes shifted-register add and subtract") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg x7 = A64::Xn(7);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);
    const A64Reg w7 = A64::Wn(7);

    SUBCASE("64-bit") {
        v.Encodes(v.enc.Add(x3, x5, x7), 0x8B0700A3);
        v.Encodes(v.enc.Adds(x3, x5, x7), 0xAB0700A3);
        v.Encodes(v.enc.Sub(x3, x5, x7), 0xCB0700A3);
        v.Encodes(v.enc.Subs(x3, x5, x7), 0xEB0700A3);
        v.Encodes(v.enc.Add(x3, x5, x7, A64ShiftKind::Lsl, 12), 0x8B0730A3);
        v.Encodes(v.enc.Add(x3, x5, x7, A64ShiftKind::Lsr, 63), 0x8B47FCA3);
        v.Encodes(v.enc.Add(x3, x5, x7, A64ShiftKind::Asr, 1), 0x8B8704A3);
        v.Encodes(v.enc.Sub(x3, x5, x7, A64ShiftKind::Asr, 63), 0xCB87FCA3);
        // Code 31 is the zero register in every field of this form.
        v.Encodes(v.enc.Add(x3, x5, A64::Xzr), 0x8B1F00A3);
        v.Encodes(v.enc.Add(A64::Xzr, x5, x7), 0x8B0700BF);
    }

    SUBCASE("32-bit") {
        v.Encodes(v.enc.Add(w3, w5, w7), 0x0B0700A3);
        v.Encodes(v.enc.Adds(w3, w5, w7, A64ShiftKind::Lsl, 31), 0x2B077CA3);
        v.Encodes(v.enc.Sub(w3, w5, w7, A64ShiftKind::Lsr, 1), 0x4B4704A3);
        v.Encodes(v.enc.Subs(w3, w5, w7, A64ShiftKind::Asr, 31), 0x6B877CA3);
    }

    SUBCASE("aliases") {
        v.Encodes(v.enc.Cmp(x5, x7), 0xEB0700BF);
        v.Encodes(v.enc.Cmp(x5, x7, A64ShiftKind::Lsl, 3), 0xEB070CBF);
        v.Encodes(v.enc.Cmn(x5, x7), 0xAB0700BF);
        v.Encodes(v.enc.Neg(x3, x7), 0xCB0703E3);
        v.Encodes(v.enc.Negs(x3, x7), 0xEB0703E3);
        v.Encodes(v.enc.Neg(w3, w7, A64ShiftKind::Lsl, 2), 0x4B070BE3);
        v.Encodes(v.enc.Cmp(w5, w7, A64ShiftKind::Asr, 4), 0x6B8710BF);
    }

    SUBCASE("refusals") {
        // The shift amount is a bit position within the register.
        v.Refuses(v.enc.Add(x3, x5, x7, A64ShiftKind::Lsl, 64), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Add(w3, w5, w7, A64ShiftKind::Lsl, 32), A64Status::InvalidImmediate);
        // ROR has no encoding in the arithmetic forms.
        v.Refuses(v.enc.Add(x3, x5, x7, A64ShiftKind::Ror, 1), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Subs(w3, w5, w7, A64ShiftKind::Ror, 0), A64Status::InvalidImmediate);
        // No field of this form reads code 31 as the stack pointer, so SP is
        // reachable only through the extended-register form.
        v.Refuses(v.enc.Add(A64::Sp, x5, x7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Add(x3, A64::Sp, x7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Cmp(A64::Sp, x7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Add(x3, x5, w7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Add(x3, x5, A64::Dn(7)), A64Status::InvalidRegister);
    }
}

// Data Processing — Register: Add/subtract (extended register).
TEST_CASE("AArch64 encodes extended-register add and subtract") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg x7 = A64::Xn(7);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);
    const A64Reg w7 = A64::Wn(7);

    SUBCASE("every option at every shift") {
        v.Encodes(v.enc.AddExt(x3, x5, w7, A64ExtendKind::Uxtb), 0x8B2700A3);
        v.Encodes(v.enc.AddExt(x3, x5, w7, A64ExtendKind::Uxth, 1), 0x8B2724A3);
        v.Encodes(v.enc.AddExt(x3, x5, w7, A64ExtendKind::Uxtw, 2), 0x8B2748A3);
        v.Encodes(v.enc.AddExt(x3, x5, x7, A64ExtendKind::Uxtx, 3), 0x8B276CA3);
        v.Encodes(v.enc.AddExt(x3, x5, w7, A64ExtendKind::Sxtb, 4), 0x8B2790A3);
        v.Encodes(v.enc.AddsExt(x3, x5, w7, A64ExtendKind::Sxth), 0xAB27A0A3);
        v.Encodes(v.enc.SubExt(x3, x5, w7, A64ExtendKind::Sxtw), 0xCB27C0A3);
        v.Encodes(v.enc.SubsExt(x3, x5, x7, A64ExtendKind::Sxtx), 0xEB27E0A3);
    }

    SUBCASE("stack pointer operands") {
        v.Encodes(v.enc.AddExt(A64::Sp, A64::Sp, x7, A64ExtendKind::Uxtx), 0x8B2763FF);
        v.Encodes(v.enc.AddExt(x3, A64::Sp, w7, A64ExtendKind::Uxtw, 2), 0x8B274BE3);
        // CMP SP, Xm is the flag-setting form, which reads SP but writes XZR.
        v.Encodes(v.enc.SubsExt(A64::Xzr, A64::Sp, x7, A64ExtendKind::Sxtx), 0xEB27E3FF);
        v.Encodes(v.enc.AddExt(A64::Wsp, A64::Wsp, w7, A64ExtendKind::Uxtw), 0x0B2743FF);
    }

    SUBCASE("32-bit") {
        v.Encodes(v.enc.AddExt(w3, w5, w7, A64ExtendKind::Uxtb), 0x0B2700A3);
        // A 32-bit instruction has no doubleword source, so even UXTX reads a W
        // register.
        v.Encodes(v.enc.AddsExt(w3, w5, w7, A64ExtendKind::Uxtx, 1), 0x2B2764A3);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.AddExt(x3, x5, w7, A64ExtendKind::Uxtb, 5), A64Status::InvalidImmediate);
        // The extension names the width of the source.
        v.Refuses(v.enc.AddExt(x3, x5, x7, A64ExtendKind::Uxtb), A64Status::InvalidRegister);
        v.Refuses(v.enc.AddExt(x3, x5, w7, A64ExtendKind::Uxtx), A64Status::InvalidRegister);
        v.Refuses(v.enc.AddExt(w3, w5, A64::Xn(7), A64ExtendKind::Uxtx), A64Status::InvalidRegister);
        // The flag-setting forms cannot write SP, and no form reads XZR as rn.
        v.Refuses(v.enc.AddsExt(A64::Sp, A64::Sp, x7, A64ExtendKind::Uxtx), A64Status::InvalidRegister);
        v.Refuses(v.enc.AddExt(x3, A64::Xzr, w7, A64ExtendKind::Uxtb), A64Status::InvalidRegister);
    }
}

// Data Processing — Register: Logical (shifted register).
TEST_CASE("AArch64 encodes the logical shifted-register group") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg x7 = A64::Xn(7);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);
    const A64Reg w7 = A64::Wn(7);

    SUBCASE("the eight operations") {
        v.Encodes(v.enc.And(x3, x5, x7), 0x8A0700A3);
        v.Encodes(v.enc.Bic(x3, x5, x7), 0x8A2700A3);
        v.Encodes(v.enc.Orr(x3, x5, x7), 0xAA0700A3);
        v.Encodes(v.enc.Orn(x3, x5, x7), 0xAA2700A3);
        v.Encodes(v.enc.Eor(x3, x5, x7), 0xCA0700A3);
        v.Encodes(v.enc.Eon(x3, x5, x7), 0xCA2700A3);
        v.Encodes(v.enc.Ands(x3, x5, x7), 0xEA0700A3);
        v.Encodes(v.enc.Bics(x3, x5, x7), 0xEA2700A3);
    }

    SUBCASE("shifts, including the ROR the arithmetic forms lack") {
        v.Encodes(v.enc.And(x3, x5, x7, A64ShiftKind::Lsl, 63), 0x8A07FCA3);
        v.Encodes(v.enc.Orr(x3, x5, x7, A64ShiftKind::Lsr, 1), 0xAA4704A3);
        v.Encodes(v.enc.Eor(x3, x5, x7, A64ShiftKind::Asr, 32), 0xCA8780A3);
        v.Encodes(v.enc.Bic(x3, x5, x7, A64ShiftKind::Ror, 16), 0x8AE740A3);
        v.Encodes(v.enc.And(w3, w5, w7), 0x0A0700A3);
        v.Encodes(v.enc.Ands(w3, w5, w7, A64ShiftKind::Ror, 31), 0x6AC77CA3);
        v.Encodes(v.enc.Orr(w3, w5, w7, A64ShiftKind::Lsl, 1), 0x2A0704A3);
    }

    SUBCASE("aliases") {
        v.Encodes(v.enc.Tst(x5, x7), 0xEA0700BF);
        v.Encodes(v.enc.Tst(w5, w7, A64ShiftKind::Lsl, 3), 0x6A070CBF);
        v.Encodes(v.enc.Mvn(x3, x7), 0xAA2703E3);
        v.Encodes(v.enc.Mvn(w3, w7, A64ShiftKind::Asr, 5), 0x2AA717E3);
        v.Encodes(v.enc.Mov(x3, x7), 0xAA0703E3);
        v.Encodes(v.enc.Mov(w3, w7), 0x2A0703E3);
        v.Encodes(v.enc.Mov(x3, A64::Xzr), 0xAA1F03E3);
        // MOV naming SP is the arithmetic form, since ORR reads code 31 as XZR.
        v.Encodes(v.enc.Mov(A64::Sp, x5), 0x910000BF);
        v.Encodes(v.enc.Mov(x3, A64::Sp), 0x910003E3);
        v.Encodes(v.enc.Mov(A64::Wsp, w5), 0x110000BF);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.And(x3, x5, x7, A64ShiftKind::Lsl, 64), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Orr(w3, w5, w7, A64ShiftKind::Ror, 32), A64Status::InvalidImmediate);
        v.Refuses(v.enc.And(A64::Sp, x5, x7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Tst(A64::Sp, x7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Orr(x3, x5, w7), A64Status::InvalidRegister);
        // Neither reading of code 31 can be moved to the other.
        v.Refuses(v.enc.Mov(A64::Sp, A64::Xzr), A64Status::InvalidRegister);
        v.Refuses(v.enc.Mov(A64::Xzr, A64::Sp), A64Status::InvalidRegister);
        v.Refuses(v.enc.Mov(x3, A64::Wn(7)), A64Status::InvalidRegister);
    }
}

// Data Processing — Register: Data-processing (2 source).
TEST_CASE("AArch64 encodes variable shifts and division") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg x7 = A64::Xn(7);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);
    const A64Reg w7 = A64::Wn(7);

    v.Encodes(v.enc.Lslv(x3, x5, x7), 0x9AC720A3);
    v.Encodes(v.enc.Lsrv(x3, x5, x7), 0x9AC724A3);
    v.Encodes(v.enc.Asrv(x3, x5, x7), 0x9AC728A3);
    v.Encodes(v.enc.Rorv(x3, x5, x7), 0x9AC72CA3);
    v.Encodes(v.enc.Lslv(w3, w5, w7), 0x1AC720A3);
    v.Encodes(v.enc.Rorv(w3, w5, w7), 0x1AC72CA3);

    v.Encodes(v.enc.Sdiv(x3, x5, x7), 0x9AC70CA3);
    v.Encodes(v.enc.Udiv(x3, x5, x7), 0x9AC708A3);
    v.Encodes(v.enc.Sdiv(w3, w5, w7), 0x1AC70CA3);
    v.Encodes(v.enc.Udiv(w3, w5, w7), 0x1AC708A3);

    v.Refuses(v.enc.Lslv(A64::Sp, x5, x7), A64Status::InvalidRegister);
    v.Refuses(v.enc.Sdiv(x3, w5, x7), A64Status::InvalidRegister);
    v.Refuses(v.enc.Udiv(x3, x5, A64::Sp), A64Status::InvalidRegister);
}

// Data Processing — Register: Data-processing (3 source).
TEST_CASE("AArch64 encodes the multiply-accumulate group") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg x7 = A64::Xn(7);
    const A64Reg x11 = A64::Xn(11);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);
    const A64Reg w7 = A64::Wn(7);
    const A64Reg w11 = A64::Wn(11);

    SUBCASE("same-width forms") {
        v.Encodes(v.enc.Madd(x3, x5, x7, x11), 0x9B072CA3);
        v.Encodes(v.enc.Msub(x3, x5, x7, x11), 0x9B07ACA3);
        v.Encodes(v.enc.Mul(x3, x5, x7), 0x9B077CA3);
        v.Encodes(v.enc.Mneg(x3, x5, x7), 0x9B07FCA3);
        v.Encodes(v.enc.Madd(w3, w5, w7, w11), 0x1B072CA3);
        v.Encodes(v.enc.Mul(w3, w5, w7), 0x1B077CA3);
        v.Encodes(v.enc.Mneg(w3, w5, w7), 0x1B07FCA3);
    }

    SUBCASE("widening and high-half forms") {
        v.Encodes(v.enc.Smaddl(x3, w5, w7, x11), 0x9B272CA3);
        v.Encodes(v.enc.Umaddl(x3, w5, w7, x11), 0x9BA72CA3);
        v.Encodes(v.enc.Smull(x3, w5, w7), 0x9B277CA3);
        v.Encodes(v.enc.Umull(x3, w5, w7), 0x9BA77CA3);
        v.Encodes(v.enc.Smulh(x3, x5, x7), 0x9B477CA3);
        v.Encodes(v.enc.Umulh(x3, x5, x7), 0x9BC77CA3);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.Madd(x3, w5, x7, x11), A64Status::InvalidRegister);
        v.Refuses(v.enc.Madd(x3, x5, x7, w11), A64Status::InvalidRegister);
        v.Refuses(v.enc.Mul(A64::Sp, x5, x7), A64Status::InvalidRegister);
        // The widening forms multiply W registers into an X register; nothing
        // else about their operand widths is negotiable.
        v.Refuses(v.enc.Smull(w3, w5, w7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Smull(x3, x5, x7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Umaddl(x3, w5, w7, w11), A64Status::InvalidRegister);
        v.Refuses(v.enc.Smulh(x3, w5, w7), A64Status::InvalidRegister);
        v.Refuses(v.enc.Umulh(w3, w5, w7), A64Status::InvalidRegister);
    }
}

// Data Processing — Register: Conditional select.
TEST_CASE("AArch64 encodes conditional selects and their aliases") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg x7 = A64::Xn(7);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);
    const A64Reg w7 = A64::Wn(7);

    SUBCASE("the four instructions") {
        v.Encodes(v.enc.Csel(x3, x5, x7, A64Condition::Eq), 0x9A8700A3);
        v.Encodes(v.enc.Csinc(x3, x5, x7, A64Condition::Ne), 0x9A8714A3);
        v.Encodes(v.enc.Csinv(x3, x5, x7, A64Condition::Ge), 0xDA87A0A3);
        v.Encodes(v.enc.Csneg(x3, x5, x7, A64Condition::Lt), 0xDA87B4A3);
        v.Encodes(v.enc.Csel(w3, w5, w7, A64Condition::Hi), 0x1A8780A3);
        // The instructions themselves accept AL and NV; only the aliases cannot.
        v.Encodes(v.enc.Csel(x3, x5, x7, A64Condition::Al), 0x9A87E0A3);
        v.Encodes(v.enc.Csel(x3, x5, x7, A64Condition::Nv), 0x9A87F0A3);
    }

    SUBCASE("aliases encode the inverse condition") {
        v.Encodes(v.enc.Cset(x3, A64Condition::Eq), 0x9A9F17E3);
        v.Encodes(v.enc.Cset(w3, A64Condition::Ne), 0x1A9F07E3);
        v.Encodes(v.enc.Csetm(x3, A64Condition::Lt), 0xDA9FA3E3);
        v.Encodes(v.enc.Cinc(x3, x5, A64Condition::Gt), 0x9A85D4A3);
        v.Encodes(v.enc.Cinv(x3, x5, A64Condition::Le), 0xDA85C0A3);
        v.Encodes(v.enc.Cneg(x3, x5, A64Condition::Mi), 0xDA8554A3);
        v.Encodes(v.enc.Cneg(w3, w5, A64Condition::Pl), 0x5A8544A3);
    }

    SUBCASE("refusals") {
        // Inverting AL gives NV, which also means always, so an alias written
        // with either would read as the opposite of itself.
        v.Refuses(v.enc.Cset(x3, A64Condition::Al), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Cset(x3, A64Condition::Nv), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Csetm(w3, A64Condition::Al), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Cinc(x3, x5, A64Condition::Nv), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Cinv(x3, x5, A64Condition::Al), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Cneg(x3, x5, A64Condition::Al), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Csel(A64::Sp, x5, x7, A64Condition::Eq), A64Status::InvalidRegister);
        v.Refuses(v.enc.Csel(x3, w5, x7, A64Condition::Eq), A64Status::InvalidRegister);
        v.Refuses(v.enc.Cset(A64::Sp, A64Condition::Eq), A64Status::InvalidRegister);
    }
}

// Data Processing — Register: Data-processing (1 source).
TEST_CASE("AArch64 encodes bit counting and byte reversal") {
    A64Vectors v;
    const A64Reg x3 = A64::Xn(3);
    const A64Reg x5 = A64::Xn(5);
    const A64Reg w3 = A64::Wn(3);
    const A64Reg w5 = A64::Wn(5);

    v.Encodes(v.enc.Clz(x3, x5), 0xDAC010A3);
    v.Encodes(v.enc.Cls(x3, x5), 0xDAC014A3);
    v.Encodes(v.enc.Rbit(x3, x5), 0xDAC000A3);
    v.Encodes(v.enc.Rev(x3, x5), 0xDAC00CA3);
    v.Encodes(v.enc.Rev16(x3, x5), 0xDAC004A3);
    v.Encodes(v.enc.Rev32(x3, x5), 0xDAC008A3);

    v.Encodes(v.enc.Clz(w3, w5), 0x5AC010A3);
    v.Encodes(v.enc.Cls(w3, w5), 0x5AC014A3);
    v.Encodes(v.enc.Rbit(w3, w5), 0x5AC000A3);
    v.Encodes(v.enc.Rev16(w3, w5), 0x5AC004A3);
    // At 32 bits REV occupies the opcode REV32 has at 64, since reversing the
    // bytes of the only word is reversing the whole register.
    v.Encodes(v.enc.Rev(w3, w5), 0x5AC008A3);

    v.Refuses(v.enc.Rev32(w3, w5), A64Status::InvalidRegister);
    v.Refuses(v.enc.Clz(x3, w5), A64Status::InvalidRegister);
    v.Refuses(v.enc.Rbit(A64::Sp, x5), A64Status::InvalidRegister);
    v.Refuses(v.enc.Rev(x3, A64::Dn(5)), A64Status::InvalidRegister);
}

// Loads and Stores: Load/store register (unsigned immediate).
TEST_CASE("AArch64 encodes unsigned-offset loads and stores") {
    A64Vectors v;
    const A64Reg x0 = A64::Xn(0);
    const A64Reg x1 = A64::Xn(1);
    const A64Reg w0 = A64::Wn(0);

    SUBCASE("general purpose, scaled by the access width") {
        v.Encodes(v.enc.Ldr(x0, x1), 0xF9400020);
        v.Encodes(v.enc.Ldr(x0, x1, 8), 0xF9400420);
        v.Encodes(v.enc.Ldr(x0, x1, 32760), 0xF97FFC20);
        v.Encodes(v.enc.Str(x0, x1), 0xF9000020);
        v.Encodes(v.enc.Str(x0, x1, 8), 0xF9000420);
        v.Encodes(v.enc.Str(x0, x1, 32760), 0xF93FFC20);
        v.Encodes(v.enc.Ldr(w0, x1), 0xB9400020);
        v.Encodes(v.enc.Ldr(w0, x1, 4), 0xB9400420);
        v.Encodes(v.enc.Ldr(w0, x1, 16380), 0xB97FFC20);
        // The base is the one field that reads code 31 as the stack pointer.
        v.Encodes(v.enc.Ldr(x0, A64::Sp, 8), 0xF94007E0);
        v.Encodes(v.enc.Ldr(A64::Xzr, x1), 0xF940003F);
    }

    SUBCASE("all five SIMD widths") {
        v.Encodes(v.enc.Ldr(A64::Bn(0), x1, 3), 0x3D400C20);
        v.Encodes(v.enc.Ldr(A64::Bn(0), x1, 4095), 0x3D7FFC20);
        v.Encodes(v.enc.Ldr(A64::Hn(0), x1, 2), 0x7D400420);
        v.Encodes(v.enc.Ldr(A64::Hn(0), x1, 8190), 0x7D7FFC20);
        v.Encodes(v.enc.Ldr(A64::Sn(0), x1, 4), 0xBD400420);
        v.Encodes(v.enc.Ldr(A64::Dn(0), x1, 8), 0xFD400420);
        // A Q access has no `size` value of its own and borrows a bit of `opc`.
        v.Encodes(v.enc.Ldr(A64::Qn(0), x1), 0x3DC00020);
        v.Encodes(v.enc.Ldr(A64::Qn(0), x1, 65520), 0x3DFFFC20);
        v.Encodes(v.enc.Str(A64::Qn(0), x1, 16), 0x3D800420);
    }

    SUBCASE("narrowing and sign-extending") {
        v.Encodes(v.enc.Ldrb(w0, x1, 3), 0x39400C20);
        v.Encodes(v.enc.Ldrb(w0, x1, 4095), 0x397FFC20);
        v.Encodes(v.enc.Strb(A64::Wn(5), x1, 1), 0x39000425);
        v.Encodes(v.enc.Ldrh(w0, x1, 2), 0x79400420);
        v.Encodes(v.enc.Ldrh(w0, x1, 8190), 0x797FFC20);
        v.Encodes(v.enc.Strh(A64::Wn(5), A64::Sp, 2), 0x790007E5);
        // The sign-extending loads name the width they extend to, so the two
        // widths of LDRSB are two encodings.
        v.Encodes(v.enc.Ldrsb(x0, x1, 1), 0x39800420);
        v.Encodes(v.enc.Ldrsb(A64::Wn(7), x1, 1), 0x39C00427);
        v.Encodes(v.enc.Ldrsh(x0, x1, 2), 0x79800420);
        v.Encodes(v.enc.Ldrsh(A64::Wn(7), x1), 0x79C00027);
        v.Encodes(v.enc.Ldrsw(x0, x1, 4), 0xB9800420);
        v.Encodes(v.enc.Ldrsw(x0, x1, 16380), 0xB9BFFC20);
    }

    SUBCASE("refusals") {
        // An offset that does not divide by the access width has no encoding
        // here at all, and one past the field is out of range, not truncated.
        v.Refuses(v.enc.Ldr(x0, x1, 4), A64Status::Unaligned);
        v.Refuses(v.enc.Ldr(A64::Qn(0), x1, 8), A64Status::Unaligned);
        v.Refuses(v.enc.Ldr(x0, x1, 32768), A64Status::OutOfRange);
        v.Refuses(v.enc.Ldrb(w0, x1, 4096), A64Status::OutOfRange);
        // The base is always a 64-bit register, and never the zero register.
        v.Refuses(v.enc.Ldr(x0, A64::Wn(1)), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ldr(x0, A64::Xzr), A64Status::InvalidRegister);
        // The transferred register never reads code 31 as the stack pointer.
        v.Refuses(v.enc.Str(A64::Sp, x1), A64Status::InvalidRegister);
        // LDRB and LDRH name a W register; LDRSW only widens, so it names an X.
        v.Refuses(v.enc.Ldrb(x0, x1), A64Status::InvalidRegister);
        v.Refuses(v.enc.Strh(x0, x1), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ldrsw(w0, x1), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ldrb(A64::Bn(0), x1), A64Status::InvalidRegister);
    }
}

// Loads and Stores: Load/store register (unscaled immediate), (immediate
// post-indexed) and (immediate pre-indexed), which share one signed 9-bit field.
TEST_CASE("AArch64 encodes unscaled and indexed loads and stores") {
    A64Vectors v;
    const A64Reg x0 = A64::Xn(0);
    const A64Reg x1 = A64::Xn(1);
    const A64Reg w0 = A64::Wn(0);

    SUBCASE("LDUR and STUR reach either side of the base") {
        v.Encodes(v.enc.Ldur(x0, x1), 0xF8400020);
        v.Encodes(v.enc.Ldur(x0, x1, 1), 0xF8401020);
        v.Encodes(v.enc.Ldur(x0, x1, 255), 0xF84FF020);
        v.Encodes(v.enc.Ldur(x0, x1, -1), 0xF85FF020);
        v.Encodes(v.enc.Ldur(x0, x1, -256), 0xF8500020);
        v.Encodes(v.enc.Ldur(x0, A64::Sp, -17), 0xF85EF3E0);
        v.Encodes(v.enc.Ldur(A64::Wn(4), x1, 1), 0xB8401024);
        v.Encodes(v.enc.Stur(x0, x1, -8), 0xF81F8020);
        v.Encodes(v.enc.Sturb(w0, x1, 1), 0x38001020);
        v.Encodes(v.enc.Sturh(w0, x1, -256), 0x78100020);
        v.Encodes(v.enc.Ldursb(A64::Xn(2), x1, 255), 0x388FF022);
        v.Encodes(v.enc.Ldursh(A64::Xn(2), x1, -1), 0x789FF022);
        v.Encodes(v.enc.Ldursw(A64::Xn(2), x1, -17), 0xB89EF022);
        // The SIMD widths reach the same 9 bits, unscaled at every one of them.
        v.Encodes(v.enc.Ldur(A64::Qn(5), x1, 255), 0x3CCFF025);
    }

    SUBCASE("pre-index and post-index write the base back") {
        // ARM spells these two modes LDR and STR: they differ from LDUR and
        // STUR only in the two bits below the immediate.
        v.Encodes(v.enc.Ldur(x0, x1, 255, A64IndexMode::PostIndex), 0xF84FF420);
        v.Encodes(v.enc.Ldur(x0, x1, 255, A64IndexMode::PreIndex), 0xF84FFC20);
        v.Encodes(v.enc.Ldur(x0, x1, -256, A64IndexMode::PostIndex), 0xF8500420);
        v.Encodes(v.enc.Ldur(x0, A64::Sp, 255, A64IndexMode::PreIndex), 0xF84FFFE0);
        v.Encodes(v.enc.Stur(A64::Qn(5), A64::Sp, -256, A64IndexMode::PreIndex), 0x3C900FE5);
        v.Encodes(v.enc.Stur(A64::Qn(5), x1, 255, A64IndexMode::PostIndex), 0x3C8FF425);
        v.Encodes(v.enc.Ldur(A64::Bn(1), x1, -256, A64IndexMode::PostIndex), 0x3C500421);
        v.Encodes(v.enc.Ldur(A64::Sn(3), A64::Sp, 255, A64IndexMode::PreIndex), 0xBC4FFFE3);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.Ldur(x0, x1, 256), A64Status::OutOfRange);
        v.Refuses(v.enc.Ldur(x0, x1, -257), A64Status::OutOfRange);
        v.Refuses(v.enc.Stur(x0, x1, 256, A64IndexMode::PreIndex), A64Status::OutOfRange);
        v.Refuses(v.enc.Ldur(x0, A64::Xzr), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ldur(A64::Sp, x1), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ldurb(x0, x1), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ldursw(w0, x1), A64Status::InvalidRegister);
    }
}

// Loads and Stores: Load/store register (register offset).
TEST_CASE("AArch64 encodes register-offset loads and stores") {
    A64Vectors v;
    const A64Reg x0 = A64::Xn(0);
    const A64Reg x1 = A64::Xn(1);
    const A64Reg x7 = A64::Xn(7);
    const A64Reg w7 = A64::Wn(7);

    SUBCASE("every option, with and without the scale bit") {
        v.Encodes(v.enc.LdrReg(x0, x1, w7, A64ExtendKind::Uxtw), 0xF8674820);
        v.Encodes(v.enc.LdrReg(x0, x1, w7, A64ExtendKind::Uxtw, 3), 0xF8675820);
        // UXTX is the encoding the assembly syntax spells LSL.
        v.Encodes(v.enc.LdrReg(x0, x1, x7), 0xF8676820);
        v.Encodes(v.enc.LdrReg(x0, x1, x7, A64ExtendKind::Uxtx, 3), 0xF8677820);
        v.Encodes(v.enc.LdrReg(x0, x1, w7, A64ExtendKind::Sxtw), 0xF867C820);
        v.Encodes(v.enc.LdrReg(x0, x1, w7, A64ExtendKind::Sxtw, 3), 0xF867D820);
        v.Encodes(v.enc.LdrReg(x0, x1, x7, A64ExtendKind::Sxtx), 0xF867E820);
        v.Encodes(v.enc.LdrReg(x0, x1, x7, A64ExtendKind::Sxtx, 3), 0xF867F820);
        v.Encodes(v.enc.LdrReg(A64::Wn(1), x1, x7, A64ExtendKind::Uxtx, 2), 0xB8677821);
        v.Encodes(v.enc.StrReg(x0, A64::Sp, x7, A64ExtendKind::Uxtx, 3), 0xF8277BE0);
    }

    SUBCASE("the scale follows the access width") {
        v.Encodes(v.enc.LdrReg(A64::Hn(3), x1, x7, A64ExtendKind::Uxtx, 1), 0x7C677823);
        v.Encodes(v.enc.LdrReg(A64::Qn(6), x1, x7, A64ExtendKind::Uxtx, 4), 0x3CE77826);
        v.Encodes(v.enc.LdrReg(A64::Qn(6), x1, x7), 0x3CE76826);
        v.Encodes(v.enc.LdrhReg(A64::Wn(0), x1, w7, A64ExtendKind::Uxtw, 1), 0x78675820);
        v.Encodes(v.enc.LdrswReg(x0, x1, x7, A64ExtendKind::Uxtx, 2), 0xB8A77820);
        v.Encodes(v.enc.LdrsbReg(x0, x1, w7, A64ExtendKind::Sxtw), 0x38A7C820);
        // A byte access has nothing to scale, so it only ever writes S as zero.
        v.Encodes(v.enc.LdrbReg(A64::Wn(0), x1, x7), 0x38676820);
        v.Encodes(v.enc.StrbReg(A64::Wn(0), x1, w7, A64ExtendKind::Uxtw), 0x38274820);
    }

    SUBCASE("refusals") {
        // Only the word and doubleword extensions index memory.
        v.Refuses(v.enc.LdrReg(x0, x1, w7, A64ExtendKind::Uxtb), A64Status::InvalidImmediate);
        v.Refuses(v.enc.LdrReg(x0, x1, w7, A64ExtendKind::Sxth), A64Status::InvalidImmediate);
        // The extension names the width of the index.
        v.Refuses(v.enc.LdrReg(x0, x1, x7, A64ExtendKind::Uxtw), A64Status::InvalidRegister);
        v.Refuses(v.enc.LdrReg(x0, x1, w7, A64ExtendKind::Uxtx), A64Status::InvalidRegister);
        // The shift is a scale bit, so it is either absent or exactly the log2
        // of the access width.
        v.Refuses(v.enc.LdrReg(x0, x1, x7, A64ExtendKind::Uxtx, 2), A64Status::InvalidImmediate);
        v.Refuses(v.enc.LdrbReg(A64::Wn(0), x1, x7, A64ExtendKind::Uxtx, 1), A64Status::InvalidImmediate);
        v.Refuses(v.enc.LdrReg(x0, A64::Xzr, x7), A64Status::InvalidRegister);
        v.Refuses(v.enc.LdrReg(x0, x1, A64::Sp), A64Status::InvalidRegister);
    }
}

// Loads and Stores: Load/store register pair, in all three modes.
TEST_CASE("AArch64 encodes load and store pair") {
    A64Vectors v;
    const A64Reg x0 = A64::Xn(0);
    const A64Reg x1 = A64::Xn(1);

    SUBCASE("general purpose, scaled by one register") {
        v.Encodes(v.enc.Ldp(A64::Fp, A64::Lr, x1), 0xA940783D);
        v.Encodes(v.enc.Ldp(A64::Fp, A64::Lr, x1, 8), 0xA940F83D);
        v.Encodes(v.enc.Ldp(A64::Fp, A64::Lr, x1, 504), 0xA95FF83D);
        v.Encodes(v.enc.Ldp(A64::Fp, A64::Lr, x1, -512), 0xA960783D);
        v.Encodes(v.enc.Stp(A64::Fp, A64::Lr, x1, -16), 0xA93F783D);
        v.Encodes(v.enc.Ldp(x0, A64::Xzr, A64::Sp, -16), 0xA97F7FE0);
        v.Encodes(v.enc.Ldp(A64::Wn(3), A64::Wn(4), x1, 4), 0x29409023);
        v.Encodes(v.enc.Stp(A64::Wn(3), A64::Wn(4), x1, -256), 0x29201023);
        v.Encodes(v.enc.Ldp(A64::Wn(3), A64::Wn(4), x1, 252), 0x295F9023);
    }

    SUBCASE("the frame chain a prologue and epilogue move") {
        v.Encodes(v.enc.Stp(A64::Fp, A64::Lr, A64::Sp, -16, A64IndexMode::PreIndex), 0xA9BF7BFD);
        v.Encodes(v.enc.Ldp(A64::Fp, A64::Lr, A64::Sp, 16, A64IndexMode::PostIndex), 0xA8C17BFD);
        v.Encodes(v.enc.Stp(A64::Fp, A64::Lr, A64::Sp, -512, A64IndexMode::PreIndex), 0xA9A07BFD);
        v.Encodes(v.enc.Ldp(A64::Fp, A64::Lr, A64::Sp, 504, A64IndexMode::PostIndex), 0xA8DFFBFD);
    }

    SUBCASE("SIMD") {
        v.Encodes(v.enc.Ldp(A64::Sn(0), A64::Sn(1), x1, 4), 0x2D408420);
        v.Encodes(v.enc.Stp(A64::Sn(0), A64::Sn(1), x1, -256), 0x2D200420);
        v.Encodes(v.enc.Ldp(A64::Dn(8), A64::Dn(9), x1, 8), 0x6D40A428);
        v.Encodes(v.enc.Stp(A64::Dn(8), A64::Dn(9), x1, -512, A64IndexMode::PreIndex), 0x6DA02428);
        v.Encodes(v.enc.Ldp(A64::Qn(2), A64::Qn(3), x1, -1024), 0xAD600C22);
        v.Encodes(v.enc.Stp(A64::Qn(2), A64::Qn(3), x1, -32, A64IndexMode::PostIndex), 0xACBF0C22);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.Ldp(x0, x1, A64::Xn(2), 4), A64Status::Unaligned);
        v.Refuses(v.enc.Ldp(x0, x1, A64::Xn(2), 512), A64Status::OutOfRange);
        v.Refuses(v.enc.Ldp(x0, x1, A64::Xn(2), -520), A64Status::OutOfRange);
        // The two registers are one width and one file.
        v.Refuses(v.enc.Ldp(x0, A64::Wn(1), A64::Xn(2)), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ldp(x0, A64::Dn(1), A64::Xn(2)), A64Status::InvalidRegister);
        // There is no byte or halfword pair, and neither reading of code 31 is
        // a transferable register.
        v.Refuses(v.enc.Ldp(A64::Bn(0), A64::Bn(1), A64::Xn(2)), A64Status::InvalidRegister);
        v.Refuses(v.enc.Ldp(A64::Hn(0), A64::Hn(1), A64::Xn(2)), A64Status::InvalidRegister);
        v.Refuses(v.enc.Stp(A64::Sp, x1, A64::Xn(2)), A64Status::InvalidRegister);
        v.Refuses(v.enc.Stp(x0, x1, A64::Xzr), A64Status::InvalidRegister);
    }
}

// Loads and Stores: Load register (literal).
TEST_CASE("AArch64 encodes LDR from a PC-relative literal") {
    A64Vectors v;

    v.Encodes(v.enc.LdrLiteral(A64::Xn(0), 0), 0x58000000);
    v.Encodes(v.enc.LdrLiteral(A64::Xn(0), 4), 0x58000020);
    v.Encodes(v.enc.LdrLiteral(A64::Xn(0), -4), 0x58FFFFE0);
    v.Encodes(v.enc.LdrLiteral(A64::Wn(3), 4), 0x18000023);
    v.Encodes(v.enc.LdrLiteral(A64::Sn(5), 0), 0x1C000005);
    v.Encodes(v.enc.LdrLiteral(A64::Dn(7), 0), 0x5C000007);
    v.Encodes(v.enc.LdrLiteral(A64::Qn(9), 0), 0x9C000009);
    // The boundaries of the 19-bit field, counted in instructions.
    v.Encodes(v.enc.LdrLiteral(A64::Xn(0), 1048572), 0x587FFFE0);
    v.Encodes(v.enc.LdrLiteral(A64::Xn(0), -1048576), 0x58800000);

    v.Refuses(v.enc.LdrLiteral(A64::Xn(0), 2), A64Status::Unaligned);
    v.Refuses(v.enc.LdrLiteral(A64::Xn(0), 1048576), A64Status::OutOfRange);
    v.Refuses(v.enc.LdrLiteral(A64::Xn(0), -1048580), A64Status::OutOfRange);
    v.Refuses(v.enc.LdrLiteral(A64::Bn(0), 0), A64Status::InvalidRegister);
    v.Refuses(v.enc.LdrLiteral(A64::Hn(0), 0), A64Status::InvalidRegister);
    v.Refuses(v.enc.LdrLiteral(A64::Sp, 0), A64Status::InvalidRegister);
}

// Branches, Exception Generating and System instructions: Unconditional branch
// (immediate) and Conditional branch (immediate).
TEST_CASE("AArch64 encodes immediate branches") {
    A64Vectors v;

    SUBCASE("B and BL over the 26-bit field") {
        v.Encodes(v.enc.B(0), 0x14000000);
        v.Encodes(v.enc.B(4), 0x14000001);
        v.Encodes(v.enc.B(-4), 0x17FFFFFF);
        v.Encodes(v.enc.B(1024), 0x14000100);
        v.Encodes(v.enc.B(-1024), 0x17FFFF00);
        v.Encodes(v.enc.Bl(0), 0x94000000);
        v.Encodes(v.enc.Bl(4), 0x94000001);
        v.Encodes(v.enc.Bl(-4), 0x97FFFFFF);
        // +/-128 MiB, the whole reach of a call needing no veneer.
        v.Encodes(v.enc.B(134217724), 0x15FFFFFF);
        v.Encodes(v.enc.B(-134217728), 0x16000000);
        v.Encodes(v.enc.Bl(134217724), 0x95FFFFFF);
        v.Encodes(v.enc.Bl(-134217728), 0x96000000);
    }

    SUBCASE("B.cond at every condition") {
        v.Encodes(v.enc.BCond(A64Condition::Eq, 8), 0x54000040);
        v.Encodes(v.enc.BCond(A64Condition::Ne, 8), 0x54000041);
        v.Encodes(v.enc.BCond(A64Condition::Cs, 8), 0x54000042);
        v.Encodes(v.enc.BCond(A64Condition::Cc, 8), 0x54000043);
        v.Encodes(v.enc.BCond(A64Condition::Mi, 8), 0x54000044);
        v.Encodes(v.enc.BCond(A64Condition::Pl, 8), 0x54000045);
        v.Encodes(v.enc.BCond(A64Condition::Vs, 8), 0x54000046);
        v.Encodes(v.enc.BCond(A64Condition::Vc, 8), 0x54000047);
        v.Encodes(v.enc.BCond(A64Condition::Hi, 8), 0x54000048);
        v.Encodes(v.enc.BCond(A64Condition::Ls, 8), 0x54000049);
        v.Encodes(v.enc.BCond(A64Condition::Ge, 8), 0x5400004A);
        v.Encodes(v.enc.BCond(A64Condition::Lt, 8), 0x5400004B);
        v.Encodes(v.enc.BCond(A64Condition::Gt, 8), 0x5400004C);
        v.Encodes(v.enc.BCond(A64Condition::Le, 8), 0x5400004D);
        // Neither of the unconditional codes is being inverted here, so both
        // encode as written rather than being refused as they are in CSET.
        v.Encodes(v.enc.BCond(A64Condition::Al, 8), 0x5400004E);
        v.Encodes(v.enc.BCond(A64Condition::Nv, 8), 0x5400004F);
        // The boundaries of the 19-bit field: +/-1 MiB.
        v.Encodes(v.enc.BCond(A64Condition::Eq, 1048572), 0x547FFFE0);
        v.Encodes(v.enc.BCond(A64Condition::Eq, -1048576), 0x54800000);
    }

    SUBCASE("refusals") {
        // An offset that names no instruction is not rounded to one, and one
        // past the end of the field does not wrap into a branch elsewhere.
        v.Refuses(v.enc.B(2), A64Status::Unaligned);
        v.Refuses(v.enc.Bl(-1), A64Status::Unaligned);
        v.Refuses(v.enc.BCond(A64Condition::Eq, 6), A64Status::Unaligned);
        v.Refuses(v.enc.B(134217728), A64Status::OutOfRange);
        v.Refuses(v.enc.B(-134217732), A64Status::OutOfRange);
        v.Refuses(v.enc.Bl(134217728), A64Status::OutOfRange);
        v.Refuses(v.enc.BCond(A64Condition::Ne, 1048576), A64Status::OutOfRange);
        v.Refuses(v.enc.BCond(A64Condition::Ne, -1048580), A64Status::OutOfRange);
    }
}

// Branches, Exception Generating and System instructions: Compare and branch
// (immediate) and Test and branch (immediate).
TEST_CASE("AArch64 encodes compare-and-branch and test-and-branch") {
    A64Vectors v;

    SUBCASE("CBZ and CBNZ") {
        v.Encodes(v.enc.Cbz(A64::Xn(0), 16), 0xB4000080);
        v.Encodes(v.enc.Cbz(A64::Xn(19), 16), 0xB4000093);
        v.Encodes(v.enc.Cbnz(A64::Wn(0), 16), 0x35000080);
        v.Encodes(v.enc.Cbnz(A64::Wn(30), 16), 0x3500009E);
        v.Encodes(v.enc.Cbz(A64::Wzr, 16), 0x3400009F);
        v.Encodes(v.enc.Cbz(A64::Xn(0), 1048572), 0xB47FFFE0);
        v.Encodes(v.enc.Cbnz(A64::Xn(0), -1048576), 0xB5800000);
    }

    SUBCASE("TBZ and TBNZ split the bit index across two fields") {
        v.Encodes(v.enc.Tbz(A64::Xn(3), 0, 12), 0x36000063);
        v.Encodes(v.enc.Tbz(A64::Xn(3), 1, 12), 0x36080063);
        v.Encodes(v.enc.Tbnz(A64::Xn(3), 0, 12), 0x37000063);
        // Bit 31 is the last one b40 holds alone; bit 32 is the first to set b5,
        // which sits where every other instruction keeps sf.
        v.Encodes(v.enc.Tbz(A64::Xn(3), 31, 12), 0x36F80063);
        v.Encodes(v.enc.Tbz(A64::Xn(3), 32, 12), 0xB6000063);
        v.Encodes(v.enc.Tbz(A64::Xn(3), 63, 12), 0xB6F80063);
        v.Encodes(v.enc.Tbnz(A64::Xn(3), 31, 12), 0x37F80063);
        v.Encodes(v.enc.Tbnz(A64::Xn(3), 32, 12), 0xB7000063);
        v.Encodes(v.enc.Tbnz(A64::Xn(3), 63, 12), 0xB7F80063);
        // A W register reaches only the low half, and encodes identically to
        // the X register it is a view of.
        v.Encodes(v.enc.Tbz(A64::Wn(5), 0, -12), 0x3607FFA5);
        v.Encodes(v.enc.Tbz(A64::Wn(5), 31, -12), 0x36FFFFA5);
        // The boundaries of the 14-bit field: +/-32 KiB.
        v.Encodes(v.enc.Tbz(A64::Xn(0), 0, 32764), 0x3603FFE0);
        v.Encodes(v.enc.Tbnz(A64::Xn(0), 63, -32768), 0xB7FC0000);
    }

    SUBCASE("refusals") {
        v.Refuses(v.enc.Cbz(A64::Xn(0), 2), A64Status::Unaligned);
        v.Refuses(v.enc.Cbz(A64::Xn(0), 1048576), A64Status::OutOfRange);
        v.Refuses(v.enc.Cbnz(A64::Xn(0), -1048580), A64Status::OutOfRange);
        v.Refuses(v.enc.Tbz(A64::Xn(0), 0, 2), A64Status::Unaligned);
        v.Refuses(v.enc.Tbz(A64::Xn(0), 0, 32768), A64Status::OutOfRange);
        v.Refuses(v.enc.Tbnz(A64::Xn(0), 0, -32772), A64Status::OutOfRange);
        // The bit index has to name a bit the register has.
        v.Refuses(v.enc.Tbz(A64::Wn(0), 32, 12), A64Status::InvalidImmediate);
        v.Refuses(v.enc.Tbz(A64::Xn(0), 64, 12), A64Status::InvalidImmediate);
        // Neither form compares the stack pointer or anything but a general
        // register.
        v.Refuses(v.enc.Cbz(A64::Sp, 16), A64Status::InvalidRegister);
        v.Refuses(v.enc.Cbz(A64::Dn(0), 16), A64Status::InvalidRegister);
        v.Refuses(v.enc.Tbz(A64::Wsp, 0, 12), A64Status::InvalidRegister);
        v.Refuses(v.enc.Tbnz(A64::Qn(0), 0, 12), A64Status::InvalidRegister);
    }
}

// Branches, Exception Generating and System instructions: Unconditional branch
// (register).
TEST_CASE("AArch64 encodes branches through a register") {
    A64Vectors v;

    v.Encodes(v.enc.Br(A64::Xn(0)), 0xD61F0000);
    v.Encodes(v.enc.Br(A64::Ip0), 0xD61F0200);
    v.Encodes(v.enc.Br(A64::Lr), 0xD61F03C0);
    v.Encodes(v.enc.Br(A64::Xzr), 0xD61F03E0);
    v.Encodes(v.enc.Blr(A64::Xn(0)), 0xD63F0000);
    v.Encodes(v.enc.Blr(A64::Ip0), 0xD63F0200);
    v.Encodes(v.enc.Blr(A64::Lr), 0xD63F03C0);
    v.Encodes(v.enc.Ret(A64::Xn(0)), 0xD65F0000);
    v.Encodes(v.enc.Ret(A64::Lr), 0xD65F03C0);
    // An epilogue returns through the link register, so that is what RET is
    // written without an operand.
    v.Encodes(v.enc.Ret(), 0xD65F03C0);

    // The target is an address, so it is 64-bit and never the stack pointer.
    v.Refuses(v.enc.Br(A64::Wn(0)), A64Status::InvalidRegister);
    v.Refuses(v.enc.Blr(A64::Sp), A64Status::InvalidRegister);
    v.Refuses(v.enc.Ret(A64::Dn(0)), A64Status::InvalidRegister);
}

// Branches, Exception Generating and System instructions: Exception generation.
TEST_CASE("AArch64 encodes exception generation") {
    A64Vectors v;

    v.Encodes(v.enc.Svc(0), 0xD4000001);
    v.Encodes(v.enc.Svc(1), 0xD4000021);
    // The syscall wrappers issue SVC #0; the rest of the field is there for a
    // debugger rather than for the kernel.
    v.Encodes(v.enc.Svc(0x1234), 0xD4024681);
    v.Encodes(v.enc.Svc(0xFFFF), 0xD41FFFE1);
    v.Encodes(v.enc.Brk(0), 0xD4200000);
    v.Encodes(v.enc.Brk(1), 0xD4200020);
    v.Encodes(v.enc.Brk(0x1234), 0xD4224680);
    v.Encodes(v.enc.Brk(0xFFFF), 0xD43FFFE0);
    v.Encodes(v.enc.Hlt(0), 0xD4400000);
    v.Encodes(v.enc.Hlt(0x1234), 0xD4424680);
    v.Encodes(v.enc.Hlt(0xFFFF), 0xD45FFFE0);

    // UDF is a hole in the instruction space rather than an instruction, so its
    // word is the immediate and nothing else — which is why a jump into zeroed
    // memory traps on the first word it reaches.
    v.Encodes(v.enc.Udf(0), 0x00000000);
    v.Encodes(v.enc.Udf(1), 0x00000001);
    v.Encodes(v.enc.Udf(0xFFFF), 0x0000FFFF);
}

// Branches, Exception Generating and System instructions: Hints and barriers.
TEST_CASE("AArch64 encodes hints and barriers") {
    A64Vectors v;

    SUBCASE("hints") {
        v.Encodes(v.enc.Nop(), 0xD503201F);
        v.Encodes(v.enc.Hint(0), 0xD503201F);
        v.Encodes(v.enc.Hint(1), 0xD503203F);
        v.Encodes(v.enc.Hint(3), 0xD503207F);
        // The immediate is CRm and op2 together, so it carries into the field
        // above at every multiple of eight.
        v.Encodes(v.enc.Hint(8), 0xD503211F);
        v.Encodes(v.enc.Hint(127), 0xD5032FFF);
        v.Refuses(v.enc.Hint(128), A64Status::InvalidImmediate);
    }

    SUBCASE("barriers at every named option") {
        v.Encodes(v.enc.Dmb(), 0xD5033FBF);
        v.Encodes(v.enc.Dmb(A64Barrier::Oshld), 0xD50331BF);
        v.Encodes(v.enc.Dmb(A64Barrier::Oshst), 0xD50332BF);
        v.Encodes(v.enc.Dmb(A64Barrier::Osh), 0xD50333BF);
        v.Encodes(v.enc.Dmb(A64Barrier::Nshld), 0xD50335BF);
        v.Encodes(v.enc.Dmb(A64Barrier::Nshst), 0xD50336BF);
        v.Encodes(v.enc.Dmb(A64Barrier::Nsh), 0xD50337BF);
        v.Encodes(v.enc.Dmb(A64Barrier::Ishld), 0xD50339BF);
        v.Encodes(v.enc.Dmb(A64Barrier::Ishst), 0xD5033ABF);
        v.Encodes(v.enc.Dmb(A64Barrier::Ish), 0xD5033BBF);
        v.Encodes(v.enc.Dmb(A64Barrier::Ld), 0xD5033DBF);
        v.Encodes(v.enc.Dmb(A64Barrier::St), 0xD5033EBF);
        v.Encodes(v.enc.Dmb(A64Barrier::Sy), 0xD5033FBF);

        // DSB differs from DMB in op2 alone, so the option field encodes the
        // same way in both.
        v.Encodes(v.enc.Dsb(), 0xD5033F9F);
        v.Encodes(v.enc.Dsb(A64Barrier::Osh), 0xD503339F);
        v.Encodes(v.enc.Dsb(A64Barrier::Ish), 0xD5033B9F);
        v.Encodes(v.enc.Dsb(A64Barrier::Ld), 0xD5033D9F);
        v.Encodes(v.enc.Dsb(A64Barrier::Sy), 0xD5033F9F);

        // ISB has one meaningful option, and it is the default.
        v.Encodes(v.enc.Isb(), 0xD5033FDF);
        v.Encodes(v.enc.Isb(A64Barrier::Sy), 0xD5033FDF);
    }
}

// Branches, Exception Generating and System instructions: System register move.
TEST_CASE("AArch64 encodes moves to and from a system register") {
    A64Vectors v;

    v.Encodes(v.enc.Mrs(A64::Xn(0), A64::Nzcv), 0xD53B4200);
    v.Encodes(v.enc.Mrs(A64::Xn(1), A64::Nzcv), 0xD53B4201);
    v.Encodes(v.enc.Mrs(A64::Lr, A64::Nzcv), 0xD53B421E);
    v.Encodes(v.enc.Mrs(A64::Xzr, A64::Nzcv), 0xD53B421F);
    v.Encodes(v.enc.Msr(A64::Nzcv, A64::Xn(0)), 0xD51B4200);
    v.Encodes(v.enc.Msr(A64::Nzcv, A64::Lr), 0xD51B421E);

    v.Encodes(v.enc.Mrs(A64::Xn(0), A64::TpidrEl0), 0xD53BD040);
    v.Encodes(v.enc.Mrs(A64::Lr, A64::TpidrEl0), 0xD53BD05E);
    v.Encodes(v.enc.Msr(A64::TpidrEl0, A64::Xn(0)), 0xD51BD040);

    // Any register the 15-bit encoding names is reachable, not only the two
    // that have a constant.
    v.Encodes(v.enc.Mrs(A64::Xn(1), A64::SysReg(3, 3, 4, 4, 0)), 0xD53B4401); // FPCR
    v.Encodes(v.enc.Mrs(A64::Xn(1), A64::SysReg(3, 3, 4, 4, 1)), 0xD53B4421); // FPSR
    v.Encodes(v.enc.Msr(A64::SysReg(3, 3, 4, 4, 0), A64::Xn(2)), 0xD51B4402); // FPCR
    v.Encodes(v.enc.Mrs(A64::Xn(1), A64::SysReg(3, 0, 0, 0, 0)), 0xD5380001); // MIDR_EL1
    v.Encodes(v.enc.Mrs(A64::Xn(1), A64::SysReg(3, 3, 0, 0, 1)), 0xD53B0021); // CTR_EL0

    // The transferred register is 64-bit whatever the width of the system
    // register, and code 31 is the zero register there.
    v.Refuses(v.enc.Mrs(A64::Wn(0), A64::Nzcv), A64Status::InvalidRegister);
    v.Refuses(v.enc.Mrs(A64::Sp, A64::Nzcv), A64Status::InvalidRegister);
    v.Refuses(v.enc.Msr(A64::Nzcv, A64::Wn(0)), A64Status::InvalidRegister);
    v.Refuses(v.enc.Msr(A64::Nzcv, A64::Dn(0)), A64Status::InvalidRegister);
}

TEST_CASE("AArch64 encoder statuses have names for diagnostics") {
    CHECK(A64StatusName(A64Status::Ok) == "ok");
    CHECK(A64StatusName(A64Status::InvalidRegister) == "invalid register");
    CHECK(A64StatusName(A64Status::InvalidImmediate) == "invalid immediate");
    CHECK(A64StatusName(A64Status::OutOfRange) == "offset out of range");
    CHECK(A64StatusName(A64Status::Unaligned) == "misaligned offset");
}
