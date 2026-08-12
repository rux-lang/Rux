// AArch64 encoder tests: the register model, the immediate helpers, and the
// harness the vector table in AArch64EncoderVectors.inc runs on.
//
// The vectors themselves live in that file rather than this one, since they are
// a table of data — a form, its operands and the word they must produce —
// while everything here is the machinery that reads it. The table is included
// at the bottom, once the fixture and the macros exist.

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"

#include <cstdint>
#include <doctest.h>
// The INFO() below streams a std::string_view, and the MSVC standard library
// declares that operator<< in <string_view> without defining std::ostream, so
// the test has to bring the definition in itself.
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

// The register factories, the named registers and the condition helpers read as
// an assembler writes them — Xn(3), Sp, Lr, Nzcv — which is what keeps a line
// of the vector table to the width of the instruction it stands for.
using namespace Rux::A64;

namespace {
// An instruction word as the table spells it. doctest reports an integer in
// decimal, which says nothing at a glance against a table of hexadecimal
// words, so a word is compared by its spelling rather than by its value.
[[nodiscard]] std::string HexWord(const std::uint32_t word) {
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        text += "0123456789ABCDEF"[(static_cast<unsigned>(word) >> shift) & 0xFU];
    }
    return text;
}

// The buffer one vector encodes into, and the checks a vector is made of.
//
// Every vector starts from an empty buffer and empties it again, so a refusal
// shows up as an empty buffer rather than as a leftover word from the vector
// before it. `form` is the encoder call as the table writes it, which is what
// names a failing vector in the report.
struct A64Vectors {
    std::vector<std::uint8_t> code;
    A64Enc enc{code};

    // One encoder call and the single word it must produce.
    void Encodes(const std::string_view form, const A64Status status, const std::uint32_t expected) {
        const std::string_view statusName = A64StatusName(status);
        INFO(form, " -> ", statusName);
        CHECK(status == A64Status::Ok);
        REQUIRE(code.size() == A64Enc::InstrSize);
        CHECK(HexWord(enc.WordAt(0)) == HexWord(expected));
        code.clear();
    }

    // A composite sequence and every word it must emit, in order and with
    // nothing after them.
    void EncodesAll(const std::string_view form, const A64Status status, const std::vector<std::uint32_t> &expected) {
        const std::string_view statusName = A64StatusName(status);
        INFO(form, " -> ", statusName);
        CHECK(status == A64Status::Ok);
        Emitted(expected);
    }

    // An operand combination with no encoding, which must be reported rather
    // than truncated into some other instruction — and must leave the buffer
    // as it found it, so a caller can try a short form and fall back.
    void Refuses(const std::string_view form, const A64Status status, const A64Status expected) {
        const std::string_view statusName = A64StatusName(status);
        INFO(form, " -> ", statusName);
        CHECK(status == expected);
        CHECK(code.empty());
    }

    // Every word emitted so far, for the sequences that report an addressing
    // decision alongside what they emitted.
    void Emitted(const std::vector<std::uint32_t> &expected) {
        REQUIRE(code.size() == expected.size() * A64Enc::InstrSize);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(HexWord(enc.WordAt(static_cast<std::uint32_t>(i) * A64Enc::InstrSize)) == HexWord(expected[i]));
        }
        code.clear();
    }
};
} // namespace

// The vector table's vocabulary. The expected word comes first so that it lines
// up down the left of the table, and the encoder call is the trailing argument
// so that its own commas need no protecting.
#define A64_UNPARENTHESIZE(...) __VA_ARGS__
#define A64_VECTORS(name) TEST_CASE_FIXTURE(A64Vectors, name)
#define A64_GROUP(name) SUBCASE(name)
#define A64_ENCODES(word, ...) Encodes(#__VA_ARGS__, enc.__VA_ARGS__, (word))
#define A64_EMITS(words, ...) EncodesAll(#__VA_ARGS__, enc.__VA_ARGS__, {A64_UNPARENTHESIZE words})
#define A64_REFUSES(status, ...) Refuses(#__VA_ARGS__, enc.__VA_ARGS__, A64Status::status)

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
    CHECK(Xn(0).code == 0);
    CHECK(Xn(30).code == 30);
    CHECK(Xn(0).bits == 64);
    CHECK(Wn(7).bits == 32);
    CHECK(Xn(0).Sf() == 1);
    CHECK(Wn(0).Sf() == 0);
    CHECK(Gpr(3, 64) == Xn(3));
    CHECK(Gpr(3, 32) == Wn(3));

    // Code 31 is XZR everywhere except where an encoder accepts SP.
    CHECK(Xzr.code == 31);
    CHECK(Sp.code == 31);
    CHECK(Xzr != Sp);
    CHECK(Xzr.IsZeroReg());
    CHECK_FALSE(Xzr.IsStackPointer());
    CHECK(Sp.IsStackPointer());
    CHECK_FALSE(Sp.IsZeroReg());
    CHECK(Wzr.IsZeroReg());
    CHECK(Wsp.IsStackPointer());
    CHECK(Wsp.bits == 32);
    CHECK(Xn(31) == Xzr);

    // Reserved registers.
    CHECK(Ip0 == Xn(16));
    CHECK(Ip1 == Xn(17));
    CHECK(PlatformReg == Xn(18));
    CHECK(Fp == Xn(29));
    CHECK(Lr == Xn(30));
}

TEST_CASE("AArch64 register model gives every vector register five width views") {
    CHECK(Bn(0).bits == 8);
    CHECK(Hn(0).bits == 16);
    CHECK(Sn(0).bits == 32);
    CHECK(Dn(0).bits == 64);
    CHECK(Qn(0).bits == 128);
    CHECK(Vn(31, 64) == Dn(31));
    CHECK(Dn(5).code == 5);
    CHECK(Dn(5).IsVector());
    CHECK_FALSE(Dn(5).IsGeneral());
    CHECK_FALSE(Xn(5).IsVector());

    // A vector register never reads as the zero register or the stack pointer,
    // whatever its code.
    CHECK_FALSE(Qn(31).IsZeroReg());
    CHECK_FALSE(Qn(31).IsStackPointer());
    CHECK(Dn(0) != Xn(0));
}

TEST_CASE("AArch64 condition codes invert in pairs") {
    CHECK(InvertCondition(A64Condition::Eq) == A64Condition::Ne);
    CHECK(InvertCondition(A64Condition::Ne) == A64Condition::Eq);
    CHECK(InvertCondition(A64Condition::Hi) == A64Condition::Ls);
    CHECK(InvertCondition(A64Condition::Ge) == A64Condition::Lt);
    CHECK(InvertCondition(A64Condition::Gt) == A64Condition::Le);
    CHECK(InvertCondition(A64Condition::Cs) == A64Condition::Cc);
    CHECK(InvertCondition(A64Condition::Al) == A64Condition::Nv);
    CHECK(Hs == A64Condition::Cs);
    CHECK(Lo == A64Condition::Cc);

    // Inversion is its own inverse for all sixteen encodings.
    for (std::uint8_t i = 0; i < 16; ++i) {
        const auto cond = static_cast<A64Condition>(i);
        CHECK(InvertCondition(InvertCondition(cond)) == cond);
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
    CHECK(SysReg(3, 3, 4, 2, 0) == 0x5A10);
    CHECK(Nzcv == 0x5A10);
    CHECK(SysReg(3, 3, 13, 0, 2) == 0x5E82);
    CHECK(TpidrEl0 == 0x5E82);

    // MIDR_EL1 is the one register of interest whose op1 is zero, and FPCR the
    // one whose CRm is not; both exercise a field the two named ones leave at a
    // constant.
    CHECK(SysReg(3, 0, 0, 0, 0) == 0x4000);
    CHECK(SysReg(3, 3, 4, 4, 0) == 0x5A20);

    // op0 is 2 or 3 for everything these instructions reach, so only its low
    // bit travels; every other field is masked to its own width.
    CHECK(SysReg(2, 0, 0, 0, 0) == 0);
    CHECK(SysReg(3, 7, 15, 15, 7) == 0x7FFF);
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

// The 256 values of FMOV (scalar, immediate), from the manual's VFPExpandImm:
// a sign, a three-bit exponent and a four-bit fraction naming
// +/-(16 + n) / 16 * 2^e.
TEST_CASE("AArch64 floating-point immediates cover eight binades and nothing else") {
    auto encode = [](const double value) {
        const auto imm = TryEncodeFpImm8(value);
        REQUIRE(imm.has_value());
        return *imm;
    };

    // The two ends of the field, and the sign bit that separates its halves.
    // The exponent is the high three bits, so the field runs from the largest
    // magnitude down to the smallest rather than the other way about.
    CHECK(encode(2.0) == 0x00);
    CHECK(encode(1.9375) == 0x7F);
    CHECK(encode(-2.0) == 0x80);
    CHECK(encode(-1.9375) == 0xFF);
    CHECK(encode(1.0) == 0x70);
    CHECK(encode(0.125) == 0x40);
    CHECK(encode(31.0) == 0x3F);
    CHECK(encode(-31.0) == 0xBF);

    // Zero carries no leading one, so it is not among them; nor is anything
    // outside the eight binades, a fraction needing a fifth bit, or a value
    // with no exponent at all.
    CHECK_FALSE(TryEncodeFpImm8(0.0).has_value());
    CHECK_FALSE(TryEncodeFpImm8(-0.0).has_value());
    CHECK_FALSE(TryEncodeFpImm8(32.0).has_value());
    CHECK_FALSE(TryEncodeFpImm8(0.0625).has_value());
    CHECK_FALSE(TryEncodeFpImm8(1.03125).has_value());
    CHECK_FALSE(TryEncodeFpImm8(3.14159).has_value());
}

TEST_CASE("AArch64 encoder statuses have names for diagnostics") {
    CHECK(A64StatusName(A64Status::Ok) == "ok");
    CHECK(A64StatusName(A64Status::InvalidRegister) == "invalid register");
    CHECK(A64StatusName(A64Status::InvalidImmediate) == "invalid immediate");
    CHECK(A64StatusName(A64Status::OutOfRange) == "offset out of range");
    CHECK(A64StatusName(A64Status::Unaligned) == "misaligned offset");
}

// Every encoder of Tasks 6 through 11, one line per vector.
#include "AArch64EncoderVectors.inc"
