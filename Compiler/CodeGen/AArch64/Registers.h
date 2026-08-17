#pragma once

// AArch64 register model and the operand vocabulary an instruction draws on:
// shift kinds, extend kinds, index modes, condition codes, barrier options and
// system registers. Pure description with no encoding logic — the encoders in
// CodeGen/AArch64/Encoder.{h,cpp} consume it, and the assembler and code
// generator build their operands from it.

#include <cstdint>

namespace Rux {
/// The register file an operand names.
enum class A64RegFile : std::uint8_t {
    General, // X (64-bit) and W (32-bit), plus the two readings of code 31
    Vector,  // V, read as B, H, S, D or Q
};

/// One register operand: the 5-bit code an instruction field carries, the file it belongs to, and the width the
/// instruction reads it at.
///
/// Code 31 names two different registers. In most fields it is the zero register (XZR / WZR); in the few that accept a
/// stack pointer it is SP / WSP. Nothing in the encoding tells them apart, so the reading travels with the operand and
/// each encoder decides which one its field allows.
struct A64Reg {
    std::uint8_t code = 0;
    A64RegFile file = A64RegFile::General;
    std::uint16_t bits = 64; // 32 or 64 for General; 8, 16, 32, 64 or 128 for Vector
    bool stackPointer = false;

    [[nodiscard]] constexpr bool IsGeneral() const noexcept {
        return file == A64RegFile::General;
    }

    [[nodiscard]] constexpr bool IsVector() const noexcept {
        return file == A64RegFile::Vector;
    }

    [[nodiscard]] constexpr bool Is64() const noexcept {
        return bits == 64;
    }

    /// The sf bit that selects the 64-bit form of a general-purpose instruction.
    [[nodiscard]] constexpr std::uint32_t Sf() const noexcept {
        return bits == 64 ? 1U : 0U;
    }

    [[nodiscard]] constexpr bool IsZeroReg() const noexcept {
        return IsGeneral() && code == 31 && !stackPointer;
    }

    [[nodiscard]] constexpr bool IsStackPointer() const noexcept {
        return IsGeneral() && code == 31 && stackPointer;
    }

    [[nodiscard]] constexpr bool operator==(const A64Reg &other) const noexcept = default;
};

/// Shift applied to the second source register of a shifted-register form, and to an immediate in the MOVZ / MOVN /
/// MOVK group. The enumerator values are the two-bit `shift` field. ROR is unavailable in the arithmetic forms.
enum class A64ShiftKind : std::uint8_t {
    Lsl = 0,
    Lsr = 1,
    Asr = 2,
    Ror = 3,
};

/// Extension applied to the second source register of an extended-register form, and to the index register of a
/// register-offset memory operand. The enumerator values are the three-bit `option` field.
enum class A64ExtendKind : std::uint8_t {
    Uxtb = 0,
    Uxth = 1,
    Uxtw = 2,
    Uxtx = 3,
    Sxtb = 4,
    Sxth = 5,
    Sxtw = 6,
    Sxtx = 7,
};

/// What a memory access does to the base register it addresses through. The three modes share one encoding page and one
/// immediate field, differing only in two bits, so they are one encoder per access rather than three; ARM spells the
/// two writeback modes with the plain LDR and STR mnemonics rather than with the LDUR and STUR that name the
/// unwritten-back form of the same field.
enum class A64IndexMode : std::uint8_t {
    Offset,    // [Xn, #imm]  — the base is read and left alone
    PostIndex, // [Xn], #imm  — the access uses the base, which is then advanced
    PreIndex,  // [Xn, #imm]! — the base is advanced first, and the access uses it
};

/// How far a memory barrier reaches and which accesses it orders. The enumerator values are the four-bit `CRm` field a
/// DMB, DSB or ISB carries; the four codes with no name here are reserved and behave as the full-system barrier the
/// domain they sit in would give.
enum class A64Barrier : std::uint8_t {
    Oshld = 1,  // outer shareable, loads
    Oshst = 2,  // outer shareable, stores
    Osh = 3,    // outer shareable, loads and stores
    Nshld = 5,  // non-shareable, loads
    Nshst = 6,  // non-shareable, stores
    Nsh = 7,    // non-shareable, loads and stores
    Ishld = 9,  // inner shareable, loads
    Ishst = 10, // inner shareable, stores
    Ish = 11,   // inner shareable, loads and stores
    Ld = 13,    // full system, loads
    St = 14,    // full system, stores
    Sy = 15,    // full system, loads and stores
};

/// Condition codes, in the order the four-bit `cond` field encodes them.
enum class A64Condition : std::uint8_t {
    Eq = 0,  // equal
    Ne = 1,  // not equal
    Cs = 2,  // carry set, unsigned higher or same
    Cc = 3,  // carry clear, unsigned lower
    Mi = 4,  // negative
    Pl = 5,  // positive or zero
    Vs = 6,  // signed overflow
    Vc = 7,  // no signed overflow
    Hi = 8,  // unsigned higher
    Ls = 9,  // unsigned lower or same
    Ge = 10, // signed greater than or equal
    Lt = 11, // signed less than
    Gt = 12, // signed greater than
    Le = 13, // signed less than or equal
    Al = 14, // always
    Nv = 15, // always; the encoding exists but carries no meaning of its own
};

// Registers, register factories and condition helpers.
//
// Registers are built from a number rather than named one at a time: an
// instruction selector almost always has an index in hand, and the names worth
// spelling out are the reserved ones. `Xn(31)` and `Wn(31)` are the zero
// registers; the stack pointer is only reachable through `Sp` and `Wsp`.
namespace A64 {
/// General-purpose register Xn, 64-bit. X31 is XZR.
[[nodiscard]] constexpr A64Reg Xn(const unsigned n) noexcept {
    return A64Reg{static_cast<std::uint8_t>(n & 31U), A64RegFile::General, 64, false};
}

/// General-purpose register Wn, the 32-bit view of Xn. W31 is WZR.
[[nodiscard]] constexpr A64Reg Wn(const unsigned n) noexcept {
    return A64Reg{static_cast<std::uint8_t>(n & 31U), A64RegFile::General, 32, false};
}

/// General-purpose register n at the width `bits` selects: 64 for Xn, 32 for Wn.
[[nodiscard]] constexpr A64Reg Gpr(const unsigned n, const unsigned bits) noexcept {
    return bits == 64 ? Xn(n) : Wn(n);
}

/// Vector register Vn read at `bits` wide: the B, H, S, D and Q views.
[[nodiscard]] constexpr A64Reg Vn(const unsigned n, const unsigned bits) noexcept {
    return A64Reg{static_cast<std::uint8_t>(n & 31U), A64RegFile::Vector, static_cast<std::uint16_t>(bits), false};
}

[[nodiscard]] constexpr A64Reg Bn(const unsigned n) noexcept {
    return Vn(n, 8);
}

[[nodiscard]] constexpr A64Reg Hn(const unsigned n) noexcept {
    return Vn(n, 16);
}

[[nodiscard]] constexpr A64Reg Sn(const unsigned n) noexcept {
    return Vn(n, 32);
}

[[nodiscard]] constexpr A64Reg Dn(const unsigned n) noexcept {
    return Vn(n, 64);
}

[[nodiscard]] constexpr A64Reg Qn(const unsigned n) noexcept {
    return Vn(n, 128);
}

/// The two readings of code 31.
inline constexpr A64Reg Xzr = Xn(31);
inline constexpr A64Reg Wzr = Wn(31);
inline constexpr A64Reg Sp = A64Reg{31, A64RegFile::General, 64, true};
inline constexpr A64Reg Wsp = A64Reg{31, A64RegFile::General, 32, true};

/// Reserved registers, named for the role AAPCS64 gives them.
///
/// X16 and X17 are the intra-procedure-call scratch registers: a linker may clobber them in a veneer, so nothing may
/// hold a value in them across a call. The composite sequences in Encoder.h take X16 when an operand does not encode.
/// X18 is the platform register, reserved on every supported system and never allocated. X29 and X30 hold the frame
/// chain.
inline constexpr A64Reg Ip0 = Xn(16);
inline constexpr A64Reg Ip1 = Xn(17);
inline constexpr A64Reg PlatformReg = Xn(18);
inline constexpr A64Reg Fp = Xn(29);
inline constexpr A64Reg Lr = Xn(30);

/// The 15-bit system-register encoding an MRS or MSR carries, assembled from the five fields ARM names a system
/// register by. The high bit of `op0` is one for every register these instructions reach, so it is the low bit that
/// travels and the field is 15 bits rather than 16.
[[nodiscard]] constexpr std::uint16_t SysReg(const unsigned op0, const unsigned op1, const unsigned crn,
                                             const unsigned crm, const unsigned op2) noexcept {
    return static_cast<std::uint16_t>((op0 & 1U) << 14U | (op1 & 7U) << 11U | (crn & 15U) << 7U | (crm & 15U) << 3U |
                                      (op2 & 7U));
}

/// The two system registers the back end needs: the condition flags, which a context switch of the flag state has to
/// move through a register, and the thread pointer, which is where thread-local storage begins.
inline constexpr std::uint16_t Nzcv = SysReg(3, 3, 4, 2, 0);
inline constexpr std::uint16_t TpidrEl0 = SysReg(3, 3, 13, 0, 2);

/// Condition-code spellings that name the same encoding.
inline constexpr A64Condition Hs = A64Condition::Cs;
inline constexpr A64Condition Lo = A64Condition::Cc;

/// The condition that holds exactly when `cond` does not. Every pair differs in the low bit, so AL inverts to NV: a
/// branch must never be widened through that pair, since NV is "always" rather than "never".
[[nodiscard]] constexpr A64Condition InvertCondition(const A64Condition cond) noexcept {
    return static_cast<A64Condition>(static_cast<std::uint8_t>(cond) ^ 1U);
}
} // namespace A64
} // namespace Rux
