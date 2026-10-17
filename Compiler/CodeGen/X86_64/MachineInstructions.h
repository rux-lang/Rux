#pragma once

// x86-64 opcodes, registers and symbol references of the machine IR (`CodeGen/Machine/MachineIr.h`).
//
// An instruction's `size` is its operand width in bytes (1, 2, 4 or 8; 4 or 8 for scalar floating point). Integer
// registers hold a value of a narrower type in their low bits; the bits above are unspecified unless the type is a
// bool, which is always held as exactly 0 or 1. Selection extends a value where an operation reads its full width.

#include "CodeGen/Machine/MachineIr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux::X86_64Machine {
using Machine::Reg;

enum : Reg {
    Rax = 0,
    Rcx,
    Rdx,
    Rbx,
    Rsp,
    Rbp,
    Rsi,
    Rdi,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
    Xmm0 = 16,
};

[[nodiscard]] constexpr bool IsXmm(const Reg reg) noexcept {
    return reg >= Xmm0 && reg < Xmm0 + 16;
}

/// The hardware number of a register within its file, as ModRM, SIB and REX encode it.
[[nodiscard]] constexpr std::uint8_t HardwareNumber(const Reg reg) noexcept {
    return static_cast<std::uint8_t>(IsXmm(reg) ? reg - Xmm0 : reg);
}

/// The full condition-code nibble.
enum class Cond : std::uint8_t {
    Overflow = 0,
    NoOverflow = 1,
    Below = 2,
    AboveEqual = 3,
    Equal = 4,
    NotEqual = 5,
    BelowEqual = 6,
    Above = 7,
    Sign = 8,
    NoSign = 9,
    Parity = 10,
    NoParity = 11,
    Less = 12,
    GreaterEqual = 13,
    LessEqual = 14,
    Greater = 15,
};

[[nodiscard]] constexpr Cond Invert(const Cond condition) noexcept {
    return static_cast<Cond>(static_cast<std::uint8_t>(condition) ^ 1U);
}

/// The condition that holds after the operands of a comparison are exchanged.
[[nodiscard]] constexpr Cond Swap(const Cond condition) noexcept {
    switch (condition) {
    case Cond::Below:
        return Cond::Above;
    case Cond::AboveEqual:
        return Cond::BelowEqual;
    case Cond::BelowEqual:
        return Cond::AboveEqual;
    case Cond::Above:
        return Cond::Below;
    case Cond::Less:
        return Cond::Greater;
    case Cond::GreaterEqual:
        return Cond::LessEqual;
    case Cond::LessEqual:
        return Cond::GreaterEqual;
    case Cond::Greater:
        return Cond::Less;
    default:
        return condition;
    }
}

enum class Op : std::uint16_t {
    // Integer moves. Mov copies a full register (operands: def, use). MovImm materializes operand 1. Load and
    // LoadSigned read `sourceSize` bytes from memory into a register of `size` bytes, zero- or sign-extending. Store
    // writes `size` bytes of a register or an immediate.
    Mov,
    MovImm,
    Load,
    LoadSigned,
    Store,
    Lea,
    // Two-address arithmetic: operand 0 is read and written, operand 1 is a register, immediate or memory.
    Add,
    Sub,
    And,
    Or,
    Xor,
    Imul,
    // Operand 0 is defined from operand 1 times the immediate operand 2.
    ImulImm,
    // Flags only.
    Cmp,
    Test,
    // One operand, read and written.
    Neg,
    Not,
    // Operand 0 read and written, operand 1 an immediate count; without one, the count is CL.
    Shl,
    Shr,
    Sar,
    Ror,
    /// Reverses the byte order of a 32- or 64-bit register in place.
    Bswap,
    /// Unsigned widening multiply of RAX by the operand into RDX:RAX; selected for the high half.
    MulHigh,
    /// Signed widening multiply of RAX by the operand into RDX:RAX; selected for the high half.
    MulHighSigned,
    /// Interleaves the low lanes of two XMM registers (`unpcklpd`); with itself, copies lane 0 into lane 1.
    UnpackLowFp,
    // Two 64-bit integer lanes of XMM registers: `paddq`, `psubq`, `pand`, `por`, `pxor`, and `psllq`/`psrlq` of
    // operand 0 by an immediate count.
    AddPacked64,
    SubPacked64,
    AndPacked,
    OrPacked,
    XorPacked,
    ShlPacked64,
    ShrPacked64,
    // Extension from `sourceSize` bytes of operand 1 into `size` bytes of operand 0.
    MovZx,
    MovSx,
    // Sign of RAX into RDX (size 8: cqo, size 4: cdq).
    SignExtendAccumulator,
    // Divide RDX:RAX by operand 0.
    Div,
    Idiv,
    // Operand 0 receives the condition as a byte.
    SetCc,
    // Control flow.
    Jmp,
    Jcc,
    Ret,
    Call,
    CallIndirect,
    Ud2,
    // Scalar floating point; `size` selects single (4) or double (8) precision.
    MovFp,
    LoadFp,
    StoreFp,
    AddFp,
    SubFp,
    MulFp,
    DivFp,
    SqrtFp,
    // Flags from comparing operand 0 with operand 1, unordered when either is NaN.
    CompareFp,
    // Bitwise exclusive or of whole vector registers, used for negation and zeroing.
    XorFp,
    // Integer of `sourceSize` bytes to floating point of `size` bytes.
    IntToFp,
    // Floating point of `sourceSize` bytes to a truncated signed integer of `size` bytes.
    FpToInt,
    // Floating point of `sourceSize` bytes to floating point of `size` bytes.
    FpToFp,
    // Raw bits between register files: operand 0 is the destination.
    GprToXmm,
    XmmToGpr,
    // Pseudo-instructions the encoder expands.
    //
    // FpToUInt64: operand 0 (GPR) receives the truncation of operand 1 (XMM, clobbered) to an unsigned 64-bit
    // integer, operand 2 is an XMM scratch register.
    FpToUInt64,
    // UInt64ToFp: operand 0 (XMM) receives operand 1 (GPR) converted as an unsigned 64-bit integer to floating point
    // of `size` bytes, correctly rounded; operands 2 and 3 are GPR scratch registers.
    UInt64ToFp,
    // Reports a runtime failure and never returns. Operand 0 holds the address of the message slice;
    // `PanicSite` carries the static text.
    Panic,
    // `rep movsb`: copies RCX bytes from [RSI] to [RDI].
    RepMovsb,
};

/// What a symbol operand refers to. Selection records names; whoever emits the instruction resolves them to object
/// symbols or prints them as labels.
struct SymbolRef {
    enum class Kind : std::uint8_t {
        /// A function or data symbol by its linker name.
        Global,
        /// External data that a named load reads.
        NamedData,
        /// A function by name, resolved as a call target.
        Function,
        /// An interned string literal's bytes, already encoded.
        String,
        Float32,
        Float64,
        Float32SignMask,
        Float64SignMask,
        /// A function imported from KERNEL32.DLL.
        Kernel32,
    };

    Kind kind = Kind::Global;
    std::string text;

    friend bool operator==(const SymbolRef &, const SymbolRef &) = default;
};

/// The static part of one runtime failure report, as symbol indices into the function's symbol table.
struct PanicSite {
    std::uint32_t prefix = Machine::NoSymbol;
    std::uint32_t prefixLength = 0;
    std::uint32_t location = Machine::NoSymbol;
    std::uint32_t locationLength = 0;
    /// Windows writes through `GetStdHandle` and `WriteFile`; other systems use the write system call.
    std::uint32_t getStdHandle = Machine::NoSymbol;
    std::uint32_t writeFile = Machine::NoSymbol;
    /// The stack-trace printer, when the program has one.
    std::uint32_t backtrace = Machine::NoSymbol;
};

/// One selected function, ready for allocation.
struct SelectedFunction {
    Machine::Function function;
    std::vector<SymbolRef> symbols;
    std::vector<PanicSite> panics;
    std::vector<std::string> blockLabels;
    /// Bytes of outgoing argument area every call shares at the bottom of the frame.
    std::int32_t outgoingArgumentSize = 0;
    bool win64 = false;
};
} // namespace Rux::X86_64Machine
