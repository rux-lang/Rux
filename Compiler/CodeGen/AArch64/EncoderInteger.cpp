#include "CodeGen/AArch64/Encoder.h"

namespace Rux {
namespace {
// All ones in the low `bits` of a 64-bit word.
constexpr std::uint64_t LowMask(const unsigned bits) {
    return ~0ULL >> (64U - bits);
}

// The `hw`-th 16-bit halfword of a value, counting from the bottom.
constexpr std::uint16_t Halfword(const std::uint64_t value, const unsigned hw) {
    return static_cast<std::uint16_t>(value >> (hw * 16U));
}

// The shortest move chain for a value at a given register width, and which
// root it grows from. A chain costs one instruction per halfword its root does
// not already leave behind: MOVZ zeroes the other halfwords and MOVN fills them
// with ones, so the two roots count the halfwords that are not 0 and not
// 0xFFFF. A value every halfword of which is what its root leaves behind is the
// root alone rather than an empty sequence.
struct MovChain {
    unsigned length = 1;
    bool invert = false;
};

constexpr MovChain ShortestMovChain(const std::uint64_t value, const unsigned bits) {
    unsigned zeroRooted = 0;
    unsigned onesRooted = 0;
    for (unsigned hw = 0; hw < bits / 16U; ++hw) {
        const std::uint16_t part = Halfword(value, hw);
        zeroRooted += part != 0 ? 1U : 0U;
        onesRooted += part != 0xFFFFU ? 1U : 0U;
    }
    const bool invert = onesRooted < zeroRooted;
    const unsigned length = invert ? onesRooted : zeroRooted;
    return MovChain{length != 0 ? length : 1U, invert};
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

// ADD / ADDS / SUB / SUBS (shifted register):
// sf | op | S | 01011 | shift | 0 | Rm | imm6 | Rn | Rd.
A64Status EncodeAddSubShifted(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const A64Reg rm,
                              const A64ShiftKind shift, const unsigned amount, const bool subtract,
                              const bool setFlags) {
    const unsigned bits = rd.bits;
    if (!ZrOperand(rd, bits) || !ZrOperand(rn, bits) || !ZrOperand(rm, bits)) {
        return A64Status::InvalidRegister;
    }
    // ROR is a logical-only shift; the arithmetic forms leave its encoding
    // unallocated rather than giving it a meaning.
    if (shift == A64ShiftKind::Ror || amount >= bits) {
        return A64Status::InvalidImmediate;
    }
    enc.Word(rd.Sf() << 31U | (subtract ? 1U : 0U) << 30U | (setFlags ? 1U : 0U) << 29U | 0x0BU << 24U |
             static_cast<std::uint32_t>(shift) << 22U | std::uint32_t{rm.code} << 16U | amount << 10U |
             std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

// ADD / ADDS / SUB / SUBS (extended register):
// sf | op | S | 01011 | 00 | 1 | Rm | option | imm3 | Rn | Rd.
A64Status EncodeAddSubExtended(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const A64Reg rm,
                               const A64ExtendKind extend, const unsigned amount, const bool subtract,
                               const bool setFlags) {
    const unsigned bits = rd.bits;
    if (!(setFlags ? ZrOperand(rd, bits) : SpOperand(rd, bits)) || !SpOperand(rn, bits)) {
        return A64Status::InvalidRegister;
    }
    // The extension says how much of `rm` is read, so everything short of a
    // whole doubleword is a W register. A 32-bit instruction has no doubleword
    // source at all, which leaves UXTX and SXTX naming a W register too.
    const bool wholeRegister = bits == 64 && (extend == A64ExtendKind::Uxtx || extend == A64ExtendKind::Sxtx);
    if (!ZrOperand(rm, wholeRegister ? 64U : 32U)) {
        return A64Status::InvalidRegister;
    }
    if (amount > 4) {
        return A64Status::InvalidImmediate;
    }
    enc.Word(rd.Sf() << 31U | (subtract ? 1U : 0U) << 30U | (setFlags ? 1U : 0U) << 29U | 0x0BU << 24U | 1U << 21U |
             std::uint32_t{rm.code} << 16U | static_cast<std::uint32_t>(extend) << 13U | amount << 10U |
             std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

// Logical (shifted register): sf | opc | 01010 | shift | N | Rm | imm6 | Rn | Rd.
// `opc` picks the operation and `N` complements the shifted `rm`.
A64Status EncodeLogicalShifted(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const A64Reg rm,
                               const A64ShiftKind shift, const unsigned amount, const unsigned opc, const bool negate) {
    const unsigned bits = rd.bits;
    if (!ZrOperand(rd, bits) || !ZrOperand(rn, bits) || !ZrOperand(rm, bits)) {
        return A64Status::InvalidRegister;
    }
    if (amount >= bits) {
        return A64Status::InvalidImmediate;
    }
    enc.Word(rd.Sf() << 31U | opc << 29U | 0x0AU << 24U | static_cast<std::uint32_t>(shift) << 22U |
             (negate ? 1U : 0U) << 21U | std::uint32_t{rm.code} << 16U | amount << 10U | std::uint32_t{rn.code} << 5U |
             rd.code);
    return A64Status::Ok;
}

// Data processing (2 source): sf | 0 | S | 11010110 | Rm | opcode | Rn | Rd.
// The variable shifts and the two divides share it.
A64Status EncodeDataProc2(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const A64Reg rm, const unsigned opcode) {
    const unsigned bits = rd.bits;
    if (!ZrOperand(rd, bits) || !ZrOperand(rn, bits) || !ZrOperand(rm, bits)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(rd.Sf() << 31U | 0xD6U << 21U | std::uint32_t{rm.code} << 16U | opcode << 10U |
             std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

// Data processing (1 source): sf | 1 | S | 11010110 | 00000 | opcode | Rn | Rd.
A64Status EncodeDataProc1(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const unsigned opcode) {
    const unsigned bits = rd.bits;
    if (!ZrOperand(rd, bits) || !ZrOperand(rn, bits)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(rd.Sf() << 31U | 1U << 30U | 0xD6U << 21U | opcode << 10U | std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

// Data processing (3 source): sf | 00 | 11011 | op31 | Rm | o0 | Ra | Rn | Rd.
// `op31` and `o0` together name the operation, and the widening and high-half
// forms differ from the plain ones only in the widths their operands take.
A64Status EncodeMulAcc(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra,
                       const unsigned op31, const bool subtract, const unsigned sourceBits) {
    if (!ZrOperand(rd, rd.bits) || !ZrOperand(ra, rd.bits)) {
        return A64Status::InvalidRegister;
    }
    if (!ZrOperand(rn, sourceBits) || !ZrOperand(rm, sourceBits)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(rd.Sf() << 31U | 0x1BU << 24U | op31 << 21U | std::uint32_t{rm.code} << 16U | (subtract ? 1U : 0U) << 15U |
             std::uint32_t{ra.code} << 10U | std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

// Conditional select: sf | op | S | 11010100 | Rm | cond | op2 | Rn | Rd.
A64Status EncodeCondSelect(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const A64Reg rm,
                           const A64Condition cond, const bool invert, const bool increment) {
    const unsigned bits = rd.bits;
    if (!ZrOperand(rd, bits) || !ZrOperand(rn, bits) || !ZrOperand(rm, bits)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(rd.Sf() << 31U | (invert ? 1U : 0U) << 30U | 0xD4U << 21U | std::uint32_t{rm.code} << 16U |
             static_cast<std::uint32_t>(cond) << 12U | (increment ? 1U : 0U) << 10U | std::uint32_t{rn.code} << 5U |
             rd.code);
    return A64Status::Ok;
}

// Every conditional-select alias reads as "when `cond`, do the thing", which
// the underlying instruction spells as "unless `cond`, transform rm". AL and NV
// are each other's inverse and mean the same thing, so no alias can be written
// with them without reversing its own sense.
constexpr bool InvertibleCondition(const A64Condition cond) {
    return cond != A64Condition::Al && cond != A64Condition::Nv;
}

// A branch immediate counts instructions from the branch itself, so a byte
// offset divides by four before it is measured against the field it has to fit.
// `imm` is left alone unless the offset encodes.
A64Status BranchOffset(const std::int64_t offset, const unsigned bits, std::uint32_t &imm) {
    if (offset % A64Enc::InstrSize != 0) {
        return A64Status::Unaligned;
    }
    const std::int64_t instrs = offset / A64Enc::InstrSize;
    if (!FitsSigned(instrs, bits)) {
        return A64Status::OutOfRange;
    }
    imm = static_cast<std::uint32_t>(static_cast<std::uint64_t>(instrs) & LowMask(bits));
    return A64Status::Ok;
}

// B / BL: op | 00101 | imm26.
A64Status EncodeBranchImm(const A64Enc &enc, const std::int64_t offset, const bool link) {
    std::uint32_t imm = 0;
    if (const A64Status status = BranchOffset(offset, 26, imm); status != A64Status::Ok) {
        return status;
    }
    enc.Word((link ? 1U : 0U) << 31U | 0x05U << 26U | imm);
    return A64Status::Ok;
}

// Compare and branch: sf | 011010 | op | imm19 | Rt.
A64Status EncodeCompareBranch(const A64Enc &enc, const A64Reg rt, const std::int64_t offset, const bool notZero) {
    if (!ZrOperand(rt, rt.bits) || (rt.bits != 32 && rt.bits != 64)) {
        return A64Status::InvalidRegister;
    }
    std::uint32_t imm = 0;
    if (const A64Status status = BranchOffset(offset, 19, imm); status != A64Status::Ok) {
        return status;
    }
    enc.Word(rt.Sf() << 31U | 0x1AU << 25U | (notZero ? 1U : 0U) << 24U | imm << 5U | rt.code);
    return A64Status::Ok;
}

// Test and branch: b5 | 011011 | op | b40 | imm14 | Rt. The bit index is split
// with its top bit above the opcode, where every other instruction keeps sf.
A64Status EncodeTestBranch(const A64Enc &enc, const A64Reg rt, const unsigned bit, const std::int64_t offset,
                           const bool notZero) {
    if (!ZrOperand(rt, rt.bits) || (rt.bits != 32 && rt.bits != 64)) {
        return A64Status::InvalidRegister;
    }
    if (bit >= rt.bits) {
        return A64Status::InvalidImmediate;
    }
    std::uint32_t imm = 0;
    if (const A64Status status = BranchOffset(offset, 14, imm); status != A64Status::Ok) {
        return status;
    }
    enc.Word((bit >> 5U) << 31U | 0x1BU << 25U | (notZero ? 1U : 0U) << 24U | (bit & 31U) << 19U | imm << 5U | rt.code);
    return A64Status::Ok;
}

// Unconditional branch to a register: 1101011 | opc | 11111 | 000000 | Rn | 00000.
A64Status EncodeBranchReg(const A64Enc &enc, const A64Reg rn, const unsigned opc) {
    if (!ZrOperand(rn, 64)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(0xD61F0000U | opc << 21U | std::uint32_t{rn.code} << 5U);
    return A64Status::Ok;
}

} // namespace

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

A64Status A64Enc::Add(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                      const unsigned amount) const {
    return EncodeAddSubShifted(*this, rd, rn, rm, shift, amount, false, false);
}

A64Status A64Enc::Adds(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                       const unsigned amount) const {
    return EncodeAddSubShifted(*this, rd, rn, rm, shift, amount, false, true);
}

A64Status A64Enc::Sub(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                      const unsigned amount) const {
    return EncodeAddSubShifted(*this, rd, rn, rm, shift, amount, true, false);
}

A64Status A64Enc::Subs(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                       const unsigned amount) const {
    return EncodeAddSubShifted(*this, rd, rn, rm, shift, amount, true, true);
}

A64Status A64Enc::Cmp(const A64Reg rn, const A64Reg rm, const A64ShiftKind shift, const unsigned amount) const {
    return Subs(A64::Gpr(31, rn.bits), rn, rm, shift, amount);
}

A64Status A64Enc::Cmn(const A64Reg rn, const A64Reg rm, const A64ShiftKind shift, const unsigned amount) const {
    return Adds(A64::Gpr(31, rn.bits), rn, rm, shift, amount);
}

A64Status A64Enc::Neg(const A64Reg rd, const A64Reg rm, const A64ShiftKind shift, const unsigned amount) const {
    return Sub(rd, A64::Gpr(31, rd.bits), rm, shift, amount);
}

A64Status A64Enc::Negs(const A64Reg rd, const A64Reg rm, const A64ShiftKind shift, const unsigned amount) const {
    return Subs(rd, A64::Gpr(31, rd.bits), rm, shift, amount);
}

A64Status A64Enc::AddExt(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                         const unsigned amount) const {
    return EncodeAddSubExtended(*this, rd, rn, rm, extend, amount, false, false);
}

A64Status A64Enc::AddsExt(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                          const unsigned amount) const {
    return EncodeAddSubExtended(*this, rd, rn, rm, extend, amount, false, true);
}

A64Status A64Enc::SubExt(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                         const unsigned amount) const {
    return EncodeAddSubExtended(*this, rd, rn, rm, extend, amount, true, false);
}

A64Status A64Enc::SubsExt(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                          const unsigned amount) const {
    return EncodeAddSubExtended(*this, rd, rn, rm, extend, amount, true, true);
}

A64Status A64Enc::And(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                      const unsigned amount) const {
    return EncodeLogicalShifted(*this, rd, rn, rm, shift, amount, 0, false);
}

A64Status A64Enc::Bic(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                      const unsigned amount) const {
    return EncodeLogicalShifted(*this, rd, rn, rm, shift, amount, 0, true);
}

A64Status A64Enc::Orr(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                      const unsigned amount) const {
    return EncodeLogicalShifted(*this, rd, rn, rm, shift, amount, 1, false);
}

A64Status A64Enc::Orn(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                      const unsigned amount) const {
    return EncodeLogicalShifted(*this, rd, rn, rm, shift, amount, 1, true);
}

A64Status A64Enc::Eor(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                      const unsigned amount) const {
    return EncodeLogicalShifted(*this, rd, rn, rm, shift, amount, 2, false);
}

A64Status A64Enc::Eon(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                      const unsigned amount) const {
    return EncodeLogicalShifted(*this, rd, rn, rm, shift, amount, 2, true);
}

A64Status A64Enc::Ands(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                       const unsigned amount) const {
    return EncodeLogicalShifted(*this, rd, rn, rm, shift, amount, 3, false);
}

A64Status A64Enc::Bics(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64ShiftKind shift,
                       const unsigned amount) const {
    return EncodeLogicalShifted(*this, rd, rn, rm, shift, amount, 3, true);
}

A64Status A64Enc::Tst(const A64Reg rn, const A64Reg rm, const A64ShiftKind shift, const unsigned amount) const {
    return Ands(A64::Gpr(31, rn.bits), rn, rm, shift, amount);
}

A64Status A64Enc::Mvn(const A64Reg rd, const A64Reg rm, const A64ShiftKind shift, const unsigned amount) const {
    return Orn(rd, A64::Gpr(31, rd.bits), rm, shift, amount);
}

A64Status A64Enc::Mov(const A64Reg rd, const A64Reg rm) const {
    // ORR reads code 31 as the zero register, so a MOV naming SP has to be the
    // arithmetic form instead. That form in turn cannot name XZR, which is why
    // MOV between the two readings of code 31 does not exist.
    if (rd.IsStackPointer() || rm.IsStackPointer()) {
        return AddImm(rd, rm, 0);
    }
    return Orr(rd, A64::Gpr(31, rd.bits), rm);
}

A64Status A64Enc::Lslv(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeDataProc2(*this, rd, rn, rm, 0x08);
}

A64Status A64Enc::Lsrv(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeDataProc2(*this, rd, rn, rm, 0x09);
}

A64Status A64Enc::Asrv(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeDataProc2(*this, rd, rn, rm, 0x0A);
}

A64Status A64Enc::Rorv(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeDataProc2(*this, rd, rn, rm, 0x0B);
}

A64Status A64Enc::Udiv(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeDataProc2(*this, rd, rn, rm, 0x02);
}

A64Status A64Enc::Sdiv(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeDataProc2(*this, rd, rn, rm, 0x03);
}

A64Status A64Enc::Madd(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra) const {
    return EncodeMulAcc(*this, rd, rn, rm, ra, 0, false, rd.bits);
}

A64Status A64Enc::Msub(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra) const {
    return EncodeMulAcc(*this, rd, rn, rm, ra, 0, true, rd.bits);
}

A64Status A64Enc::Mul(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return Madd(rd, rn, rm, A64::Gpr(31, rd.bits));
}

A64Status A64Enc::Mneg(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return Msub(rd, rn, rm, A64::Gpr(31, rd.bits));
}

A64Status A64Enc::Smaddl(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra) const {
    if (rd.bits != 64) {
        return A64Status::InvalidRegister;
    }
    return EncodeMulAcc(*this, rd, rn, rm, ra, 1, false, 32);
}

A64Status A64Enc::Umaddl(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra) const {
    if (rd.bits != 64) {
        return A64Status::InvalidRegister;
    }
    return EncodeMulAcc(*this, rd, rn, rm, ra, 5, false, 32);
}

A64Status A64Enc::Smull(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return Smaddl(rd, rn, rm, A64::Xzr);
}

A64Status A64Enc::Umull(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return Umaddl(rd, rn, rm, A64::Xzr);
}

A64Status A64Enc::Smulh(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    if (rd.bits != 64) {
        return A64Status::InvalidRegister;
    }
    return EncodeMulAcc(*this, rd, rn, rm, A64::Xzr, 2, false, 64);
}

A64Status A64Enc::Umulh(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    if (rd.bits != 64) {
        return A64Status::InvalidRegister;
    }
    return EncodeMulAcc(*this, rd, rn, rm, A64::Xzr, 6, false, 64);
}

A64Status A64Enc::Csel(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Condition cond) const {
    return EncodeCondSelect(*this, rd, rn, rm, cond, false, false);
}

A64Status A64Enc::Csinc(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Condition cond) const {
    return EncodeCondSelect(*this, rd, rn, rm, cond, false, true);
}

A64Status A64Enc::Csinv(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Condition cond) const {
    return EncodeCondSelect(*this, rd, rn, rm, cond, true, false);
}

A64Status A64Enc::Csneg(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Condition cond) const {
    return EncodeCondSelect(*this, rd, rn, rm, cond, true, true);
}

A64Status A64Enc::Cset(const A64Reg rd, const A64Condition cond) const {
    if (!InvertibleCondition(cond)) {
        return A64Status::InvalidImmediate;
    }
    const A64Reg zero = A64::Gpr(31, rd.bits);
    return Csinc(rd, zero, zero, A64::InvertCondition(cond));
}

A64Status A64Enc::Csetm(const A64Reg rd, const A64Condition cond) const {
    if (!InvertibleCondition(cond)) {
        return A64Status::InvalidImmediate;
    }
    const A64Reg zero = A64::Gpr(31, rd.bits);
    return Csinv(rd, zero, zero, A64::InvertCondition(cond));
}

A64Status A64Enc::Cinc(const A64Reg rd, const A64Reg rn, const A64Condition cond) const {
    if (!InvertibleCondition(cond)) {
        return A64Status::InvalidImmediate;
    }
    return Csinc(rd, rn, rn, A64::InvertCondition(cond));
}

A64Status A64Enc::Cinv(const A64Reg rd, const A64Reg rn, const A64Condition cond) const {
    if (!InvertibleCondition(cond)) {
        return A64Status::InvalidImmediate;
    }
    return Csinv(rd, rn, rn, A64::InvertCondition(cond));
}

A64Status A64Enc::Cneg(const A64Reg rd, const A64Reg rn, const A64Condition cond) const {
    if (!InvertibleCondition(cond)) {
        return A64Status::InvalidImmediate;
    }
    return Csneg(rd, rn, rn, A64::InvertCondition(cond));
}

A64Status A64Enc::Rbit(const A64Reg rd, const A64Reg rn) const {
    return EncodeDataProc1(*this, rd, rn, 0x00);
}

A64Status A64Enc::Rev16(const A64Reg rd, const A64Reg rn) const {
    return EncodeDataProc1(*this, rd, rn, 0x01);
}

A64Status A64Enc::Rev32(const A64Reg rd, const A64Reg rn) const {
    // Reversing the bytes of each word is only distinct from REV where there is
    // more than one word, so the 32-bit encoding is REV itself and REV32 has
    // none of its own.
    if (rd.bits != 64) {
        return A64Status::InvalidRegister;
    }
    return EncodeDataProc1(*this, rd, rn, 0x02);
}

A64Status A64Enc::Rev(const A64Reg rd, const A64Reg rn) const {
    return EncodeDataProc1(*this, rd, rn, rd.bits == 64 ? 0x03U : 0x02U);
}

A64Status A64Enc::Clz(const A64Reg rd, const A64Reg rn) const {
    return EncodeDataProc1(*this, rd, rn, 0x04);
}

A64Status A64Enc::Cls(const A64Reg rd, const A64Reg rn) const {
    return EncodeDataProc1(*this, rd, rn, 0x05);
}

A64Status A64Enc::B(const std::int64_t offset) const {
    return EncodeBranchImm(*this, offset, false);
}

A64Status A64Enc::Bl(const std::int64_t offset) const {
    return EncodeBranchImm(*this, offset, true);
}

// B.cond: 0101010 | 0 | imm19 | 0 | cond.
A64Status A64Enc::BCond(const A64Condition cond, const std::int64_t offset) const {
    std::uint32_t imm = 0;
    if (const A64Status status = BranchOffset(offset, 19, imm); status != A64Status::Ok) {
        return status;
    }
    Word(0x54U << 24U | imm << 5U | static_cast<std::uint32_t>(cond));
    return A64Status::Ok;
}

A64Status A64Enc::Cbz(const A64Reg rt, const std::int64_t offset) const {
    return EncodeCompareBranch(*this, rt, offset, false);
}

A64Status A64Enc::Cbnz(const A64Reg rt, const std::int64_t offset) const {
    return EncodeCompareBranch(*this, rt, offset, true);
}

A64Status A64Enc::Tbz(const A64Reg rt, const unsigned bit, const std::int64_t offset) const {
    return EncodeTestBranch(*this, rt, bit, offset, false);
}

A64Status A64Enc::Tbnz(const A64Reg rt, const unsigned bit, const std::int64_t offset) const {
    return EncodeTestBranch(*this, rt, bit, offset, true);
}

A64Status A64Enc::Br(const A64Reg rn) const {
    return EncodeBranchReg(*this, rn, 0);
}

A64Status A64Enc::Blr(const A64Reg rn) const {
    return EncodeBranchReg(*this, rn, 1);
}

A64Status A64Enc::Ret(const A64Reg rn) const {
    return EncodeBranchReg(*this, rn, 2);
}

A64Status A64Enc::LoadImm64(const A64Reg rd, const std::uint64_t value) const {
    if (!ZrOperand(rd, rd.bits) || (rd.bits != 32 && rd.bits != 64)) {
        return A64Status::InvalidRegister;
    }
    // A W register has no room for a value wider than a word, and names
    // nothing that could hold one either.
    if (rd.bits == 32 && (value >> 32U) != 0) {
        return A64Status::InvalidImmediate;
    }

    unsigned bits = rd.bits;
    MovChain chain = ShortestMovChain(value, bits);
    bool logical = TryEncodeBitmaskImm(value, bits == 64).has_value();
    // Writing a W register zeroes the rest of the X register it is a view of,
    // so a 64-bit destination holding a value that fits in a word has a second
    // sequence available to it — often a shorter one, since the halfwords above
    // the word cost nothing there and the 32-bit logical immediates are a
    // different set from the 64-bit ones.
    if (bits == 64 && (value >> 32U) == 0 && (logical ? 1U : chain.length) > 1) {
        const MovChain narrow = ShortestMovChain(value, 32);
        const bool narrowLogical = TryEncodeBitmaskImm(value, false).has_value();
        if ((narrowLogical ? 1U : narrow.length) < (logical ? 1U : chain.length)) {
            bits = 32;
            chain = narrow;
            logical = narrowLogical;
        }
    }

    // One logical immediate beats a chain of two or more. It loses to a chain
    // of one only because MOVZ is the canonical spelling of the values that
    // both forms reach.
    const A64Reg dest = A64::Gpr(rd.code, bits);
    if (logical && chain.length > 1) {
        return OrrImm(dest, A64::Gpr(31, bits), value);
    }

    const std::uint16_t rest = chain.invert ? 0xFFFFU : 0U;
    bool rooted = false;
    for (unsigned hw = 0; hw < bits / 16U; ++hw) {
        const std::uint16_t part = Halfword(value, hw);
        if (part == rest) {
            continue;
        }
        const unsigned shift = hw * 16U;
        // MOVN writes the inverse of the halfword it is given, so the root
        // carries the complement of the value it is putting there.
        const A64Status status =
            rooted ? Movk(dest, part, shift)
                   : (chain.invert ? Movn(dest, static_cast<std::uint16_t>(~part), shift) : Movz(dest, part, shift));
        if (status != A64Status::Ok) {
            return status;
        }
        rooted = true;
    }
    if (!rooted) {
        return chain.invert ? Movn(dest, 0) : Movz(dest, 0);
    }
    return A64Status::Ok;
}

} // namespace Rux
