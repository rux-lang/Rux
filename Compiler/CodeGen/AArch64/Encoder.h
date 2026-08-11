#pragma once

// AArch64 instruction encoder: structured operands in, instruction words out.
//
// The counterpart of CodeGen/X86_64/Encoder.h. It is pure encoding — it knows
// nothing of LIR, frames or symbols — but where the variable-length x86-64
// encoder exposes one method per fixed operand shape, AArch64's fixed-width
// encoding lets each instruction family take its operands as parameters.
//
// An operand combination the encoding cannot express is a value, never an
// assertion: an encoder reports A64Status and emits nothing, and the immediate
// helpers below return an empty optional.

#include "CodeGen/AArch64/Registers.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Rux {
// Why an encoder refused to encode an instruction.
enum class A64Status : std::uint8_t {
    Ok,
    InvalidRegister,  // wrong register file or width, or SP where only XZR is allowed
    InvalidImmediate, // a value with no encoding in this instruction's immediate field
    OutOfRange,       // a branch or memory offset outside the reach of its field
    Unaligned,        // an offset the instruction's implicit scaling cannot express
};

// Short phrase naming a status, for diagnostics and test failures.
[[nodiscard]] std::string_view A64StatusName(A64Status status);

// A logical (bitmask) immediate, as the `N:immr:imms` fields of the
// AND / ORR / EOR / ANDS immediate forms encode it.
struct A64BitmaskImm {
    std::uint8_t n = 0;
    std::uint8_t immr = 0;
    std::uint8_t imms = 0;
};

// A 12-bit arithmetic immediate, optionally shifted left by 12.
struct A64ArithImm {
    std::uint16_t imm12 = 0;
    bool shift12 = false;
};

// A halfword immediate for the MOVZ / MOVN / MOVK group.
struct A64MovwImm {
    std::uint16_t imm16 = 0;
    std::uint8_t hw = 0;   // halfword index: the immediate is shifted left by hw * 16
    bool inverted = false; // the value needs MOVN of ~value, not MOVZ of value
};

// Encode `value` as a logical immediate for a 64- or 32-bit instruction.
//
// A logical immediate is a run of `imms + 1` one bits inside a 2, 4, 8, 16, 32
// or 64-bit element, rotated right by `immr` and replicated across the
// register. All-zeros and all-ones have no such encoding and are rejected, as
// is a value with bits above the instruction's width.
[[nodiscard]] std::optional<A64BitmaskImm> TryEncodeBitmaskImm(std::uint64_t value, bool is64);

// Encode `value` as an ADD / SUB / CMP immediate: either `imm12` or
// `imm12 << 12`, with nothing in between.
[[nodiscard]] std::optional<A64ArithImm> TryEncodeArithImm12(std::uint64_t value);

// Encode `value` as a single MOVZ or MOVN, choosing the halfword to keep and
// inverting when that is what makes the rest of the register vanish. A value
// needing more than one halfword is rejected: loading it takes a MOVZ followed
// by a chain of MOVK, which is a sequence rather than an encoding.
[[nodiscard]] std::optional<A64MovwImm> TryEncodeMovwImm(std::uint64_t value, bool is64);

// Instruction-stream writer.
//
// Owns nothing: words are appended to the caller's buffer, so the code
// generator can interleave encoding with its own bookkeeping, exactly as
// X64Enc does. Every AArch64 instruction is one little-endian 32-bit word, so
// a patch site is an instruction offset in all but name.
class A64Enc {
public:
    // Every instruction is this many bytes wide, with no exceptions.
    static constexpr std::uint32_t InstrSize = 4;

    explicit A64Enc(std::vector<std::uint8_t> &buf)
        : out(buf) {
    }

    // Bytes emitted so far, which is the offset the next word will land at.
    [[nodiscard]] std::uint32_t Size() const;

    // Append one instruction word.
    void Word(std::uint32_t word) const;

    // Read back the word at a byte offset.
    [[nodiscard]] std::uint32_t WordAt(std::uint32_t offset) const;

    // Replace the word at a byte offset.
    void PatchWord(std::uint32_t offset, std::uint32_t word) const;

    // Replace `width` bits starting at bit `lsb` of the word at a byte offset,
    // leaving the rest of the instruction alone. This is how a split immediate
    // is filled in once a branch target or a symbol address is known; bits of
    // `value` above `width` are discarded.
    void PatchField(std::uint32_t offset, unsigned lsb, unsigned width, std::uint32_t value) const;

    // Data processing — immediate.
    //
    // Every encoder below reports A64Status and emits nothing at all when it
    // refuses, so a caller can try the short form and fall back to a longer
    // sequence without rewinding the buffer.
    //
    // The operand width comes from `rd`: an X register selects the 64-bit form
    // and a W register the 32-bit one, and every other general-purpose operand
    // must agree or the encoder reports InvalidRegister. Code 31 is read as
    // whichever register the field it lands in actually names, so A64::Sp is
    // refused where only XZR encodes and A64::Xzr where only SP does.

    // ADD / ADDS / SUB / SUBS (immediate). `imm` is the unshifted value and the
    // LSL #12 form is selected for it when it needs one, so the two shift
    // positions are an encoding detail rather than an operand. ADD and SUB read
    // and write SP; the flag-setting forms read it but write XZR, which is what
    // makes them the CMN and CMP aliases.
    [[nodiscard]] A64Status AddImm(A64Reg rd, A64Reg rn, std::uint64_t imm) const;
    [[nodiscard]] A64Status AddsImm(A64Reg rd, A64Reg rn, std::uint64_t imm) const;
    [[nodiscard]] A64Status SubImm(A64Reg rd, A64Reg rn, std::uint64_t imm) const;
    [[nodiscard]] A64Status SubsImm(A64Reg rd, A64Reg rn, std::uint64_t imm) const;

    // AND / ORR / EOR / ANDS (bitmask immediate). `imm` is the value the
    // instruction applies, encoded through TryEncodeBitmaskImm. AND, ORR and
    // EOR write SP; ANDS writes XZR for its TST alias.
    [[nodiscard]] A64Status AndImm(A64Reg rd, A64Reg rn, std::uint64_t imm) const;
    [[nodiscard]] A64Status OrrImm(A64Reg rd, A64Reg rn, std::uint64_t imm) const;
    [[nodiscard]] A64Status EorImm(A64Reg rd, A64Reg rn, std::uint64_t imm) const;
    [[nodiscard]] A64Status AndsImm(A64Reg rd, A64Reg rn, std::uint64_t imm) const;

    // MOVZ / MOVN / MOVK. `shift` is a bit count and must name a halfword the
    // register has: 0, 16, 32 or 48 for an X register, 0 or 16 for a W one.
    // MOVZ writes the halfword and zeros the rest, MOVN writes the inverse of
    // the whole register, and MOVK leaves the other halfwords alone.
    [[nodiscard]] A64Status Movz(A64Reg rd, std::uint16_t imm16, unsigned shift = 0) const;
    [[nodiscard]] A64Status Movn(A64Reg rd, std::uint16_t imm16, unsigned shift = 0) const;
    [[nodiscard]] A64Status Movk(A64Reg rd, std::uint16_t imm16, unsigned shift = 0) const;

    // ADR and ADRP, whose 21-bit immediate is split across two fields.
    //
    // Both take a byte offset from the instruction: ADR reaches +/-1 MiB, and
    // ADRP reaches +/-4 GiB but counts whole 4 KiB pages, so its offset is the
    // distance between the page holding the instruction and the page holding
    // the target and must be a multiple of 4096. `rd` is always 64-bit.
    [[nodiscard]] A64Status Adr(A64Reg rd, std::int64_t offset) const;
    [[nodiscard]] A64Status Adrp(A64Reg rd, std::int64_t offset) const;

    // SBFM / UBFM / BFM, the three bitfield instructions every alias below is
    // built from. `immr` rotates the source right and `imms` names the top bit
    // of the field; both must be smaller than the register width.
    [[nodiscard]] A64Status Sbfm(A64Reg rd, A64Reg rn, unsigned immr, unsigned imms) const;
    [[nodiscard]] A64Status Ubfm(A64Reg rd, A64Reg rn, unsigned immr, unsigned imms) const;
    [[nodiscard]] A64Status Bfm(A64Reg rd, A64Reg rn, unsigned immr, unsigned imms) const;

    // Shifts by a constant. `shift` must be smaller than the register width;
    // the variable-register forms are LSLV, LSRV and ASRV, not these.
    [[nodiscard]] A64Status Lsl(A64Reg rd, A64Reg rn, unsigned shift) const;
    [[nodiscard]] A64Status Lsr(A64Reg rd, A64Reg rn, unsigned shift) const;
    [[nodiscard]] A64Status Asr(A64Reg rd, A64Reg rn, unsigned shift) const;

    // Field extraction and insertion. `width` is the number of bits moved and
    // must be at least one; `lsb` is where the field sits in the source for the
    // extracts and in the destination for BFI. UBFX and SBFX clear or sign
    // extend the rest of `rd`, while BFI and BFXIL leave it alone.
    [[nodiscard]] A64Status Ubfx(A64Reg rd, A64Reg rn, unsigned lsb, unsigned width) const;
    [[nodiscard]] A64Status Sbfx(A64Reg rd, A64Reg rn, unsigned lsb, unsigned width) const;
    [[nodiscard]] A64Status Bfi(A64Reg rd, A64Reg rn, unsigned lsb, unsigned width) const;
    [[nodiscard]] A64Status Bfxil(A64Reg rd, A64Reg rn, unsigned lsb, unsigned width) const;

    // Extensions of a narrow value held in a W register. `rn` is 32-bit in
    // every form, since the bits above the field are the ones being replaced,
    // and `rd` selects the width of the result. SXTW only ever widens, so its
    // `rd` is 64-bit; UXTB and UXTH have no 64-bit form at all, because the
    // 32-bit instruction already zeroes the upper half of the register.
    [[nodiscard]] A64Status Sxtb(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Sxth(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Sxtw(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Uxtb(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Uxth(A64Reg rd, A64Reg rn) const;

    // EXTR takes the `lsb` low bits of `rn` as the high part of the result and
    // the rest from the top of `rm`; ROR is the form that reads one register
    // twice.
    [[nodiscard]] A64Status Extr(A64Reg rd, A64Reg rn, A64Reg rm, unsigned lsb) const;
    [[nodiscard]] A64Status Ror(A64Reg rd, A64Reg rn, unsigned shift) const;

    // Data processing — register.
    //
    // The same width rule holds: `rd` selects the form and every other
    // general-purpose operand must match it, apart from the deliberately mixed
    // widths of the extended-register and widening-multiply forms. Only the
    // extended-register instructions reach the stack pointer; the shifted and
    // logical forms read code 31 as the zero register throughout, which is what
    // makes CMP, TST, NEG and MOV aliases of them.

    // ADD / ADDS / SUB / SUBS (shifted register). `amount` must be smaller than
    // the register width, and `shift` is LSL, LSR or ASR — ROR has no encoding
    // in the arithmetic forms.
    [[nodiscard]] A64Status Add(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Adds(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                 unsigned amount = 0) const;
    [[nodiscard]] A64Status Sub(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Subs(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                 unsigned amount = 0) const;

    // The shifted-register aliases: CMP and CMN discard the result, NEG and
    // NEGS subtract from the zero register.
    [[nodiscard]] A64Status Cmp(A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Cmn(A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Neg(A64Reg rd, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Negs(A64Reg rd, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                 unsigned amount = 0) const;

    // ADD / ADDS / SUB / SUBS (extended register). The extension names how much
    // of `rm` is read and whether the rest is copied or replicated, so `rm` is a
    // W register for every option but UXTX and SXTX; `amount` is a left shift of
    // 0 to 4 applied after the extension. These are the only arithmetic forms
    // that reach SP, which they read through `rn` and — outside the
    // flag-setting pair — write through `rd`.
    [[nodiscard]] A64Status AddExt(A64Reg rd, A64Reg rn, A64Reg rm, A64ExtendKind extend, unsigned amount = 0) const;
    [[nodiscard]] A64Status AddsExt(A64Reg rd, A64Reg rn, A64Reg rm, A64ExtendKind extend, unsigned amount = 0) const;
    [[nodiscard]] A64Status SubExt(A64Reg rd, A64Reg rn, A64Reg rm, A64ExtendKind extend, unsigned amount = 0) const;
    [[nodiscard]] A64Status SubsExt(A64Reg rd, A64Reg rn, A64Reg rm, A64ExtendKind extend, unsigned amount = 0) const;

    // Logical (shifted register). The four inverting forms apply the shift and
    // then complement `rm`; unlike the arithmetic forms these accept ROR.
    [[nodiscard]] A64Status And(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Bic(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Orr(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Orn(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Eor(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Eon(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Ands(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                 unsigned amount = 0) const;
    [[nodiscard]] A64Status Bics(A64Reg rd, A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                 unsigned amount = 0) const;

    // The logical aliases. MOV is ORR from the zero register, except when
    // either operand is SP — code 31 names the wrong register there, so the
    // stack-pointer form of MOV is ADD with an immediate of zero instead.
    [[nodiscard]] A64Status Tst(A64Reg rn, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Mvn(A64Reg rd, A64Reg rm, A64ShiftKind shift = A64ShiftKind::Lsl,
                                unsigned amount = 0) const;
    [[nodiscard]] A64Status Mov(A64Reg rd, A64Reg rm) const;

    // Variable shifts, which take the amount from a register and mask it to the
    // operand width, so no shift is ever out of range.
    [[nodiscard]] A64Status Lslv(A64Reg rd, A64Reg rn, A64Reg rm) const;
    [[nodiscard]] A64Status Lsrv(A64Reg rd, A64Reg rn, A64Reg rm) const;
    [[nodiscard]] A64Status Asrv(A64Reg rd, A64Reg rn, A64Reg rm) const;
    [[nodiscard]] A64Status Rorv(A64Reg rd, A64Reg rn, A64Reg rm) const;

    // SDIV and UDIV. Division by zero yields zero and overflow saturates, both
    // without trapping, so a language that needs a diagnostic must test first.
    [[nodiscard]] A64Status Sdiv(A64Reg rd, A64Reg rn, A64Reg rm) const;
    [[nodiscard]] A64Status Udiv(A64Reg rd, A64Reg rn, A64Reg rm) const;

    // Multiply-accumulate: rd = ra +/- rn * rm, at the width of `rd`. MUL and
    // MNEG are the forms that accumulate into the zero register.
    [[nodiscard]] A64Status Madd(A64Reg rd, A64Reg rn, A64Reg rm, A64Reg ra) const;
    [[nodiscard]] A64Status Msub(A64Reg rd, A64Reg rn, A64Reg rm, A64Reg ra) const;
    [[nodiscard]] A64Status Mul(A64Reg rd, A64Reg rn, A64Reg rm) const;
    [[nodiscard]] A64Status Mneg(A64Reg rd, A64Reg rn, A64Reg rm) const;

    // Widening multiply: a 32-bit `rn` times a 32-bit `rm` into a 64-bit `rd`,
    // so these are the only forms whose operand widths deliberately disagree.
    [[nodiscard]] A64Status Smaddl(A64Reg rd, A64Reg rn, A64Reg rm, A64Reg ra) const;
    [[nodiscard]] A64Status Umaddl(A64Reg rd, A64Reg rn, A64Reg rm, A64Reg ra) const;
    [[nodiscard]] A64Status Smull(A64Reg rd, A64Reg rn, A64Reg rm) const;
    [[nodiscard]] A64Status Umull(A64Reg rd, A64Reg rn, A64Reg rm) const;

    // The high half of a 64-by-64 multiply. All three operands are 64-bit.
    [[nodiscard]] A64Status Smulh(A64Reg rd, A64Reg rn, A64Reg rm) const;
    [[nodiscard]] A64Status Umulh(A64Reg rd, A64Reg rn, A64Reg rm) const;

    // Conditional select: `rd` takes `rn` when `cond` holds and otherwise a
    // transformation of `rm` — as it stands, incremented, complemented or
    // negated.
    [[nodiscard]] A64Status Csel(A64Reg rd, A64Reg rn, A64Reg rm, A64Condition cond) const;
    [[nodiscard]] A64Status Csinc(A64Reg rd, A64Reg rn, A64Reg rm, A64Condition cond) const;
    [[nodiscard]] A64Status Csinv(A64Reg rd, A64Reg rn, A64Reg rm, A64Condition cond) const;
    [[nodiscard]] A64Status Csneg(A64Reg rd, A64Reg rn, A64Reg rm, A64Condition cond) const;

    // The conditional-select aliases, each of which encodes the inverse of the
    // condition it is written with. AL and NV therefore have no alias form —
    // inverting them would silently reverse the sense of the instruction — and
    // are refused as InvalidImmediate.
    [[nodiscard]] A64Status Cset(A64Reg rd, A64Condition cond) const;
    [[nodiscard]] A64Status Csetm(A64Reg rd, A64Condition cond) const;
    [[nodiscard]] A64Status Cinc(A64Reg rd, A64Reg rn, A64Condition cond) const;
    [[nodiscard]] A64Status Cinv(A64Reg rd, A64Reg rn, A64Condition cond) const;
    [[nodiscard]] A64Status Cneg(A64Reg rd, A64Reg rn, A64Condition cond) const;

    // Bit and byte counting and reversal. REV reverses every byte of the
    // register and REV32 every byte of each word, so REV32 exists only in the
    // 64-bit form and REV at that width is what REV32 would otherwise be.
    [[nodiscard]] A64Status Clz(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Cls(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Rbit(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Rev(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Rev16(A64Reg rd, A64Reg rn) const;
    [[nodiscard]] A64Status Rev32(A64Reg rd, A64Reg rn) const;

private:
    std::vector<std::uint8_t> &out;
};
} // namespace Rux
