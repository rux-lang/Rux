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

// One load or store form: the `size` and `opc` fields that name it, the
// register file it transfers, and the width it accesses — which is also the
// scale of a scaled offset and of a scaled index register.
struct MemForm {
    std::uint32_t size = 0;
    std::uint32_t opc = 0;
    std::uint32_t v = 0;
    std::uint32_t bytes = 1;
};

// What a narrowing load or store does with the bits of the register above the
// value it moves.
enum class MemAccess : std::uint8_t {
    Store,
    Load,
    LoadSigned,
};

// LDR / STR of a whole register: `rt` names both the width of the access and
// the file it lands in, and a Q access is the one that borrows a bit of `opc`
// because `size` has no fifth value.
std::optional<MemForm> WholeRegisterForm(const A64Reg rt, const bool load) {
    const std::uint32_t opc = load ? 1U : 0U;
    if (rt.IsGeneral()) {
        if (rt.IsStackPointer()) {
            return std::nullopt;
        }
        if (rt.bits == 64) {
            return MemForm{3, opc, 0, 8};
        }
        if (rt.bits == 32) {
            return MemForm{2, opc, 0, 4};
        }
        return std::nullopt;
    }
    switch (rt.bits) {
    case 8:
        return MemForm{0, opc, 1, 1};
    case 16:
        return MemForm{1, opc, 1, 2};
    case 32:
        return MemForm{2, opc, 1, 4};
    case 64:
        return MemForm{3, opc, 1, 8};
    case 128:
        return MemForm{0, load ? 3U : 2U, 1, 16};
    default:
        return std::nullopt;
    }
}

// The narrowing and sign-extending forms, whose access width is fixed by the
// mnemonic. `rt` is instead the width the value ends up at, which for the
// sign-extending loads has to be wider than what memory supplied.
std::optional<MemForm> NarrowForm(const A64Reg rt, const std::uint32_t size, const MemAccess access) {
    if (!rt.IsGeneral() || rt.IsStackPointer() || (rt.bits != 32 && rt.bits != 64)) {
        return std::nullopt;
    }
    const std::uint32_t bytes = 1U << size;
    if (access == MemAccess::LoadSigned) {
        if (rt.bits <= bytes * 8U) {
            return std::nullopt;
        }
        // opc 10 extends to a doubleword, opc 11 to a word.
        return MemForm{size, rt.bits == 64 ? 2U : 3U, 0, bytes};
    }
    // A narrowing load or store names the register it truncates as a W one,
    // whatever the width of the value the front end had in it.
    if (rt.bits != 32) {
        return std::nullopt;
    }
    return MemForm{size, access == MemAccess::Load ? 1U : 0U, 0, bytes};
}

// Load/store register (unsigned immediate):
// size | 111 | V | 01 | opc | imm12 | Rn | Rt.
A64Status EncodeMemUnsigned(const A64Enc &enc, const std::optional<MemForm> &form, const A64Reg rt, const A64Reg rn,
                            const std::uint64_t offset) {
    if (!form || !SpOperand(rn, 64)) {
        return A64Status::InvalidRegister;
    }
    if (offset % form->bytes != 0) {
        return A64Status::Unaligned;
    }
    const std::uint64_t imm = offset / form->bytes;
    if (imm > 0xFFFU) {
        return A64Status::OutOfRange;
    }
    enc.Word(form->size << 30U | 0x39U << 24U | form->v << 26U | form->opc << 22U |
             static_cast<std::uint32_t>(imm) << 10U | std::uint32_t{rn.code} << 5U | rt.code);
    return A64Status::Ok;
}

// The two bits that tell the three immediate addressing modes of a single
// register apart, sitting just below the field they share.
constexpr std::uint32_t IndexModeBits(const A64IndexMode mode) {
    switch (mode) {
    case A64IndexMode::PostIndex:
        return 1;
    case A64IndexMode::PreIndex:
        return 3;
    case A64IndexMode::Offset:
        break;
    }
    return 0;
}

// Load/store register (immediate): size | 111 | V | 00 | opc | 0 | imm9 | mode | Rn | Rt.
A64Status EncodeMemImm9(const A64Enc &enc, const std::optional<MemForm> &form, const A64Reg rt, const A64Reg rn,
                        const std::int64_t offset, const A64IndexMode mode) {
    if (!form || !SpOperand(rn, 64)) {
        return A64Status::InvalidRegister;
    }
    if (!FitsSigned(offset, 9)) {
        return A64Status::OutOfRange;
    }
    enc.Word(form->size << 30U | 0x38U << 24U | form->v << 26U | form->opc << 22U |
             static_cast<std::uint32_t>(offset & 0x1FF) << 12U | IndexModeBits(mode) << 10U |
             std::uint32_t{rn.code} << 5U | rt.code);
    return A64Status::Ok;
}

// Load/store register (register offset):
// size | 111 | V | 00 | opc | 1 | Rm | option | S | 10 | Rn | Rt.
A64Status EncodeMemReg(const A64Enc &enc, const std::optional<MemForm> &form, const A64Reg rt, const A64Reg rn,
                       const A64Reg rm, const A64ExtendKind extend, const unsigned amount) {
    if (!form || !SpOperand(rn, 64)) {
        return A64Status::InvalidRegister;
    }
    // An index is a word or a doubleword; the byte and halfword extensions have
    // no encoding here, since nothing addresses memory eight bits at a time.
    const bool doubleword = extend == A64ExtendKind::Uxtx || extend == A64ExtendKind::Sxtx;
    if (!doubleword && extend != A64ExtendKind::Uxtw && extend != A64ExtendKind::Sxtw) {
        return A64Status::InvalidImmediate;
    }
    if (!ZrOperand(rm, doubleword ? 64U : 32U)) {
        return A64Status::InvalidRegister;
    }
    // The instruction carries one scale bit, so the shift is either absent or
    // exactly the one that turns an element index into a byte offset.
    if (amount != 0 && amount != static_cast<unsigned>(std::countr_zero(form->bytes))) {
        return A64Status::InvalidImmediate;
    }
    enc.Word(form->size << 30U | 0x38U << 24U | form->v << 26U | form->opc << 22U | 1U << 21U |
             std::uint32_t{rm.code} << 16U | static_cast<std::uint32_t>(extend) << 13U |
             (amount != 0 ? 1U : 0U) << 12U | 2U << 10U | std::uint32_t{rn.code} << 5U | rt.code);
    return A64Status::Ok;
}

// Load/store pair: opc | 101 | V | mode | L | imm7 | Rt2 | Rn | Rt. The pair
// forms number their widths in their own `opc` field rather than sharing the
// `size` field of the single-register ones, and every mode writes an immediate
// scaled by the width of one register.
A64Status EncodeMemPair(const A64Enc &enc, const A64Reg rt, const A64Reg rt2, const A64Reg rn,
                        const std::int64_t offset, const A64IndexMode mode, const bool load) {
    if (!SpOperand(rn, 64) || rt.file != rt2.file || rt.bits != rt2.bits) {
        return A64Status::InvalidRegister;
    }
    std::uint32_t opc = 0;
    std::uint32_t v = 0;
    std::int64_t bytes = 0;
    if (rt.IsGeneral()) {
        if (!ZrOperand(rt, rt.bits) || !ZrOperand(rt2, rt2.bits)) {
            return A64Status::InvalidRegister;
        }
        if (rt.bits != 32 && rt.bits != 64) {
            return A64Status::InvalidRegister;
        }
        opc = rt.bits == 64 ? 2U : 0U;
        bytes = rt.bits / 8;
    }
    else {
        switch (rt.bits) {
        case 32:
            opc = 0;
            break;
        case 64:
            opc = 1;
            break;
        case 128:
            opc = 2;
            break;
        default:
            return A64Status::InvalidRegister;
        }
        v = 1;
        bytes = rt.bits / 8;
    }
    if (offset % bytes != 0) {
        return A64Status::Unaligned;
    }
    const std::int64_t imm = offset / bytes;
    if (!FitsSigned(imm, 7)) {
        return A64Status::OutOfRange;
    }
    // The pair modes sit in three bits of their own, with the unwritten-back
    // form in the middle rather than at zero.
    const std::uint32_t modeBits = mode == A64IndexMode::Offset ? 2U : (mode == A64IndexMode::PostIndex ? 1U : 3U);
    enc.Word(opc << 30U | 0x05U << 27U | v << 26U | modeBits << 23U | (load ? 1U : 0U) << 22U |
             static_cast<std::uint32_t>(imm & 0x7F) << 15U | std::uint32_t{rt2.code} << 10U |
             std::uint32_t{rn.code} << 5U | rt.code);
    return A64Status::Ok;
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

// Exception generation: 11010100 | opc | imm16 | op2 | LL. Each instruction
// fixes both trailing fields, so the two of them travel together as `tail`.
A64Status EncodeException(const A64Enc &enc, const std::uint16_t imm16, const unsigned opc, const unsigned tail) {
    enc.Word(0xD4000000U | opc << 21U | std::uint32_t{imm16} << 5U | tail);
    return A64Status::Ok;
}

// Hints and barriers: 11010101 | 00000011 | 0011 | CRm | op2 | 11111, with the
// hint page sitting at CRn 0010 and the barrier page at 0011.
A64Status EncodeSystemNoReg(const A64Enc &enc, const unsigned crn, const unsigned crm, const unsigned op2) {
    enc.Word(0xD5030000U | crn << 12U | crm << 8U | op2 << 5U | 0x1FU);
    return A64Status::Ok;
}

// MRS / MSR (register): 1101010100 | L | 1 | o0 | op1 | CRn | CRm | op2 | Rt,
// which is the 15-bit system-register encoding laid down whole.
A64Status EncodeSysRegMove(const A64Enc &enc, const A64Reg rt, const std::uint16_t sysreg, const bool read) {
    if (!ZrOperand(rt, 64)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(0xD5100000U | (read ? 1U : 0U) << 21U | (std::uint32_t{sysreg} & 0x7FFFU) << 5U | rt.code);
    return A64Status::Ok;
}

// The two-bit `ptype` field naming the precision a floating-point instruction
// works at. Half precision has an encoding here, but it needs an architectural
// extension and no Rux type reaches it, so it is left unallocated and a B, H or
// Q operand has no form at all.
constexpr std::optional<std::uint32_t> FpType(const A64Reg &reg) {
    if (!reg.IsVector()) {
        return std::nullopt;
    }
    switch (reg.bits) {
    case 32:
        return 0U;
    case 64:
        return 1U;
    default:
        return std::nullopt;
    }
}

// A floating-point operand at the width the instruction works at. The vector
// file has no second reading of any register code, so unlike ZrOperand and
// SpOperand this is a width check and nothing more.
constexpr bool FpOperand(const A64Reg &reg, const unsigned bits) {
    return reg.IsVector() && reg.bits == bits;
}

// Floating-point data processing (1 source):
// M | 0 | S | 11110 | ptype | 1 | opcode | 10000 | Rn | Rd. `ptype` is the
// precision read, which for everything but FCVT is also the one written.
void EncodeFpDataProc1(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const std::uint32_t type,
                       const unsigned opcode) {
    enc.Word(0x1E204000U | type << 22U | opcode << 15U | std::uint32_t{rn.code} << 5U | rd.code);
}

// The one-source operations that read and write the same precision.
A64Status EncodeFpUnary(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const unsigned opcode) {
    const std::optional<std::uint32_t> type = FpType(rd);
    if (!type || !FpOperand(rn, rd.bits)) {
        return A64Status::InvalidRegister;
    }
    EncodeFpDataProc1(enc, rd, rn, *type, opcode);
    return A64Status::Ok;
}

// Floating-point data processing (2 source):
// M | 0 | S | 11110 | ptype | 1 | Rm | opcode | 10 | Rn | Rd.
A64Status EncodeFpDataProc2(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const A64Reg rm,
                            const unsigned opcode) {
    const std::optional<std::uint32_t> type = FpType(rd);
    if (!type || !FpOperand(rn, rd.bits) || !FpOperand(rm, rd.bits)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(0x1E200800U | *type << 22U | std::uint32_t{rm.code} << 16U | opcode << 12U | std::uint32_t{rn.code} << 5U |
             rd.code);
    return A64Status::Ok;
}

// Floating-point data processing (3 source):
// M | 0 | S | 11111 | ptype | o1 | Rm | o0 | Ra | Rn | Rd. `o1` negates the
// result and `o0` the product, which is what tells the four fused
// multiply-adds apart.
A64Status EncodeFpDataProc3(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra,
                            const bool negateResult, const bool negateProduct) {
    const std::optional<std::uint32_t> type = FpType(rd);
    if (!type || !FpOperand(rn, rd.bits) || !FpOperand(rm, rd.bits) || !FpOperand(ra, rd.bits)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(0x1F000000U | *type << 22U | (negateResult ? 1U : 0U) << 21U | std::uint32_t{rm.code} << 16U |
             (negateProduct ? 1U : 0U) << 15U | std::uint32_t{ra.code} << 10U | std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

// Floating-point compare: M | 0 | S | 11110 | ptype | 1 | Rm | 00 | 1000 | Rn |
// opcode2. The zero forms say so in `opcode2` and leave `Rm` empty, since the
// value they compare against is part of the instruction rather than an operand.
A64Status EncodeFpCompare(const A64Enc &enc, const A64Reg rn, const A64Reg rm, const bool zero, const bool signalling) {
    const std::optional<std::uint32_t> type = FpType(rn);
    if (!type || (!zero && !FpOperand(rm, rn.bits))) {
        return A64Status::InvalidRegister;
    }
    enc.Word(0x1E202000U | *type << 22U | (zero ? 0U : std::uint32_t{rm.code}) << 16U | std::uint32_t{rn.code} << 5U |
             (signalling ? 16U : 0U) | (zero ? 8U : 0U));
    return A64Status::Ok;
}

// Conversion between floating point and integer:
// sf | 0 | S | 11110 | ptype | 1 | rmode | opcode | 000000 | Rn | Rd. The
// integer width comes from the general-purpose register and the precision from
// the vector one, so the two are independent. Which operand is which is fixed
// by the instruction rather than read off the registers, so writing one
// backwards is refused instead of encoding the opposite conversion.
A64Status EncodeFpIntConv(const A64Enc &enc, const A64Reg rd, const A64Reg rn, const bool toGeneral,
                          const unsigned rmode, const unsigned opcode) {
    const A64Reg gpr = toGeneral ? rd : rn;
    const std::optional<std::uint32_t> type = FpType(toGeneral ? rn : rd);
    if (!type || !ZrOperand(gpr, gpr.bits) || (gpr.bits != 32 && gpr.bits != 64)) {
        return A64Status::InvalidRegister;
    }
    enc.Word(0x1E200000U | gpr.Sf() << 31U | *type << 22U | rmode << 19U | opcode << 16U |
             std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
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

std::optional<std::uint8_t> TryEncodeFpImm8(const double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const std::uint64_t sign = bits >> 63U;
    const std::uint64_t exponent = (bits >> 52U) & 0x7FFU;
    const std::uint64_t fraction = bits & LowMask(52);

    // The immediate supplies the top four bits of the fraction and nothing
    // below them.
    if ((fraction & LowMask(48)) != 0) {
        return std::nullopt;
    }
    // The exponent is one bit written twice over, inverted once and repeated
    // eight times, followed by two bits of the immediate. That pattern is what
    // confines the value to eight binades around one, and what leaves zero,
    // the infinities and the NaNs outside the set.
    const std::uint64_t repeated = (exponent >> 9U) & 1U;
    if (((exponent >> 10U) & 1U) == repeated || ((exponent >> 2U) & 0xFFU) != (repeated != 0 ? 0xFFU : 0U)) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(sign << 7U | repeated << 6U | (exponent & 3U) << 4U | (fraction >> 48U));
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

A64Status A64Enc::Ldr(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, WholeRegisterForm(rt, true), rt, rn, offset);
}

A64Status A64Enc::Str(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, WholeRegisterForm(rt, false), rt, rn, offset);
}

A64Status A64Enc::Ldrb(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, NarrowForm(rt, 0, MemAccess::Load), rt, rn, offset);
}

A64Status A64Enc::Strb(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, NarrowForm(rt, 0, MemAccess::Store), rt, rn, offset);
}

A64Status A64Enc::Ldrh(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, NarrowForm(rt, 1, MemAccess::Load), rt, rn, offset);
}

A64Status A64Enc::Strh(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, NarrowForm(rt, 1, MemAccess::Store), rt, rn, offset);
}

A64Status A64Enc::Ldrsb(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, NarrowForm(rt, 0, MemAccess::LoadSigned), rt, rn, offset);
}

A64Status A64Enc::Ldrsh(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, NarrowForm(rt, 1, MemAccess::LoadSigned), rt, rn, offset);
}

A64Status A64Enc::Ldrsw(const A64Reg rt, const A64Reg rn, const std::uint64_t offset) const {
    return EncodeMemUnsigned(*this, NarrowForm(rt, 2, MemAccess::LoadSigned), rt, rn, offset);
}

A64Status A64Enc::Ldur(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, WholeRegisterForm(rt, true), rt, rn, offset, mode);
}

A64Status A64Enc::Stur(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, WholeRegisterForm(rt, false), rt, rn, offset, mode);
}

A64Status A64Enc::Ldurb(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, NarrowForm(rt, 0, MemAccess::Load), rt, rn, offset, mode);
}

A64Status A64Enc::Sturb(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, NarrowForm(rt, 0, MemAccess::Store), rt, rn, offset, mode);
}

A64Status A64Enc::Ldurh(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, NarrowForm(rt, 1, MemAccess::Load), rt, rn, offset, mode);
}

A64Status A64Enc::Sturh(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, NarrowForm(rt, 1, MemAccess::Store), rt, rn, offset, mode);
}

A64Status A64Enc::Ldursb(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, NarrowForm(rt, 0, MemAccess::LoadSigned), rt, rn, offset, mode);
}

A64Status A64Enc::Ldursh(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, NarrowForm(rt, 1, MemAccess::LoadSigned), rt, rn, offset, mode);
}

A64Status A64Enc::Ldursw(const A64Reg rt, const A64Reg rn, const std::int64_t offset, const A64IndexMode mode) const {
    return EncodeMemImm9(*this, NarrowForm(rt, 2, MemAccess::LoadSigned), rt, rn, offset, mode);
}

A64Status A64Enc::LdrReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                         const unsigned amount) const {
    return EncodeMemReg(*this, WholeRegisterForm(rt, true), rt, rn, rm, extend, amount);
}

A64Status A64Enc::StrReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                         const unsigned amount) const {
    return EncodeMemReg(*this, WholeRegisterForm(rt, false), rt, rn, rm, extend, amount);
}

A64Status A64Enc::LdrbReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                          const unsigned amount) const {
    return EncodeMemReg(*this, NarrowForm(rt, 0, MemAccess::Load), rt, rn, rm, extend, amount);
}

A64Status A64Enc::StrbReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                          const unsigned amount) const {
    return EncodeMemReg(*this, NarrowForm(rt, 0, MemAccess::Store), rt, rn, rm, extend, amount);
}

A64Status A64Enc::LdrhReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                          const unsigned amount) const {
    return EncodeMemReg(*this, NarrowForm(rt, 1, MemAccess::Load), rt, rn, rm, extend, amount);
}

A64Status A64Enc::StrhReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                          const unsigned amount) const {
    return EncodeMemReg(*this, NarrowForm(rt, 1, MemAccess::Store), rt, rn, rm, extend, amount);
}

A64Status A64Enc::LdrsbReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                           const unsigned amount) const {
    return EncodeMemReg(*this, NarrowForm(rt, 0, MemAccess::LoadSigned), rt, rn, rm, extend, amount);
}

A64Status A64Enc::LdrshReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                           const unsigned amount) const {
    return EncodeMemReg(*this, NarrowForm(rt, 1, MemAccess::LoadSigned), rt, rn, rm, extend, amount);
}

A64Status A64Enc::LdrswReg(const A64Reg rt, const A64Reg rn, const A64Reg rm, const A64ExtendKind extend,
                           const unsigned amount) const {
    return EncodeMemReg(*this, NarrowForm(rt, 2, MemAccess::LoadSigned), rt, rn, rm, extend, amount);
}

A64Status A64Enc::Ldp(const A64Reg rt, const A64Reg rt2, const A64Reg rn, const std::int64_t offset,
                      const A64IndexMode mode) const {
    return EncodeMemPair(*this, rt, rt2, rn, offset, mode, true);
}

A64Status A64Enc::Stp(const A64Reg rt, const A64Reg rt2, const A64Reg rn, const std::int64_t offset,
                      const A64IndexMode mode) const {
    return EncodeMemPair(*this, rt, rt2, rn, offset, mode, false);
}

// Load register (literal): opc | 011 | V | 00 | imm19 | Rt.
A64Status A64Enc::LdrLiteral(const A64Reg rt, const std::int64_t offset) const {
    std::uint32_t opc = 0;
    std::uint32_t v = 0;
    if (rt.IsGeneral()) {
        if (!ZrOperand(rt, rt.bits) || (rt.bits != 32 && rt.bits != 64)) {
            return A64Status::InvalidRegister;
        }
        opc = rt.bits == 64 ? 1U : 0U;
    }
    else {
        switch (rt.bits) {
        case 32:
            opc = 0;
            break;
        case 64:
            opc = 1;
            break;
        case 128:
            opc = 2;
            break;
        default:
            return A64Status::InvalidRegister;
        }
        v = 1;
    }
    // The immediate counts instructions, so a target that is not one away is
    // not reachable at all rather than reachable to the nearest word.
    if (offset % A64Enc::InstrSize != 0) {
        return A64Status::Unaligned;
    }
    const std::int64_t imm = offset / A64Enc::InstrSize;
    if (!FitsSigned(imm, 19)) {
        return A64Status::OutOfRange;
    }
    Word(opc << 30U | 0x18U << 24U | v << 26U | static_cast<std::uint32_t>(imm & 0x7FFFF) << 5U | rt.code);
    return A64Status::Ok;
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

A64Status A64Enc::Svc(const std::uint16_t imm16) const {
    return EncodeException(*this, imm16, 0, 1);
}

A64Status A64Enc::Brk(const std::uint16_t imm16) const {
    return EncodeException(*this, imm16, 1, 0);
}

A64Status A64Enc::Hlt(const std::uint16_t imm16) const {
    return EncodeException(*this, imm16, 2, 0);
}

// UDF: 0000000000000000 | imm16. The whole of the rest of the word is zero,
// which is what makes a page of zeroed memory trap on its first instruction.
A64Status A64Enc::Udf(const std::uint16_t imm16) const {
    Word(imm16);
    return A64Status::Ok;
}

A64Status A64Enc::Nop() const {
    return Hint(0);
}

A64Status A64Enc::Hint(const unsigned imm7) const {
    if (imm7 > 0x7FU) {
        return A64Status::InvalidImmediate;
    }
    return EncodeSystemNoReg(*this, 2, imm7 >> 3U, imm7 & 7U);
}

A64Status A64Enc::Dsb(const A64Barrier option) const {
    return EncodeSystemNoReg(*this, 3, static_cast<unsigned>(option), 4);
}

A64Status A64Enc::Dmb(const A64Barrier option) const {
    return EncodeSystemNoReg(*this, 3, static_cast<unsigned>(option), 5);
}

A64Status A64Enc::Isb(const A64Barrier option) const {
    return EncodeSystemNoReg(*this, 3, static_cast<unsigned>(option), 6);
}

A64Status A64Enc::Mrs(const A64Reg rt, const std::uint16_t sysreg) const {
    return EncodeSysRegMove(*this, rt, sysreg, true);
}

A64Status A64Enc::Msr(const std::uint16_t sysreg, const A64Reg rt) const {
    return EncodeSysRegMove(*this, rt, sysreg, false);
}

A64Status A64Enc::Fmov(const A64Reg rd, const A64Reg rn) const {
    if (rd.IsVector() && rn.IsVector()) {
        return EncodeFpUnary(*this, rd, rn, 0x00);
    }
    // The transfers to and from the general-purpose file move the bits of a
    // value rather than the value itself, so the two registers hold the same
    // number of them: a word pairs with an S register and a doubleword with a
    // D one, and every other pairing is unallocated.
    if (rd.IsVector() == rn.IsVector() || rd.bits != rn.bits) {
        return A64Status::InvalidRegister;
    }
    return EncodeFpIntConv(*this, rd, rn, !rd.IsVector(), 0, rd.IsVector() ? 7U : 6U);
}

// FMOV (scalar, immediate): M | 0 | S | 11110 | ptype | 1 | imm8 | 100 | 00000 | Rd.
A64Status A64Enc::FmovImm(const A64Reg rd, const double value) const {
    const std::optional<std::uint32_t> type = FpType(rd);
    if (!type) {
        return A64Status::InvalidRegister;
    }
    const std::optional<std::uint8_t> imm8 = TryEncodeFpImm8(value);
    if (!imm8) {
        return A64Status::InvalidImmediate;
    }
    Word(0x1E201000U | *type << 22U | std::uint32_t{*imm8} << 13U | rd.code);
    return A64Status::Ok;
}

A64Status A64Enc::Fadd(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeFpDataProc2(*this, rd, rn, rm, 0x2);
}

A64Status A64Enc::Fsub(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeFpDataProc2(*this, rd, rn, rm, 0x3);
}

A64Status A64Enc::Fmul(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeFpDataProc2(*this, rd, rn, rm, 0x0);
}

A64Status A64Enc::Fdiv(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeFpDataProc2(*this, rd, rn, rm, 0x1);
}

A64Status A64Enc::Fmax(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeFpDataProc2(*this, rd, rn, rm, 0x4);
}

A64Status A64Enc::Fmin(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeFpDataProc2(*this, rd, rn, rm, 0x5);
}

A64Status A64Enc::Fmaxnm(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeFpDataProc2(*this, rd, rn, rm, 0x6);
}

A64Status A64Enc::Fminnm(const A64Reg rd, const A64Reg rn, const A64Reg rm) const {
    return EncodeFpDataProc2(*this, rd, rn, rm, 0x7);
}

A64Status A64Enc::Fabs(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpUnary(*this, rd, rn, 0x01);
}

A64Status A64Enc::Fneg(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpUnary(*this, rd, rn, 0x02);
}

A64Status A64Enc::Fsqrt(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpUnary(*this, rd, rn, 0x03);
}

A64Status A64Enc::Frintn(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpUnary(*this, rd, rn, 0x08);
}

A64Status A64Enc::Frintp(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpUnary(*this, rd, rn, 0x09);
}

A64Status A64Enc::Frintm(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpUnary(*this, rd, rn, 0x0A);
}

A64Status A64Enc::Frintz(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpUnary(*this, rd, rn, 0x0B);
}

A64Status A64Enc::Frinta(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpUnary(*this, rd, rn, 0x0C);
}

A64Status A64Enc::Fmadd(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra) const {
    return EncodeFpDataProc3(*this, rd, rn, rm, ra, false, false);
}

A64Status A64Enc::Fmsub(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra) const {
    return EncodeFpDataProc3(*this, rd, rn, rm, ra, false, true);
}

A64Status A64Enc::Fnmadd(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra) const {
    return EncodeFpDataProc3(*this, rd, rn, rm, ra, true, false);
}

A64Status A64Enc::Fnmsub(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Reg ra) const {
    return EncodeFpDataProc3(*this, rd, rn, rm, ra, true, true);
}

A64Status A64Enc::Fcmp(const A64Reg rn, const A64Reg rm) const {
    return EncodeFpCompare(*this, rn, rm, false, false);
}

A64Status A64Enc::Fcmpe(const A64Reg rn, const A64Reg rm) const {
    return EncodeFpCompare(*this, rn, rm, false, true);
}

A64Status A64Enc::FcmpZero(const A64Reg rn) const {
    return EncodeFpCompare(*this, rn, rn, true, false);
}

A64Status A64Enc::FcmpeZero(const A64Reg rn) const {
    return EncodeFpCompare(*this, rn, rn, true, true);
}

// Floating-point conditional compare:
// M | 0 | S | 11110 | ptype | 1 | Rm | cond | 01 | Rn | op | nzcv.
A64Status A64Enc::Fccmp(const A64Reg rn, const A64Reg rm, const unsigned nzcv, const A64Condition cond) const {
    const std::optional<std::uint32_t> type = FpType(rn);
    if (!type || !FpOperand(rm, rn.bits)) {
        return A64Status::InvalidRegister;
    }
    if (nzcv > 0xFU) {
        return A64Status::InvalidImmediate;
    }
    Word(0x1E200400U | *type << 22U | std::uint32_t{rm.code} << 16U | static_cast<std::uint32_t>(cond) << 12U |
         std::uint32_t{rn.code} << 5U | nzcv);
    return A64Status::Ok;
}

// Floating-point conditional select:
// M | 0 | S | 11110 | ptype | 1 | Rm | cond | 11 | Rn | Rd.
A64Status A64Enc::Fcsel(const A64Reg rd, const A64Reg rn, const A64Reg rm, const A64Condition cond) const {
    const std::optional<std::uint32_t> type = FpType(rd);
    if (!type || !FpOperand(rn, rd.bits) || !FpOperand(rm, rd.bits)) {
        return A64Status::InvalidRegister;
    }
    Word(0x1E200C00U | *type << 22U | std::uint32_t{rm.code} << 16U | static_cast<std::uint32_t>(cond) << 12U |
         std::uint32_t{rn.code} << 5U | rd.code);
    return A64Status::Ok;
}

A64Status A64Enc::Fcvt(const A64Reg rd, const A64Reg rn) const {
    const std::optional<std::uint32_t> to = FpType(rd);
    const std::optional<std::uint32_t> from = FpType(rn);
    if (!to || !from || rd.bits == rn.bits) {
        return A64Status::InvalidRegister;
    }
    // The destination precision sits in the low two bits of the opcode and the
    // source precision in the type field, so a conversion between two registers
    // of one precision would name the encoding of no instruction at all.
    EncodeFpDataProc1(*this, rd, rn, *from, 0x04U | *to);
    return A64Status::Ok;
}

A64Status A64Enc::Fcvtzs(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpIntConv(*this, rd, rn, true, 3, 0);
}

A64Status A64Enc::Fcvtzu(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpIntConv(*this, rd, rn, true, 3, 1);
}

A64Status A64Enc::Scvtf(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpIntConv(*this, rd, rn, false, 0, 2);
}

A64Status A64Enc::Ucvtf(const A64Reg rd, const A64Reg rn) const {
    return EncodeFpIntConv(*this, rd, rn, false, 0, 3);
}
} // namespace Rux
