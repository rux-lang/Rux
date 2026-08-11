#include "CodeGen/AArch64/Encoder.h"

#include <bit>

namespace Rux {
namespace {
// A value whose set bits form one contiguous run ending at bit 0.
constexpr bool IsMask(const std::uint64_t value) {
    return value != 0 && ((value + 1) & value) == 0;
}

// A value whose set bits form one contiguous run anywhere in the word.
constexpr bool IsShiftedMask(const std::uint64_t value) {
    return value != 0 && IsMask((value - 1) | value);
}

// All ones in the low `bits` of a 64-bit word.
constexpr std::uint64_t LowMask(const unsigned bits) {
    return ~0ULL >> (64U - bits);
}

// The halfword index of the only non-zero halfword of `value`, or -1 when it
// has none or more than one. A zero value reports halfword 0, which encodes as
// MOVZ #0.
constexpr int SoleHalfword(const std::uint64_t value, const unsigned halfwords) {
    for (unsigned hw = 0; hw < halfwords; ++hw) {
        if ((value & ~(0xFFFFULL << (hw * 16U))) == 0) {
            return static_cast<int>(hw);
        }
    }
    return -1;
}

// A general-purpose operand of width `bits` in a field that reads code 31 as
// the zero register. SP has no encoding there, so passing it is an error rather
// than a silent rename.
constexpr bool ZrOperand(const A64Reg &reg, const unsigned bits) {
    return reg.IsGeneral() && reg.bits == bits && !reg.IsStackPointer();
}

// The same, for a field that reads code 31 as the stack pointer.
constexpr bool SpOperand(const A64Reg &reg, const unsigned bits) {
    return reg.IsGeneral() && reg.bits == bits && !reg.IsZeroReg();
}

// Whether a signed value fits a two's-complement field `bits` wide.
constexpr bool FitsSigned(const std::int64_t value, const unsigned bits) {
    const std::int64_t limit = std::int64_t{1} << (bits - 1U);
    return value >= -limit && value < limit;
}

// ADD / ADDS / SUB / SUBS (immediate): sf | op | S | 100010 | sh | imm12 | Rn | Rd.
A64Status EncodeAddSubImm(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const std::uint64_t imm,
                          const bool subtract, const bool setFlags) {
    const unsigned bits = rd.bits;
    // Only the flag-setting forms leave code 31 free for the zero register,
    // which is what CMP and CMN are.
    if (!(setFlags ? ZrOperand(rd, bits) : SpOperand(rd, bits)) || !SpOperand(rn, bits)) {
        return A64Status::InvalidRegister;
    }
    const std::optional<A64ArithImm> encoded = TryEncodeArithImm12(imm);
    if (!encoded) {
        return A64Status::InvalidImmediate;
    }
    enc.Word(rd.Sf() << 31U | (subtract ? 1U : 0U) << 30U | (setFlags ? 1U : 0U) << 29U | 0x22U << 23U |
             (encoded->shift12 ? 1U : 0U) << 22U | std::uint32_t{encoded->imm12} << 10U | std::uint32_t{rn.code} << 5U |
             rd.code);
    return A64Status::Ok;
}

// AND / ORR / EOR / ANDS (bitmask immediate): sf | opc | 100100 | N | immr | imms | Rn | Rd.
A64Status EncodeLogicalImm(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const std::uint64_t imm,
                           const unsigned opc) {
    const unsigned bits = rd.bits;
    const bool setsFlags = opc == 3U;
    if (!(setsFlags ? ZrOperand(rd, bits) : SpOperand(rd, bits)) || !ZrOperand(rn, bits)) {
        return A64Status::InvalidRegister;
    }
    const std::optional<A64BitmaskImm> encoded = TryEncodeBitmaskImm(imm, bits == 64);
    if (!encoded) {
        return A64Status::InvalidImmediate;
    }
    enc.Word(rd.Sf() << 31U | opc << 29U | 0x24U << 23U | std::uint32_t{encoded->n} << 22U |
             std::uint32_t{encoded->immr} << 16U | std::uint32_t{encoded->imms} << 10U | std::uint32_t{rn.code} << 5U |
             rd.code);
    return A64Status::Ok;
}

// MOVN / MOVZ / MOVK: sf | opc | 100101 | hw | imm16 | Rd.
A64Status EncodeMovWide(const A64Enc &enc, const A64Reg rd, const std::uint16_t imm16, const unsigned shift,
                        const unsigned opc) {
    if (!ZrOperand(rd, rd.bits)) {
        return A64Status::InvalidRegister;
    }
    if (shift % 16U != 0 || shift >= rd.bits) {
        return A64Status::InvalidImmediate;
    }
    enc.Word(rd.Sf() << 31U | opc << 29U | 0x25U << 23U | (shift / 16U) << 21U | std::uint32_t{imm16} << 5U | rd.code);
    return A64Status::Ok;
}

// ADR / ADRP: op | immlo | 10000 | immhi | Rd, the 21-bit immediate split with
// its low two bits stranded above the opcode.
A64Status EncodeAdr(const A64Enc &enc, const A64Reg rd, const std::int64_t offset, const bool page) {
    if (!ZrOperand(rd, 64)) {
        return A64Status::InvalidRegister;
    }
    if (page && offset % 4096 != 0) {
        return A64Status::Unaligned;
    }
    const std::int64_t imm = page ? offset / 4096 : offset;
    if (!FitsSigned(imm, 21)) {
        return A64Status::OutOfRange;
    }
    const auto bitsOf = static_cast<std::uint32_t>(imm & 0x1FFFFF);
    enc.Word((page ? 1U : 0U) << 31U | (bitsOf & 3U) << 29U | 0x10U << 24U | (bitsOf >> 2U) << 5U | rd.code);
    return A64Status::Ok;
}

// SBFM / BFM / UBFM: sf | opc | 100110 | N | immr | imms | Rn | Rd. The N bit
// selects the register width alongside sf and always matches it.
A64Status EncodeBitfield(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const unsigned immr, const unsigned imms,
                         const unsigned opc) {
    const unsigned bits = rd.bits;
    if (!ZrOperand(rd, bits) || !ZrOperand(rn, bits)) {
        return A64Status::InvalidRegister;
    }
    if (immr >= bits || imms >= bits) {
        return A64Status::InvalidImmediate;
    }
    enc.Word(rd.Sf() << 31U | opc << 29U | 0x26U << 23U | rd.Sf() << 22U | immr << 16U | imms << 10U |
             std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

// The extends are all bitfield moves of a field that starts at bit 0, so they
// differ only in how many bits they keep and whether they replicate the sign.
A64Status EncodeExtend(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const unsigned width,
                       const bool signExtend) {
    if (!ZrOperand(rn, 32)) {
        return A64Status::InvalidRegister;
    }
    // The bitfield instruction reads its source at the destination's width, so
    // the operand the caller wrote as Wn is encoded as Xn for a 64-bit result.
    const A64Reg source = A64::Gpr(rn.code, rd.bits);
    return EncodeBitfield(enc, rd, source, 0, width - 1U, signExtend ? 0U : 2U);
}
} // namespace

std::string_view A64StatusName(const A64Status status) {
    switch (status) {
    case A64Status::Ok:
        return "ok";
    case A64Status::InvalidRegister:
        return "invalid register";
    case A64Status::InvalidImmediate:
        return "invalid immediate";
    case A64Status::OutOfRange:
        return "offset out of range";
    case A64Status::Unaligned:
        return "misaligned offset";
    }
    return "unknown status";
}

std::optional<A64BitmaskImm> TryEncodeBitmaskImm(std::uint64_t value, const bool is64) {
    const unsigned regSize = is64 ? 64U : 32U;
    if (!is64 && (value >> 32U) != 0) {
        return std::nullopt;
    }
    // A run of ones cannot cover the whole register or none of it, because
    // both patterns are also every rotation of themselves.
    if (value == 0 || value == LowMask(regSize)) {
        return std::nullopt;
    }

    // The element size is the narrowest power-of-two slice the value repeats
    // in; the search stops at 2 bits, the narrowest element the encoding has.
    unsigned size = regSize;
    while (size > 2) {
        size /= 2;
        const std::uint64_t half = LowMask(size);
        if ((value & half) != ((value >> size) & half)) {
            size *= 2;
            break;
        }
    }

    // Within one element the value must be a rotation of `0*1*`. `rotation` is
    // how far right that run has moved, `ones` how long it is.
    const std::uint64_t mask = LowMask(size);
    value &= mask;
    unsigned rotation = 0;
    unsigned ones = 0;
    if (IsShiftedMask(value)) {
        rotation = static_cast<unsigned>(std::countr_zero(value));
        ones = static_cast<unsigned>(std::countr_one(value >> rotation));
    }
    else {
        // The run wraps around the top of the element, so it is the zeros that
        // are contiguous. Widening the value with ones above the element lets
        // the same count work on the whole word.
        value |= ~mask;
        if (!IsShiftedMask(~value)) {
            return std::nullopt;
        }
        const unsigned leadingOnes = static_cast<unsigned>(std::countl_one(value));
        rotation = 64U - leadingOnes;
        ones = leadingOnes + static_cast<unsigned>(std::countr_one(value)) - (64U - size);
    }

    // immr rotates the canonical run right into place; imms carries the run
    // length in the low bits under a prefix that names the element size, and
    // the N bit is the sixth bit of that prefix, inverted.
    const std::uint64_t imms = (~static_cast<std::uint64_t>(size - 1U) << 1U) | (ones - 1U);
    A64BitmaskImm encoded;
    encoded.n = static_cast<std::uint8_t>(((imms >> 6U) & 1U) ^ 1U);
    encoded.immr = static_cast<std::uint8_t>((size - rotation) & (size - 1U));
    encoded.imms = static_cast<std::uint8_t>(imms & 0x3FU);
    return encoded;
}

std::optional<A64ArithImm> TryEncodeArithImm12(const std::uint64_t value) {
    if (value <= 0xFFFU) {
        return A64ArithImm{static_cast<std::uint16_t>(value), false};
    }
    if ((value & 0xFFFU) == 0 && (value >> 12U) <= 0xFFFU) {
        return A64ArithImm{static_cast<std::uint16_t>(value >> 12U), true};
    }
    return std::nullopt;
}

std::optional<A64MovwImm> TryEncodeMovwImm(const std::uint64_t value, const bool is64) {
    const unsigned regSize = is64 ? 64U : 32U;
    if (!is64 && (value >> 32U) != 0) {
        return std::nullopt;
    }

    const unsigned halfwords = regSize / 16U;
    if (const int hw = SoleHalfword(value, halfwords); hw >= 0) {
        const unsigned shift = static_cast<unsigned>(hw) * 16U;
        return A64MovwImm{static_cast<std::uint16_t>(value >> shift), static_cast<std::uint8_t>(hw), false};
    }
    // MOVN writes the inverse, so a value that is mostly ones costs one
    // instruction too.
    const std::uint64_t inverted = ~value & LowMask(regSize);
    if (const int hw = SoleHalfword(inverted, halfwords); hw >= 0) {
        const unsigned shift = static_cast<unsigned>(hw) * 16U;
        return A64MovwImm{static_cast<std::uint16_t>(inverted >> shift), static_cast<std::uint8_t>(hw), true};
    }
    return std::nullopt;
}

std::uint32_t A64Enc::Size() const {
    return static_cast<std::uint32_t>(out.size());
}

void A64Enc::Word(const std::uint32_t word) const {
    out.push_back(static_cast<std::uint8_t>(word & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((word >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((word >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((word >> 24U) & 0xFFU));
}

std::uint32_t A64Enc::WordAt(const std::uint32_t offset) const {
    std::uint32_t word = 0;
    for (std::uint32_t i = 0; i < InstrSize; ++i) {
        word |= static_cast<std::uint32_t>(out[offset + i]) << (i * 8U);
    }
    return word;
}

void A64Enc::PatchWord(const std::uint32_t offset, const std::uint32_t word) const {
    for (std::uint32_t i = 0; i < InstrSize; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((word >> (i * 8U)) & 0xFFU);
    }
}

void A64Enc::PatchField(const std::uint32_t offset, const unsigned lsb, const unsigned width,
                        const std::uint32_t value) const {
    const std::uint32_t mask = width >= 32U ? ~0U : (1U << width) - 1U;
    const std::uint32_t word = (WordAt(offset) & ~(mask << lsb)) | ((value & mask) << lsb);
    PatchWord(offset, word);
}

A64Status A64Enc::AddImm(const A64Reg rd, const A64Reg rn, const std::uint64_t imm) const {
    return EncodeAddSubImm(*this, rd, rn, imm, false, false);
}

A64Status A64Enc::AddsImm(const A64Reg rd, const A64Reg rn, const std::uint64_t imm) const {
    return EncodeAddSubImm(*this, rd, rn, imm, false, true);
}

A64Status A64Enc::SubImm(const A64Reg rd, const A64Reg rn, const std::uint64_t imm) const {
    return EncodeAddSubImm(*this, rd, rn, imm, true, false);
}

A64Status A64Enc::SubsImm(const A64Reg rd, const A64Reg rn, const std::uint64_t imm) const {
    return EncodeAddSubImm(*this, rd, rn, imm, true, true);
}

A64Status A64Enc::AndImm(const A64Reg rd, const A64Reg rn, const std::uint64_t imm) const {
    return EncodeLogicalImm(*this, rd, rn, imm, 0);
}

A64Status A64Enc::OrrImm(const A64Reg rd, const A64Reg rn, const std::uint64_t imm) const {
    return EncodeLogicalImm(*this, rd, rn, imm, 1);
}

A64Status A64Enc::EorImm(const A64Reg rd, const A64Reg rn, const std::uint64_t imm) const {
    return EncodeLogicalImm(*this, rd, rn, imm, 2);
}

A64Status A64Enc::AndsImm(const A64Reg rd, const A64Reg rn, const std::uint64_t imm) const {
    return EncodeLogicalImm(*this, rd, rn, imm, 3);
}

A64Status A64Enc::Movn(const A64Reg rd, const std::uint16_t imm16, const unsigned shift) const {
    return EncodeMovWide(*this, rd, imm16, shift, 0);
}

A64Status A64Enc::Movz(const A64Reg rd, const std::uint16_t imm16, const unsigned shift) const {
    return EncodeMovWide(*this, rd, imm16, shift, 2);
}

A64Status A64Enc::Movk(const A64Reg rd, const std::uint16_t imm16, const unsigned shift) const {
    return EncodeMovWide(*this, rd, imm16, shift, 3);
}

A64Status A64Enc::Adr(const A64Reg rd, const std::int64_t offset) const {
    return EncodeAdr(*this, rd, offset, false);
}

A64Status A64Enc::Adrp(const A64Reg rd, const std::int64_t offset) const {
    return EncodeAdr(*this, rd, offset, true);
}

A64Status A64Enc::Sbfm(const A64Reg rd, const A64Reg rn, const unsigned immr, const unsigned imms) const {
    return EncodeBitfield(*this, rd, rn, immr, imms, 0);
}

A64Status A64Enc::Bfm(const A64Reg rd, const A64Reg rn, const unsigned immr, const unsigned imms) const {
    return EncodeBitfield(*this, rd, rn, immr, imms, 1);
}

A64Status A64Enc::Ubfm(const A64Reg rd, const A64Reg rn, const unsigned immr, const unsigned imms) const {
    return EncodeBitfield(*this, rd, rn, immr, imms, 2);
}

A64Status A64Enc::Lsl(const A64Reg rd, const A64Reg rn, const unsigned shift) const {
    if (shift >= rd.bits) {
        return A64Status::InvalidImmediate;
    }
    // Shifting left by n keeps the low width - n bits and rotates them up,
    // which is the same as rotating right by the rest of the register.
    return Ubfm(rd, rn, (rd.bits - shift) % rd.bits, rd.bits - 1U - shift);
}

A64Status A64Enc::Lsr(const A64Reg rd, const A64Reg rn, const unsigned shift) const {
    if (shift >= rd.bits) {
        return A64Status::InvalidImmediate;
    }
    return Ubfm(rd, rn, shift, rd.bits - 1U);
}

A64Status A64Enc::Asr(const A64Reg rd, const A64Reg rn, const unsigned shift) const {
    if (shift >= rd.bits) {
        return A64Status::InvalidImmediate;
    }
    return Sbfm(rd, rn, shift, rd.bits - 1U);
}

A64Status A64Enc::Ubfx(const A64Reg rd, const A64Reg rn, const unsigned lsb, const unsigned width) const {
    if (width == 0 || lsb + width > rd.bits) {
        return A64Status::InvalidImmediate;
    }
    return Ubfm(rd, rn, lsb, lsb + width - 1U);
}

A64Status A64Enc::Sbfx(const A64Reg rd, const A64Reg rn, const unsigned lsb, const unsigned width) const {
    if (width == 0 || lsb + width > rd.bits) {
        return A64Status::InvalidImmediate;
    }
    return Sbfm(rd, rn, lsb, lsb + width - 1U);
}

A64Status A64Enc::Bfi(const A64Reg rd, const A64Reg rn, const unsigned lsb, const unsigned width) const {
    if (width == 0 || lsb + width > rd.bits) {
        return A64Status::InvalidImmediate;
    }
    return Bfm(rd, rn, (rd.bits - lsb) % rd.bits, width - 1U);
}

A64Status A64Enc::Bfxil(const A64Reg rd, const A64Reg rn, const unsigned lsb, const unsigned width) const {
    if (width == 0 || lsb + width > rd.bits) {
        return A64Status::InvalidImmediate;
    }
    return Bfm(rd, rn, lsb, lsb + width - 1U);
}

A64Status A64Enc::Sxtb(const A64Reg rd, const A64Reg rn) const {
    return EncodeExtend(*this, rd, rn, 8, true);
}

A64Status A64Enc::Sxth(const A64Reg rd, const A64Reg rn) const {
    return EncodeExtend(*this, rd, rn, 16, true);
}

A64Status A64Enc::Sxtw(const A64Reg rd, const A64Reg rn) const {
    if (rd.bits != 64) {
        return A64Status::InvalidRegister;
    }
    return EncodeExtend(*this, rd, rn, 32, true);
}

A64Status A64Enc::Uxtb(const A64Reg rd, const A64Reg rn) const {
    if (rd.bits != 32) {
        return A64Status::InvalidRegister;
    }
    return EncodeExtend(*this, rd, rn, 8, false);
}

A64Status A64Enc::Uxth(const A64Reg rd, const A64Reg rn) const {
    if (rd.bits != 32) {
        return A64Status::InvalidRegister;
    }
    return EncodeExtend(*this, rd, rn, 16, false);
}

A64Status A64Enc::Extr(const A64Reg rd, const A64Reg rn, const A64Reg rm, const unsigned lsb) const {
    const unsigned bits = rd.bits;
    if (!ZrOperand(rd, bits) || !ZrOperand(rn, bits) || !ZrOperand(rm, bits)) {
        return A64Status::InvalidRegister;
    }
    if (lsb >= bits) {
        return A64Status::InvalidImmediate;
    }
    Word(rd.Sf() << 31U | 0x27U << 23U | rd.Sf() << 22U | std::uint32_t{rm.code} << 16U | lsb << 10U |
         std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

A64Status A64Enc::Ror(const A64Reg rd, const A64Reg rn, const unsigned shift) const {
    return Extr(rd, rn, rn, shift);
}
} // namespace Rux
