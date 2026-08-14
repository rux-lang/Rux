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

// The `hw`-th 16-bit halfword of a value, counting from the bottom.
// A register a composite sequence can address through, or leave an address in.
// Neither reading of code 31 is one: the zero register discards what it is
// given, and the stack pointer is not something a sequence may take for its
// own.
constexpr bool AddressableReg(const A64Reg &reg) {
    return reg.IsGeneral() && reg.bits == 64 && reg.code != 31;
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

A64Status A64Enc::LoadAddress(const A64Reg rd, A64SymbolRef &ref) const {
    if (!AddressableReg(rd)) {
        return A64Status::InvalidRegister;
    }
    // Both immediates are zero here and belong to the relocations rather than
    // to the encoding, which is why the sequence is fixed at two instructions
    // whatever the symbol turns out to be.
    const std::uint32_t adrp = Size();
    if (const A64Status status = Adrp(rd, 0); status != A64Status::Ok) {
        return status;
    }
    const std::uint32_t lo12 = Size();
    if (const A64Status status = AddImm(rd, rd, 0); status != A64Status::Ok) {
        return status;
    }
    ref = A64SymbolRef{adrp, lo12};
    return A64Status::Ok;
}

A64Status A64Enc::AddSubLargeImm(const A64Reg rd, const A64Reg rn, const std::int64_t value,
                                 const A64Reg scratch) const {
    if (!SpOperand(rd, 64) || !SpOperand(rn, 64)) {
        return A64Status::InvalidRegister;
    }
    const bool subtract = value < 0;
    // Negated in unsigned arithmetic, so the most negative value has a
    // magnitude too — which it does not in two's complement.
    const std::uint64_t magnitude =
        subtract ? 0U - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);

    if (magnitude == 0) {
        return rd == rn ? A64Status::Ok : Mov(rd, rn);
    }
    if (TryEncodeArithImm12(magnitude)) {
        return subtract ? SubImm(rd, rn, magnitude) : AddImm(rd, rn, magnitude);
    }
    // The two shift positions of imm12 abut, so every value below 2^24 is the
    // sum of one immediate of each — one instruction cheaper than materializing
    // it, and costing no scratch register at all. Neither half is zero here,
    // since a value with a zero half is one the single form already reached.
    if (magnitude < (1ULL << 24U)) {
        const std::uint64_t high = magnitude & ~0xFFFULL;
        const std::uint64_t low = magnitude & 0xFFFULL;
        if (const A64Status status = subtract ? SubImm(rd, rn, high) : AddImm(rd, rn, high); status != A64Status::Ok) {
            return status;
        }
        return subtract ? SubImm(rd, rd, low) : AddImm(rd, rd, low);
    }

    // Out of reach of the immediate forms: the magnitude goes into the scratch
    // register, which must not be the source the register form has yet to read.
    if (!AddressableReg(scratch) || scratch.code == rn.code) {
        return A64Status::InvalidRegister;
    }
    if (const A64Status status = LoadImm64(scratch, magnitude); status != A64Status::Ok) {
        return status;
    }
    // The shifted-register forms read code 31 as the zero register throughout,
    // so an operand that is SP takes the extended-register form instead, whose
    // UXTX option reads the whole of the index and shifts it by nothing.
    if (rd.IsStackPointer() || rn.IsStackPointer()) {
        return subtract ? SubExt(rd, rn, scratch, A64ExtendKind::Uxtx) : AddExt(rd, rn, scratch, A64ExtendKind::Uxtx);
    }
    return subtract ? Sub(rd, rn, scratch) : Add(rd, rn, scratch);
}

A64Status A64Enc::FrameAdjust(const std::int64_t delta, const A64Reg scratch) const {
    if (delta % 16 != 0) {
        return A64Status::Unaligned;
    }
    return AddSubLargeImm(A64::Sp, A64::Sp, delta, scratch);
}

A64Status A64Enc::ProbeStack(const std::int64_t bytes) const {
    constexpr std::int64_t pageBytes = 4096;
    if (bytes < 0) {
        return A64Status::InvalidImmediate;
    }
    if (bytes % 16 != 0) {
        return A64Status::Unaligned;
    }

    std::int64_t remaining = bytes;
    while (remaining >= pageBytes) {
        if (const A64Status status = SubImm(A64::Sp, A64::Sp, pageBytes); status != A64Status::Ok) {
            return status;
        }
        if (const A64Status status = Str(A64::Xzr, A64::Sp); status != A64Status::Ok) {
            return status;
        }
        remaining -= pageBytes;
    }
    return FrameAdjust(-remaining);
}

} // namespace Rux
