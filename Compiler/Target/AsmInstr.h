#pragma once

// Structured representation of a single instruction inside an `asm func` body.
// Produced by the parser, threaded unchanged through HIR and LIR, and encoded
// to machine code by the assembler of the architecture the body was parsed for
// — CodeGen/X86_64/Assembler.cpp or CodeGen/AArch64/Assembler.cpp.
//
// The two architectures share one operand type rather than one per back end:
// an operand is a register, an immediate, a memory reference or a symbol on
// both, and only the trimmings differ — an x86-64 memory reference scales an
// index register, an AArch64 one extends it and may write the base back.
// Fields belonging to one architecture are simply left at their defaults by the
// other.

#include "SourceModel/SourceLocation.h"
#include "Target/AsmRegisters.h"
#include "Target/Target.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Rux {
/// AArch64: the shift applied to a register operand (`X1, LSL #3`), to the index register of a memory operand (`[X0,
/// X1, LSL #3]`), or to an arithmetic immediate (`#1, LSL #12`). `None` means none was written, which is not the same
/// as `Lsl` by zero: only the latter reaches the encoding.
enum class AsmShiftKind : std::uint8_t {
    None,
    Lsl,
    Lsr,
    Asr,
    Ror,
};

/// AArch64: the extension applied to the second source register of an extended-register form and to the index register
/// of a memory operand.
enum class AsmExtendKind : std::uint8_t {
    None,
    Uxtb,
    Uxth,
    Uxtw,
    Uxtx,
    Sxtb,
    Sxth,
    Sxtw,
    Sxtx,
};

/// AArch64: what a memory operand does to the base register it addresses through — `[X0, #8]`, `[X0], #8` and `[X0,
/// #8]!` respectively.
enum class AsmIndexMode : std::uint8_t {
    Offset,
    PostIndex,
    PreIndex,
};

/// A single operand of an inline-assembly instruction.
struct AsmOperand {
    enum class Kind : std::uint8_t {
        None,
        Reg, // a register:            rax, ecx, r8, x0, w1, sp, d2
        Imm, // an integer immediate:  42, -1, 0xFF, #8
        Mem, // a memory reference:    [rbp + rax*4 - 8], [x0, x1, lsl #3]
        Sym, // a symbol / label name: some_func, .loop, eq
    };

    Kind kind = Kind::None;
    SourceLocation location;

    /// Reg: register name (lower-cased). Sym: symbol / label name.
    std::string name;

    /// Imm: the immediate value. Mem: the displacement.
    std::int64_t imm = 0;

    /// Memory operand. x86-64: [memBase + memIndex*memScale + imm (+ memSym)]. AArch64: [memBase, memIndex] with
    /// `shift` or `extend` applied to the index, or [memBase, #imm] with `indexMode` deciding the writeback.
    std::string memBase;  // may be empty
    std::string memIndex; // may be empty
    int memScale = 1;     // x86-64: 1, 2, 4 or 8
    int memSize = 0;      // x86-64 size hint from byte/word/dword/qword (1/2/4/8); 0 = unspecified
    std::string memSym;   // symbol referenced inside the brackets (rip-relative)

    /// AArch64 trimmings. `shift` and `extend` are mutually exclusive and share the amount, since an instruction never
    /// writes both.
    AsmShiftKind shift = AsmShiftKind::None;
    AsmExtendKind extend = AsmExtendKind::None;
    int shiftAmount = 0;
    AsmIndexMode indexMode = AsmIndexMode::Offset;
};

/// A single instruction (or, when `labelDef` is set, a label definition).
struct AsmInstr {
    SourceLocation location;
    std::string mnemonic; // lower-cased; empty when this entry is a label
    std::vector<AsmOperand> operands;
    std::string labelDef; // non-empty => this entry defines a label

    /// The architecture the body was parsed for, and therefore the one whose register table classified the operands and
    /// whose assembler encodes them.
    Target::Arch arch = Target::Arch::Unknown;
};

/// The mnemonic without the condition an AArch64 branch carries in its name: `b.eq` is the `b.` form of `B`, not an
/// instruction of its own.
[[nodiscard]] inline std::string_view AsmBaseMnemonic(const std::string_view mnemonic) noexcept {
    if (const auto dot = mnemonic.find('.'); dot != std::string_view::npos) {
        return mnemonic.substr(0, dot);
    }
    return mnemonic;
}

/// The condition names an AArch64 instruction may be written with, as a branch suffix (`B.EQ`) or as the operand of a
/// conditional select or compare.
[[nodiscard]] inline bool IsAArch64ConditionName(const std::string_view name) noexcept {
    static const std::unordered_set<std::string_view> conditions = {
        "eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv",
    };
    return conditions.contains(name);
}

/// The instruction names each architecture answers to. The set is wider than what that architecture's assembler
/// encodes: it also names common instructions no back end implements, so an unimplemented instruction is reported by
/// the assembler as unsupported rather than by the front end as belonging to the other architecture. clang-format off
inline const std::unordered_set<std::string_view> &X86_64Mnemonics() {
    static const std::unordered_set<std::string_view> table = {
        /// Encoded by CodeGen/X86_64/Assembler.cpp.
        "adc",
        "add",
        "and",
        "call",
        "cdq",
        "cdqe",
        "cmp",
        "cqo",
        "dec",
        "div",
        "idiv",
        "imul",
        "inc",
        "int",
        "int3",
        "jmp",
        "lea",
        "leave",
        "mov",
        "movapd",
        "movaps",
        "movd",
        "movq",
        "movsd",
        "movss",
        "movsx",
        "movsxd",
        "movupd",
        "movups",
        "movzx",
        "mul",
        "neg",
        "nop",
        "not",
        "or",
        "pop",
        "push",
        "ret",
        "rol",
        "ror",
        "sal",
        "sar",
        "sbb",
        "shl",
        "shr",
        "sub",
        "syscall",
        "test",
        "xor",
        "addpd",
        "addps",
        "addsd",
        "addss",
        "andnpd",
        "andnps",
        "andpd",
        "andps",
        "comisd",
        "comiss",
        "cvtsd2si",
        "cvtsd2ss",
        "cvtsi2sd",
        "cvtsi2ss",
        "cvtss2sd",
        "cvtss2si",
        "cvttsd2si",
        "cvttss2si",
        "divpd",
        "divps",
        "divsd",
        "divss",
        "maxpd",
        "maxps",
        "maxsd",
        "maxss",
        "minpd",
        "minps",
        "minsd",
        "minss",
        "mulpd",
        "mulps",
        "mulsd",
        "mulss",
        "orpd",
        "orps",
        "paddb",
        "paddd",
        "paddq",
        "paddw",
        "pand",
        "pmullw",
        "por",
        "psubb",
        "psubd",
        "psubq",
        "psubw",
        "pxor",
        "sqrtpd",
        "sqrtps",
        "sqrtsd",
        "sqrtss",
        "subpd",
        "subps",
        "subsd",
        "subss",
        "ucomisd",
        "ucomiss",
        "xorpd",
        "xorps",
        /// Recognized but not encoded.
        "bsf",
        "bsr",
        "bswap",
        "bt",
        "btc",
        "btr",
        "bts",
        "cld",
        "cli",
        "cmpxchg",
        "cpuid",
        "enter",
        "hlt",
        "in",
        "iret",
        "lfence",
        "lock",
        "lodsb",
        "lodsq",
        "loop",
        "lzcnt",
        "mfence",
        "movabs",
        "movsb",
        "movsq",
        "out",
        "pause",
        "popcnt",
        "popf",
        "prefetcht0",
        "pushf",
        "rdtsc",
        "rep",
        "repe",
        "repne",
        "scasb",
        "sfence",
        "shld",
        "shrd",
        "std",
        "sti",
        "stosb",
        "stosq",
        "sysenter",
        "sysret",
        "tzcnt",
        "ud2",
        "xadd",
        "xchg",
        "xgetbv",
        /// The x87 stack, whose names an AArch64 body has its own spellings of.
        "fabs",
        "fadd",
        "fchs",
        "fcom",
        "fcomp",
        "fdiv",
        "fild",
        "fistp",
        "fld",
        "fmul",
        "fnstsw",
        "fsqrt",
        "fst",
        "fstp",
        "fsub",
        "fwait",
    };
    return table;
}

inline const std::unordered_set<std::string_view> &AArch64Mnemonics() {
    static const std::unordered_set<std::string_view> table = {
        /// Encoded by CodeGen/AArch64/Encoder.cpp.
        "add",
        "adds",
        "adr",
        "adrp",
        "and",
        "ands",
        "asr",
        "asrv",
        "b",
        "bfi",
        "bfm",
        "bfxil",
        "bic",
        "bics",
        "bl",
        "blr",
        "br",
        "brk",
        "cbnz",
        "cbz",
        "cinc",
        "cinv",
        "cls",
        "clz",
        "cmn",
        "cmp",
        "cneg",
        "csel",
        "cset",
        "csetm",
        "csinc",
        "csinv",
        "csneg",
        "dmb",
        "dsb",
        "eon",
        "eor",
        "extr",
        "hint",
        "hlt",
        "isb",
        "ldp",
        "ldr",
        "ldrb",
        "ldrh",
        "ldrsb",
        "ldrsh",
        "ldrsw",
        "ldur",
        "ldurb",
        "ldurh",
        "ldursb",
        "ldursh",
        "ldursw",
        "lsl",
        "lslv",
        "lsr",
        "lsrv",
        "madd",
        "mneg",
        "mov",
        "movk",
        "movn",
        "movz",
        "mrs",
        "msr",
        "msub",
        "mul",
        "mvn",
        "neg",
        "negs",
        "nop",
        "orn",
        "orr",
        "rbit",
        "ret",
        "rev",
        "rev16",
        "rev32",
        "ror",
        "rorv",
        "sbfm",
        "sbfx",
        "sdiv",
        "smaddl",
        "smulh",
        "smull",
        "stp",
        "str",
        "strb",
        "strh",
        "stur",
        "sturb",
        "sturh",
        "sub",
        "subs",
        "svc",
        "sxtb",
        "sxth",
        "sxtw",
        "tbnz",
        "tbz",
        "tst",
        "ubfm",
        "ubfx",
        "udf",
        "udiv",
        "umaddl",
        "umulh",
        "umull",
        "uxtb",
        "uxth",
        "fabs",
        "fadd",
        "fccmp",
        "fcmp",
        "fcmpe",
        "fcsel",
        "fcvt",
        "fcvtzs",
        "fcvtzu",
        "fdiv",
        "fmadd",
        "fmax",
        "fmaxnm",
        "fmin",
        "fminnm",
        "fmov",
        "fmsub",
        "fmul",
        "fneg",
        "fnmadd",
        "fnmsub",
        "fsqrt",
        "fsub",
        "frinta",
        "frintm",
        "frintn",
        "frintp",
        "frintz",
        "scvtf",
        "ucvtf",
        /// Recognized but not encoded.
        "adc",
        "adcs",
        "at",
        "autiasp",
        "bfc",
        "bti",
        "cas",
        "casa",
        "casal",
        "ccmn",
        "ccmp",
        "csdb",
        "dc",
        "dcps1",
        "drps",
        "eret",
        "esb",
        "fcvtas",
        "fcvtau",
        "fcvtms",
        "fcvtmu",
        "fcvtns",
        "fcvtnu",
        "fcvtps",
        "fcvtpu",
        "fjcvtzs",
        "fnmul",
        "frinti",
        "frintx",
        "hvc",
        "ic",
        "ldadd",
        "ldar",
        "ldaxr",
        "ldclr",
        "ldnp",
        "ldset",
        "ldxr",
        "ngc",
        "ngcs",
        "paciasp",
        "prfm",
        "sbc",
        "sbcs",
        "sev",
        "sevl",
        "smc",
        "stlr",
        "stlxr",
        "stnp",
        "stxr",
        "swp",
        "sys",
        "sysl",
        "tlbi",
        "wfe",
        "wfi",
        "yield",
    };
    return table;
}

// clang-format on

/// True when `arch` has an instruction spelled `mnemonic` (lower-cased, and possibly carrying an AArch64 condition
/// suffix).
[[nodiscard]] inline bool IsAsmMnemonic(const Target::Arch arch, const std::string_view mnemonic) {
    switch (arch) {
    case Target::Arch::X86_64: {
        if (X86_64Mnemonics().contains(mnemonic)) {
            return true;
        }
        /// The condition-code families are written as one mnemonic per condition rather than as a suffixed operand.
        static constexpr std::string_view prefixes[3] = {"j", "set", "cmov"};
        static const std::unordered_set<std::string_view> conditions = {
            "o",   "no", "b",  "c", "nae", "ae", "nb", "nc", "e",   "z",  "ne", "nz", "be", "na", "a",
            "nbe", "s",  "ns", "p", "pe",  "np", "po", "l",  "nge", "ge", "nl", "le", "ng", "g",  "nle",
        };
        for (const auto prefix : prefixes) {
            if (mnemonic.size() > prefix.size() && mnemonic.starts_with(prefix) &&
                conditions.contains(mnemonic.substr(prefix.size()))) {
                return true;
            }
        }
        return false;
    }
    case Target::Arch::AArch64: {
        const std::string_view base = AsmBaseMnemonic(mnemonic);
        if (base.size() == mnemonic.size()) {
            return AArch64Mnemonics().contains(mnemonic);
        }
        /// Only a conditional branch carries a condition in its name.
        return base == "b" && IsAArch64ConditionName(mnemonic.substr(base.size() + 1));
    }
    default:
        return false;
    }
}

/// The architecture that names `mnemonic`, when exactly one of the two the compiler assembles for does. `Unknown` when
/// both do — `add` and `ret` are nobody's alone — or when neither does, which is a misspelling rather than a foreign
/// instruction.
[[nodiscard]] inline Target::Arch AsmMnemonicArch(const std::string_view mnemonic) {
    const bool x86 = IsAsmMnemonic(Target::Arch::X86_64, mnemonic);
    const bool arm = IsAsmMnemonic(Target::Arch::AArch64, mnemonic);
    if (x86 == arm) {
        return Target::Arch::Unknown;
    }
    return x86 ? Target::Arch::X86_64 : Target::Arch::AArch64;
}

/// The instruction names `arch` answers to, for a consumer that has to walk them rather than ask about one. Empty for
/// an architecture with no inline-assembly support.
[[nodiscard]] inline const std::unordered_set<std::string_view> &AsmMnemonics(const Target::Arch arch) {
    static const std::unordered_set<std::string_view> none;
    switch (arch) {
    case Target::Arch::X86_64:
        return X86_64Mnemonics();
    case Target::Arch::AArch64:
        return AArch64Mnemonics();
    default:
        return none;
    }
}

/// The number of single-character insertions, deletions and substitutions that turn `left` into `right`.
[[nodiscard]] inline std::size_t AsmNameDistance(const std::string_view left, const std::string_view right) {
    std::vector<std::size_t> row(right.size() + 1);
    for (std::size_t i = 0; i <= right.size(); ++i) {
        row[i] = i;
    }
    for (std::size_t i = 1; i <= left.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= right.size(); ++j) {
            const std::size_t previous = row[j];
            const std::size_t substitute = diagonal + (left[i - 1] == right[j - 1] ? 0 : 1);
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, substitute});
            diagonal = previous;
        }
    }
    return row.back();
}

/// The instruction of `arch` closest to `mnemonic`, when one is close enough to be worth naming in a diagnostic. A
/// misspelling is usually one key away, so the threshold is a single edit for the short names most mnemonics have and
/// two for the longer ones; a name nothing comes that close to is a name its author meant, and offering the nearest of
/// a hundred instructions would only be noise.
///
/// Equal distances are broken alphabetically rather than by whichever the table yields first: the table is unordered,
/// and a suggestion that depends on the standard library's hashing would differ between platforms for the same mistake.
[[nodiscard]] inline std::optional<std::string_view> ClosestAsmMnemonic(const Target::Arch arch,
                                                                        const std::string_view mnemonic) {
    const std::size_t threshold = mnemonic.size() <= 4 ? 1 : 2;
    std::size_t bestDistance = threshold + 1;
    std::optional<std::string_view> best;
    for (const std::string_view candidate : AsmMnemonics(arch)) {
        const std::size_t distance = AsmNameDistance(mnemonic, candidate);
        if (distance > threshold) {
            continue;
        }
        if (!best.has_value() || distance < bestDistance || (distance == bestDistance && candidate < *best)) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}
} // namespace Rux
