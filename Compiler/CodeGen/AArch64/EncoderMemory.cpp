// AArch64 encoding for loads, stores and address generation, across the scaled,
// unscaled, indexed and pair addressing forms.

#include "CodeGen/AArch64/Encoder.h"

#include <bit>

namespace Rux {
namespace {
/// A register a composite sequence can address through, or leave an address in. Neither reading of code 31 is one: the
/// zero register discards what it is given, and the stack pointer is not something a sequence may take for its own.
constexpr bool AddressableReg(const A64Reg &reg) {
    return reg.IsGeneral() && reg.bits == 64 && reg.code != 31;
}

/// A general-purpose operand of width `bits` in a field that reads code 31 as the zero register. SP has no encoding
/// there, so passing it is an error rather than a silent rename.
constexpr bool ZrOperand(const A64Reg &reg, const unsigned bits) {
    return reg.IsGeneral() && reg.bits == bits && !reg.IsStackPointer();
}

/// The same, for a field that reads code 31 as the stack pointer.
constexpr bool SpOperand(const A64Reg &reg, const unsigned bits) {
    return reg.IsGeneral() && reg.bits == bits && !reg.IsZeroReg();
}

/// Whether a signed value fits a two's-complement field `bits` wide.
constexpr bool FitsSigned(const std::int64_t value, const unsigned bits) {
    const std::int64_t limit = std::int64_t{1} << (bits - 1U);
    return value >= -limit && value < limit;
}

/// One load or store form: the `size` and `opc` fields that name it, the register file it transfers, and the width it
/// accesses — which is also the scale of a scaled offset and of a scaled index register.
struct MemForm {
    std::uint32_t size = 0;
    std::uint32_t opc = 0;
    std::uint32_t v = 0;
    std::uint32_t bytes = 1;
};

/// What a narrowing load or store does with the bits of the register above the value it moves.
enum class MemAccess : std::uint8_t {
    Store,
    Load,
    LoadSigned,
};

/// LDR / STR of a whole register: `rt` names both the width of the access and the file it lands in, and a Q access is
/// the one that borrows a bit of `opc` because `size` has no fifth value.
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

/// The narrowing and sign-extending forms, whose access width is fixed by the mnemonic. `rt` is instead the width the
/// value ends up at, which for the sign-extending loads has to be wider than what memory supplied.
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

/// Load/store register (unsigned immediate): size | 111 | V | 01 | opc | imm12 | Rn | Rt.
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

/// The two bits that tell the three immediate addressing modes of a single register apart, sitting just below the field
/// they share.
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

/// Load/store register (immediate): size | 111 | V | 00 | opc | 0 | imm9 | mode | Rn | Rt.
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

/// Load/store register (register offset): size | 111 | V | 00 | opc | 1 | Rm | option | S | 10 | Rn | Rt.
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

/// Load/store pair: opc | 101 | V | mode | L | imm7 | Rt2 | Rn | Rt. The pair forms number their widths in their own
/// `opc` field rather than sharing the `size` field of the single-register ones, and every mode writes an immediate
/// scaled by the width of one register.
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
} // namespace

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

/// Load register (literal): opc | 011 | V | 00 | imm19 | Rt.
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

A64Status A64Enc::LoadFromSymbol(const A64Reg rt, A64SymbolRef &ref, const A64Reg base) const {
    // The access is checked before the page address is emitted, so a transfer
    // register with no whole-register form leaves no half a sequence behind.
    if (!AddressableReg(base) || !WholeRegisterForm(rt, true)) {
        return A64Status::InvalidRegister;
    }
    const std::uint32_t adrp = Size();
    if (const A64Status status = Adrp(base, 0); status != A64Status::Ok) {
        return status;
    }
    const std::uint32_t lo12 = Size();
    if (const A64Status status = Ldr(rt, base, 0); status != A64Status::Ok) {
        return status;
    }
    ref = A64SymbolRef{adrp, lo12};
    return A64Status::Ok;
}

A64Status A64Enc::StoreToSymbol(const A64Reg rt, A64SymbolRef &ref, const A64Reg base) const {
    if (!AddressableReg(base) || !WholeRegisterForm(rt, false)) {
        return A64Status::InvalidRegister;
    }
    // The page address arrives in `base` one instruction before the store reads
    // it, so a store out of that same register would have overwritten the
    // address it was about to use.
    if (rt.IsGeneral() && rt.code == base.code) {
        return A64Status::InvalidRegister;
    }
    const std::uint32_t adrp = Size();
    if (const A64Status status = Adrp(base, 0); status != A64Status::Ok) {
        return status;
    }
    const std::uint32_t lo12 = Size();
    if (const A64Status status = Str(rt, base, 0); status != A64Status::Ok) {
        return status;
    }
    ref = A64SymbolRef{adrp, lo12};
    return A64Status::Ok;
}

A64Status A64Enc::ResolveMemOperand(const A64Reg base, const std::int64_t offset, const unsigned accessBytes,
                                    A64MemOperand &operand, const A64Reg scratch) const {
    if (!SpOperand(base, 64)) {
        return A64Status::InvalidRegister;
    }
    // The width scales the offset of the scaled form, so it has to be one an
    // access actually has: a power of two from a byte to a Q register.
    if (accessBytes == 0 || accessBytes > 16 || (accessBytes & (accessBytes - 1U)) != 0) {
        return A64Status::InvalidImmediate;
    }

    const auto bytes = static_cast<std::int64_t>(accessBytes);
    if (offset >= 0 && offset % bytes == 0 && offset / bytes <= 0xFFF) {
        operand = A64MemOperand{base, offset, false};
        return A64Status::Ok;
    }
    // What a negative or unaligned displacement falls back to, at every width.
    if (FitsSigned(offset, 9)) {
        operand = A64MemOperand{base, offset, true};
        return A64Status::Ok;
    }

    if (!AddressableReg(scratch) || scratch.code == base.code) {
        return A64Status::InvalidRegister;
    }
    // The whole displacement moves into the scratch register, which then
    // addresses at zero. AddSubLargeImm may want a scratch of its own, and the
    // one it is writing is free the moment it has been written.
    if (const A64Status status = AddSubLargeImm(scratch, base, offset, scratch); status != A64Status::Ok) {
        return status;
    }
    operand = A64MemOperand{scratch, 0, false};
    return A64Status::Ok;
}
} // namespace Rux
