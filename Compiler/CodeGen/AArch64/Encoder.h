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

private:
    std::vector<std::uint8_t> &out;
};
} // namespace Rux
