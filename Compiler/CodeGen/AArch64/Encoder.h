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

private:
    std::vector<std::uint8_t> &out;
};
} // namespace Rux
