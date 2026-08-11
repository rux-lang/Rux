#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"

#include <cstdint>
#include <doctest.h>
#include <vector>

using namespace Rux;

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

TEST_CASE("AArch64 encoder statuses have names for diagnostics") {
    CHECK(A64StatusName(A64Status::Ok) == "ok");
    CHECK(A64StatusName(A64Status::InvalidRegister) == "invalid register");
    CHECK(A64StatusName(A64Status::InvalidImmediate) == "invalid immediate");
    CHECK(A64StatusName(A64Status::OutOfRange) == "offset out of range");
    CHECK(A64StatusName(A64Status::Unaligned) == "misaligned offset");
}
