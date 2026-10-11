#pragma once

// AArch64 opcodes, registers and symbol references of the machine IR (`CodeGen/Machine/MachineIr.h`).
//
// Registers 0 to 30 are X0 to X30 and 32 to 63 are V0 to V31; register 31 names nothing, because the zero register and
// the stack pointer are spelled by the encoder where an operation needs them. An instruction's `size` is its operand
// width in bytes: 4 or 8 for integer operations, which select the W or X form, and 4 or 8 for scalar floating point.
// Integer registers hold a value of a narrower type in their low bits; the bits above are unspecified unless the type
// is a bool, which is always held as exactly 0 or 1. Selection extends a value where an operation reads its full
// width, as the x86-64 selector does.

#include "CodeGen/AArch64/Registers.h"
#include "CodeGen/Machine/MachineIr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux::AArch64Machine {
using Machine::Reg;

inline constexpr Reg X0 = 0;
/// The indirect-result register: a caller passes the address of a result too large for registers here.
inline constexpr Reg X8 = 8;
/// The intra-procedure-call scratch registers. Never allocated: the encoder legalizes addresses and immediates in
/// them, and a linker veneer may clobber them at a call.
inline constexpr Reg X16 = 16;
inline constexpr Reg X17 = 17;
inline constexpr Reg X19 = 19;
inline constexpr Reg Fp = 29;
/// The stack pointer, as the base of an address only: register 31 is never allocated or named elsewhere.
inline constexpr Reg Sp = 31;
inline constexpr Reg Lr = 30;
inline constexpr Reg V0 = 32;

[[nodiscard]] constexpr bool IsVectorRegister(const Reg reg) noexcept {
    return reg >= V0 && reg < V0 + 32;
}

/// The register's number within its file, as the instruction fields encode it.
[[nodiscard]] constexpr unsigned HardwareNumber(const Reg reg) noexcept {
    return IsVectorRegister(reg) ? reg - V0 : reg;
}

enum class Op : std::uint16_t {
    // Integer moves. Mov copies a whole register (def, use). MovImm materializes operand 1, any 64-bit value. Load
    // reads `sourceSize` bytes zero-extended into a register; LoadSigned sign-extends them to `size` bytes. Store
    // writes the low `size` bytes of a register, or zero from an immediate 0. AddressOf computes an address.
    Mov,
    MovImm,
    Load,
    LoadSigned,
    Store,
    AddressOf,
    // Three-address arithmetic: operand 0 is defined from operand 1 and operand 2, a register or an immediate the
    // instruction encodes (selection checks).
    Add,
    Sub,
    And,
    Or,
    Xor,
    Mul,
    // The high 64 bits of the 128-bit product of two registers.
    MulHigh,
    MulHighSigned,
    Div,
    Idiv,
    // Operand 0 = operand 3 - operand 1 * operand 2.
    MultiplySubtract,
    // Two operands: defined, read.
    Neg,
    Not,
    // Operand 0 from operand 1 shifted by operand 2, a register (taken modulo the width) or an immediate.
    Shl,
    Shr,
    Sar,
    Ror,
    // Operand 0 from the low `sourceSize` bytes of operand 1, extended to 64 bits.
    ZeroExtend,
    SignExtend,
    /// Reverses the byte order of a 4- or 8-byte register.
    Bswap,
    // Flags only: compare operand 0 with operand 1, a register or an immediate (negative ones compare by addition),
    // or test their common bits.
    Cmp,
    Tst,
    // Operand 0 becomes 1 when the condition holds, 0 otherwise.
    CSet,
    // Control flow.
    Jmp,
    Jcc,
    Ret,
    Call,
    CallIndirect,
    Trap,
    // Scalar floating point; `size` selects single (4) or double (8) precision. MovFp copies a whole vector register.
    MovFp,
    LoadFp,
    StoreFp,
    AddFp,
    SubFp,
    MulFp,
    DivFp,
    SqrtFp,
    NegFp,
    CompareFp,
    // Conversions: an integer of `sourceSize` bytes to floating point of `size` bytes, signed or unsigned; floating
    // point of `sourceSize` bytes to an integer of `size` bytes, truncated; floating point between precisions.
    IntToFp,
    UIntToFp,
    FpToInt,
    FpToUInt,
    FpToFp,
    // Raw bits between register files: operand 0 is the destination, `size` the width.
    GprToFp,
    FpToGpr,
    // Copies `size` bytes from the address in operand 1 to the address in operand 0; both registers are advanced
    // past the copy (use and def).
    CopyBytes,
    // Reports a runtime failure and never returns. Operand 0 holds the address of the message slice; `PanicSite`
    // carries the static text.
    Panic,
};

/// What a symbol operand refers to. Selection records names; whoever emits the instruction resolves them to object
/// symbols.
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
};
} // namespace Rux::AArch64Machine
