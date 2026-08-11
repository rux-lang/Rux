// AArch64 assembler for `asm func` bodies. ARM syntax (destination first).
//
// The counterpart of CodeGen/X86_64/Assembler.cpp: it reads the instructions
// the parser produced for Target::Arch::AArch64, picks an encoding for each,
// resolves the branches whose target the body itself defines, and reports every
// other reference as a fixup for the object emitter to relocate.
//
// Nothing here builds an instruction word. Every form goes through A64Enc,
// which reports A64Status and emits nothing when it refuses, so an operand
// combination the architecture cannot express arrives as a status to turn into
// a diagnostic rather than as a wrong instruction. What is left for this file
// is choosing which encoder a written instruction means: `ADD` is three
// instructions depending on whether its last operand is an immediate, a shifted
// register or an extended one, and `LDR` is five depending on how its memory
// operand addresses.
//
// The supported subset is every instruction the Phase 1 encoders provide. A
// mnemonic the architecture has but they do not — the atomics, the
// load-exclusive pairs, the vector forms — is reported as unsupported, which is
// what keeps the front end from having to know which of the two it is.
//
// What an encoder reports is that some operand had no encoding, which is true
// but useless to whoever wrote the instruction. So every form validates its
// operands here first, against the shape the instruction actually has, and each
// diagnostic ends by quoting that shape: the width the registers had to share,
// the range the immediate had to fall in, the reading of code 31 the field
// takes. The encoder's own status is the last resort, for the combinations no
// check above it anticipated.

#include "CodeGen/AArch64/Assembler.h"

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"
#include "Object/Rcu/Rcu.h"

#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Rux {
namespace {
using Bytes = std::vector<std::uint8_t>;

// The encoder entry points, grouped by the operand list they take. An
// instruction family is a table from mnemonic to one of these, so the operand
// checking is written once per shape rather than once per instruction.
using RegRegImmFn = A64Status (A64Enc::*)(A64Reg, A64Reg, std::uint64_t) const;
using ShiftedFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, A64ShiftKind, unsigned) const;
using ExtendedFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, A64ExtendKind, unsigned) const;
using UnscaledMemFn = A64Status (A64Enc::*)(A64Reg, A64Reg, std::int64_t, A64IndexMode) const;
using PairFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, std::int64_t, A64IndexMode) const;
using Reg1Fn = A64Status (A64Enc::*)(A64Reg) const;
using Reg2Fn = A64Status (A64Enc::*)(A64Reg, A64Reg) const;
using Reg3Fn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg) const;
using Reg4Fn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, A64Reg) const;
using Reg2ShiftFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64ShiftKind, unsigned) const;
using ShiftImmFn = A64Status (A64Enc::*)(A64Reg, A64Reg, unsigned) const;
using BitfieldFn = A64Status (A64Enc::*)(A64Reg, A64Reg, unsigned, unsigned) const;
using MovwFn = A64Status (A64Enc::*)(A64Reg, std::uint16_t, unsigned) const;
using CondSelFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, A64Condition) const;
using CondAliasFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Condition) const;
using CondSetFn = A64Status (A64Enc::*)(A64Reg, A64Condition) const;
using CompareBranchFn = A64Status (A64Enc::*)(A64Reg, std::int64_t) const;
using TestBranchFn = A64Status (A64Enc::*)(A64Reg, unsigned, std::int64_t) const;
using Imm16Fn = A64Status (A64Enc::*)(std::uint16_t) const;
using BarrierFn = A64Status (A64Enc::*)(A64Barrier) const;

// The register file a form draws its operands from, which is what makes
// `fadd x0, x1, x2` a mistake this file can name rather than a status the
// encoder reports.
enum class RegClass : std::uint8_t {
    General, // X and W registers, every one of them at the same width
    Float,   // the scalar views of the SIMD file, every one at the same width
    Mixed,   // the files or the widths deliberately disagree; the encoder rules
};

// The syntax a diagnostic quotes back, written the way the architecture manual
// writes it: `Xd` and `Wd` where only that width encodes, `Rd` where the
// instruction has both and either will do, `Vd` for a scalar floating-point
// register at whatever precision the form carries, `#imm` for an immediate, and
// braces around what may be left out. An empty string means the family's
// default shape, which each Encode* function supplies.
using Syntax = std::string_view;

// One entry of a dispatch table: the encoder to call, the syntax that names it,
// and the file its registers come from. Both trailing fields have defaults, so
// an instruction whose shape is the family's says nothing about it.
template <typename Fn>
struct Form {
    Fn fn = nullptr;
    Syntax syntax = "";
    RegClass regClass = RegClass::General;
};

// ADD / SUB and their flag-setting forms, which have all three source shapes;
// CMP and CMN are the same instructions with the result discarded.
struct ArithForms {
    RegRegImmFn imm;
    ShiftedFn shifted;
    ExtendedFn extended;
    bool discardsResult = false;
    // ADD and SUB address through SP and write it back; the flag-setting pair
    // reads it and writes the zero register, which is what makes them CMP and
    // CMN.
    bool writesStackPointer = false;
};

// The logical group, which has no extended-register form and, in the inverting
// half, no immediate one either.
struct LogicForms {
    RegRegImmFn imm; // null: the form is register-only
    ShiftedFn shifted;
    bool discardsResult = false;
};

// A load or store, in the addressing modes its mnemonic can reach. The LDUR and
// STUR spellings name the unscaled form outright, so they carry neither of the
// other two.
struct MemForms {
    RegRegImmFn scaled; // null: the mnemonic is one of the unscaled spellings
    UnscaledMemFn unscaled;
    ExtendedFn indexed; // null: no register-offset form under this mnemonic
    // Bytes one access moves, which is the scale of the immediate offset and so
    // the reach a diagnostic names. Zero means the width comes from the
    // transferred register rather than from the mnemonic.
    unsigned accessBytes = 0;
    bool literal = false;
};

// A shift by a constant and the variable-register instruction the same mnemonic
// means when its last operand is a register.
struct ShiftForms {
    ShiftImmFn imm;
    Reg3Fn variable;
};

// A bitfield move. SBFM, UBFM and BFM name a rotate and the top bit of the
// field; the four aliases name where a field sits and how wide it is. Both
// pairs are bounded by the register, but they are not the same pair of numbers,
// so a diagnostic about one has to say which it means.
struct BitfieldForms {
    BitfieldFn fn;
    Syntax syntax;
    bool field = false;
};

// Which immediate field an instruction keeps a code address in. Every one of
// them counts instructions rather than bytes, apart from ADR, which is the only
// form here that reaches an arbitrary byte and the only one whose immediate is
// split across two fields.
enum class TargetField : std::uint8_t {
    Imm26, // B and BL
    Imm19, // B.cond, CBZ / CBNZ, LDR (literal)
    Imm14, // TBZ / TBNZ
    Adr,   // ADR
};

// Where the field sits and how wide it is, which is all the second pass needs
// to patch it and to know how far it reaches.
struct FieldLayout {
    unsigned lsb;
    unsigned width;
};

[[nodiscard]] constexpr FieldLayout LayoutOf(const TargetField field) noexcept {
    switch (field) {
    case TargetField::Imm26:
        return {0, 26};
    case TargetField::Imm14:
        return {5, 14};
    case TargetField::Adr:
        return {5, 21}; // immhi; immlo is patched alongside it
    case TargetField::Imm19:
        break;
    }
    return {5, 19};
}

// How far a field reaches from the instruction that carries it, in bytes. Every
// field is signed, and all but ADR's are scaled by the instruction size.
[[nodiscard]] constexpr std::int64_t ReachOf(const TargetField field) noexcept {
    const std::int64_t half = std::int64_t{1} << (LayoutOf(field).width - 1U);
    return field == TargetField::Adr ? half : half * A64Enc::InstrSize;
}

// The reach as a diagnostic spells it.
[[nodiscard]] std::string ReachText(const TargetField field) {
    const std::int64_t reach = ReachOf(field);
    if (reach >= 1024 * 1024) {
        return std::format("+/-{} MiB", reach / (1024 * 1024));
    }
    return std::format("+/-{} KiB", reach / 1024);
}

// A register name as the encoder models it. Both tables describe a register the
// same way — a file, a number and a width — so the translation is the width in
// bits and the reading of code 31 the name carries.
[[nodiscard]] A64Reg ToA64Reg(const AsmRegInfo &info) noexcept {
    const auto bits = static_cast<unsigned>(info.size) * 8U;
    if (info.file == AsmRegFile::Vector) {
        return A64::Vn(static_cast<unsigned>(info.code), bits);
    }
    A64Reg reg = A64::Gpr(static_cast<unsigned>(info.code), bits);
    reg.stackPointer = info.stackPointer;
    return reg;
}

// The zero register at the width of `reg`, which is the destination of every
// instruction written as a comparison.
[[nodiscard]] A64Reg ZeroLike(const A64Reg reg) noexcept {
    return A64::Gpr(31, reg.bits);
}

// `None` means the operand carried no shift at all, which the encoders spell as
// LSL by zero.
[[nodiscard]] A64ShiftKind ToA64Shift(const AsmShiftKind kind) noexcept {
    switch (kind) {
    case AsmShiftKind::Lsr:
        return A64ShiftKind::Lsr;
    case AsmShiftKind::Asr:
        return A64ShiftKind::Asr;
    case AsmShiftKind::Ror:
        return A64ShiftKind::Ror;
    case AsmShiftKind::None:
    case AsmShiftKind::Lsl:
        break;
    }
    return A64ShiftKind::Lsl;
}

// The spellings a diagnostic gives back the shift and extend a body wrote.
[[nodiscard]] std::string_view ShiftName(const AsmShiftKind kind) noexcept {
    switch (kind) {
    case AsmShiftKind::Lsr:
        return "LSR";
    case AsmShiftKind::Asr:
        return "ASR";
    case AsmShiftKind::Ror:
        return "ROR";
    case AsmShiftKind::None:
    case AsmShiftKind::Lsl:
        break;
    }
    return "LSL";
}

[[nodiscard]] std::string_view ExtendName(const AsmExtendKind kind) noexcept {
    switch (kind) {
    case AsmExtendKind::Uxtb:
        return "UXTB";
    case AsmExtendKind::Uxth:
        return "UXTH";
    case AsmExtendKind::Uxtw:
        return "UXTW";
    case AsmExtendKind::Sxtb:
        return "SXTB";
    case AsmExtendKind::Sxth:
        return "SXTH";
    case AsmExtendKind::Sxtw:
        return "SXTW";
    case AsmExtendKind::Sxtx:
        return "SXTX";
    case AsmExtendKind::None:
    case AsmExtendKind::Uxtx:
        break;
    }
    return "UXTX";
}

// An extension that reads the whole of its register rather than a part of it,
// which is the pair assembly syntax also spells LSL.
[[nodiscard]] bool ExtendsWholeRegister(const AsmExtendKind kind) noexcept {
    return kind == AsmExtendKind::None || kind == AsmExtendKind::Uxtx || kind == AsmExtendKind::Sxtx;
}

[[nodiscard]] A64ExtendKind ToA64Extend(const AsmExtendKind kind) noexcept {
    switch (kind) {
    case AsmExtendKind::Uxtb:
        return A64ExtendKind::Uxtb;
    case AsmExtendKind::Uxth:
        return A64ExtendKind::Uxth;
    case AsmExtendKind::Uxtw:
        return A64ExtendKind::Uxtw;
    case AsmExtendKind::Sxtb:
        return A64ExtendKind::Sxtb;
    case AsmExtendKind::Sxth:
        return A64ExtendKind::Sxth;
    case AsmExtendKind::Sxtw:
        return A64ExtendKind::Sxtw;
    case AsmExtendKind::Sxtx:
        return A64ExtendKind::Sxtx;
    case AsmExtendKind::None:
    case AsmExtendKind::Uxtx:
        break;
    }
    return A64ExtendKind::Uxtx;
}

[[nodiscard]] A64IndexMode ToA64IndexMode(const AsmIndexMode mode) noexcept {
    switch (mode) {
    case AsmIndexMode::PreIndex:
        return A64IndexMode::PreIndex;
    case AsmIndexMode::PostIndex:
        return A64IndexMode::PostIndex;
    case AsmIndexMode::Offset:
        break;
    }
    return A64IndexMode::Offset;
}

// A symbol operand keeps the spelling it was written with, since it may name a
// label whose case matters; the vocabulary words spelled in that position —
// conditions, barrier options, system registers — do not.
[[nodiscard]] std::string Lowered(const std::string_view name) {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char c : name) {
        lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered;
}

// A mnemonic as a quoted form spells it. The parser lower-cases what it reads,
// and a form is conventionally written in upper case.
[[nodiscard]] std::string Uppered(const std::string_view name) {
    std::string uppered;
    uppered.reserve(name.size());
    for (const char c : name) {
        uppered += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return uppered;
}

// What an operand turned out to be, for a diagnostic that wanted something else
// of it. A register and a symbol are worth quoting, since an unknown register
// name arrives as a symbol and the spelling is the whole of the mistake.
[[nodiscard]] std::string FoundText(const AsmOperand &op) {
    switch (op.kind) {
    case AsmOperand::Kind::Reg:
        return std::format("the register '{}'", op.name);
    case AsmOperand::Kind::Sym:
        return std::format("the symbol '{}'", op.name);
    case AsmOperand::Kind::Imm:
        return std::format("the immediate {}", op.imm);
    case AsmOperand::Kind::Mem:
        return "a memory operand";
    case AsmOperand::Kind::None:
        break;
    }
    return "nothing";
}

[[nodiscard]] std::optional<A64Condition> ConditionFromName(const std::string_view name) {
    static const std::unordered_map<std::string_view, A64Condition> table = {
        {"eq", A64Condition::Eq}, {"ne", A64Condition::Ne}, {"cs", A64Condition::Cs}, {"hs", A64Condition::Cs},
        {"cc", A64Condition::Cc}, {"lo", A64Condition::Cc}, {"mi", A64Condition::Mi}, {"pl", A64Condition::Pl},
        {"vs", A64Condition::Vs}, {"vc", A64Condition::Vc}, {"hi", A64Condition::Hi}, {"ls", A64Condition::Ls},
        {"ge", A64Condition::Ge}, {"lt", A64Condition::Lt}, {"gt", A64Condition::Gt}, {"le", A64Condition::Le},
        {"al", A64Condition::Al}, {"nv", A64Condition::Nv},
    };
    if (const auto it = table.find(name); it != table.end()) {
        return it->second;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<A64Barrier> BarrierFromName(const std::string_view name) {
    static const std::unordered_map<std::string_view, A64Barrier> table = {
        {"oshld", A64Barrier::Oshld}, {"oshst", A64Barrier::Oshst}, {"osh", A64Barrier::Osh},
        {"nshld", A64Barrier::Nshld}, {"nshst", A64Barrier::Nshst}, {"nsh", A64Barrier::Nsh},
        {"ishld", A64Barrier::Ishld}, {"ishst", A64Barrier::Ishst}, {"ish", A64Barrier::Ish},
        {"ld", A64Barrier::Ld},       {"st", A64Barrier::St},       {"sy", A64Barrier::Sy},
    };
    if (const auto it = table.find(name); it != table.end()) {
        return it->second;
    }
    return std::nullopt;
}

// The system registers an `asm func` body has reason to name: the condition
// flags, the two floating-point control and status registers, the interrupt
// mask, and the thread pointer that thread-local storage begins at.
[[nodiscard]] std::optional<std::uint16_t> SysRegFromName(const std::string_view name) {
    static const std::unordered_map<std::string_view, std::uint16_t> table = {
        {"nzcv", A64::Nzcv},
        {"daif", A64::SysReg(3, 3, 4, 2, 1)},
        {"fpcr", A64::SysReg(3, 3, 4, 4, 0)},
        {"fpsr", A64::SysReg(3, 3, 4, 4, 1)},
        {"tpidr_el0", A64::TpidrEl0},
    };
    if (const auto it = table.find(name); it != table.end()) {
        return it->second;
    }
    return std::nullopt;
}

// The entry of `table` named by `mnemonic`, or null when it names none. The
// tables are keyed by string_view over literals that outlive the assembler.
template <typename Map>
[[nodiscard]] const typename Map::mapped_type *Lookup(const Map &table, const std::string &mnemonic) {
    const auto it = table.find(mnemonic);
    return it == table.end() ? nullptr : &it->second;
}

class Assembler {
public:
    Assembler(const std::vector<AsmInstr> &instrs, std::string sourceName, Bytes &out)
        : instrs_(instrs)
        , sourceName_(std::move(sourceName))
        , out_(out)
        , enc_(out_) {
    }

    AsmAssembly Run() {
        CollectLabels();
        for (const auto &instr : instrs_) {
            if (!instr.labelDef.empty()) {
                labels_[instr.labelDef] = Here();
                continue;
            }
            EncodeInstr(instr);
        }
        ResolveLocalTargets();
        result_.ok = result_.diagnostics.empty();
        return std::move(result_);
    }

private:
    const std::vector<AsmInstr> &instrs_;
    std::string sourceName_;
    Bytes &out_;
    A64Enc enc_;
    AsmAssembly result_;

    // Label name -> offset within out_ (absolute).
    std::unordered_map<std::string, std::uint32_t> labels_;

    // An instruction pointed at a label the body defines, whose offset is only
    // known once every instruction after it has been encoded.
    struct LocalTarget {
        std::uint32_t instrOffset;
        std::string label;
        SourceLocation loc;
        std::string mnemonic;
        TargetField field;
    };

    std::vector<LocalTarget> targets_;

    // The instruction every diagnostic below reports against and the form it
    // quotes. An assembler encodes one instruction at a time, and threading
    // both of these through every operand check would bury the checks
    // themselves, so they are set once per instruction instead.
    const AsmInstr *in_ = nullptr;
    Syntax syntax_;

    void Begin(const AsmInstr &in, const Syntax syntax) {
        in_ = &in;
        syntax_ = syntax;
    }

    void Error(const SourceLocation &loc, std::string msg) {
        Diagnostic d;
        d.severity = Diagnostic::Severity::Error;
        d.message = std::move(msg);
        d.location = loc;
        d.sourceName = sourceName_;
        result_.diagnostics.push_back(std::move(d));
    }

    // The form the current instruction should have taken, as a diagnostic
    // quotes it.
    [[nodiscard]] std::string FormText() const {
        std::string mnemonic = Uppered(in_->mnemonic);
        return syntax_.empty() ? mnemonic : std::format("{} {}", mnemonic, syntax_);
    }

    // Report `what` and name the form that was wanted. Every operand
    // diagnostic ends this way: what was written is half a report, and what
    // should have been written is the other half.
    void FormError(const SourceLocation &loc, const std::string &what) {
        Error(loc, std::format("{}; the form is '{}'", what, FormText()));
    }

    // Which operand of the current instruction `op` is, counting from one.
    [[nodiscard]] std::size_t IndexOf(const AsmOperand &op) const {
        return static_cast<std::size_t>(&op - in_->operands.data()) + 1;
    }

    [[nodiscard]] std::uint32_t Here() const {
        return static_cast<std::uint32_t>(out_.size());
    }

    void CollectLabels() {
        // Offsets are filled in during encoding; here we only reserve the names
        // so a forward branch can tell a local label from an extern symbol.
        for (const auto &instr : instrs_) {
            if (!instr.labelDef.empty()) {
                labels_.emplace(instr.labelDef, 0);
            }
        }
    }

    // Operand access. Each of these reports what it wanted and hands back
    // nothing, so a handler that reads several operands stops at the first
    // mistake rather than inventing a register to carry on with.

    [[nodiscard]] bool Operands(const std::size_t count) {
        if (in_->operands.size() == count) {
            return true;
        }
        FormError(in_->location, std::format("'{}' takes {} operand{}, found {}", in_->mnemonic, count,
                                             count == 1 ? "" : "s", in_->operands.size()));
        return false;
    }

    [[nodiscard]] bool Operands(const std::size_t least, const std::size_t most) {
        if (in_->operands.size() >= least && in_->operands.size() <= most) {
            return true;
        }
        FormError(in_->location, std::format("'{}' takes {} to {} operands, found {}", in_->mnemonic, least, most,
                                             in_->operands.size()));
        return false;
    }

    [[nodiscard]] std::optional<A64Reg> RegNamed(const std::string &name, const SourceLocation &loc) {
        const AsmRegInfo info = LookupRegister(Target::Arch::AArch64, name);
        if (!info.valid) {
            FormError(loc, std::format("'{}' is not an AArch64 register", name));
            return std::nullopt;
        }
        return ToA64Reg(info);
    }

    [[nodiscard]] std::optional<A64Reg> RegOf(const AsmOperand &op) {
        if (op.kind != AsmOperand::Kind::Reg) {
            FormError(op.location, std::format("'{}' takes a register as operand {}, found {}", in_->mnemonic,
                                               IndexOf(op), FoundText(op)));
            return std::nullopt;
        }
        return RegNamed(op.name, op.location);
    }

    [[nodiscard]] std::optional<std::int64_t> ImmOf(const AsmOperand &op) {
        if (op.kind != AsmOperand::Kind::Imm) {
            FormError(op.location, std::format("'{}' takes an immediate as operand {}, found {}", in_->mnemonic,
                                               IndexOf(op), FoundText(op)));
            return std::nullopt;
        }
        return op.imm;
    }

    // An immediate operand written with `LSL #n`, as the value it stands for:
    // the arithmetic encoders take the unshifted value and select the shifted
    // field for it themselves.
    [[nodiscard]] std::uint64_t ShiftedImm(const AsmOperand &op) const {
        const auto value = static_cast<std::uint64_t>(op.imm);
        if (op.shift == AsmShiftKind::Lsl && op.shiftAmount > 0 && op.shiftAmount < 64) {
            return value << static_cast<unsigned>(op.shiftAmount);
        }
        return value;
    }

    // An immediate in a field that counts from zero — a halfword, a bit number,
    // a shift, an exception code. `what` names the field, since "0 to 63" says
    // nothing on its own about which of the two immediates of a bitfield
    // instruction was wrong.
    [[nodiscard]] std::optional<unsigned> UnsignedImmOf(const AsmOperand &op, const std::uint64_t limit,
                                                        const std::string_view what) {
        const auto value = ImmOf(op);
        if (!value) {
            return std::nullopt;
        }
        if (*value < 0 || static_cast<std::uint64_t>(*value) > limit) {
            FormError(op.location,
                      std::format("'{}' takes {} of 0 to {}, found {}", in_->mnemonic, what, limit, *value));
            return std::nullopt;
        }
        return static_cast<unsigned>(*value);
    }

    [[nodiscard]] std::optional<A64Condition> CondOf(const AsmOperand &op) {
        if (op.kind != AsmOperand::Kind::Sym) {
            FormError(op.location, std::format("'{}' takes a condition as operand {}, found {}", in_->mnemonic,
                                               IndexOf(op), FoundText(op)));
            return std::nullopt;
        }
        if (const auto cond = ConditionFromName(Lowered(op.name))) {
            return cond;
        }
        FormError(op.location, std::format("unknown condition '{}'", op.name));
        return std::nullopt;
    }

    // Operand-form checks. What an encoder reports is that an operand had no
    // encoding; what these report is which operand, what was wrong with it and
    // what would have been right, which is the difference between a status and
    // a diagnostic.

    // A register operand as it was written and as the encoder models it, so a
    // check can read the fields while the diagnostic names the spelling. The
    // name travels separately because the base register of a memory operand is
    // spelled inside the brackets rather than in the operand's own name.
    struct RegRef {
        const AsmOperand *op = nullptr;
        A64Reg reg;
        std::string_view name;
    };

    [[nodiscard]] static RegRef Ref(const AsmOperand &op, const A64Reg reg) {
        return {&op, reg, op.name};
    }

    // Every register of a uniform form comes from one file and is read at one
    // width. The forms whose operands deliberately disagree — the
    // extended-register arithmetic, the widening multiplies, the extensions,
    // the conversions between the two files — are RegClass::Mixed and are left
    // to the encoder, which knows which of the widths is the odd one.
    [[nodiscard]] bool Uniform(const RegClass regClass, const std::initializer_list<RegRef> regs) {
        if (regClass == RegClass::Mixed) {
            return true;
        }
        const RegRef *first = nullptr;
        for (const auto &ref : regs) {
            if (regClass == RegClass::Float && !ref.reg.IsVector()) {
                FormError(ref.op->location, std::format("'{}' takes a floating-point register as operand {}, found the "
                                                        "general-purpose '{}'",
                                                        in_->mnemonic, IndexOf(*ref.op), ref.name));
                return false;
            }
            if (regClass == RegClass::General && ref.reg.IsVector()) {
                FormError(ref.op->location,
                          std::format("'{}' takes a general-purpose register as operand {}, found the "
                                      "floating-point '{}'",
                                      in_->mnemonic, IndexOf(*ref.op), ref.name));
                return false;
            }
            if (first == nullptr) {
                first = &ref;
                continue;
            }
            if (ref.reg.bits != first->reg.bits) {
                FormError(ref.op->location,
                          std::format("'{}' takes operands of one width, and operand {} '{}' is {}-bit where '{}' "
                                      "is {}-bit",
                                      in_->mnemonic, IndexOf(*ref.op), ref.name, ref.reg.bits, first->name,
                                      first->reg.bits));
                return false;
            }
        }
        return true;
    }

    // Code 31 is the zero register in every field but the few that address
    // memory or arithmetic through the stack pointer. Nothing in the encoding
    // tells the two apart, so `sp` written where XZR is meant would otherwise
    // assemble into an instruction about the wrong register.
    [[nodiscard]] bool NoStackPointer(const std::initializer_list<RegRef> regs) {
        for (const auto &ref : regs) {
            if (!ref.reg.IsStackPointer()) {
                continue;
            }
            FormError(ref.op->location,
                      std::format("'{}' reads register 31 as the zero register, so operand {} cannot be '{}' but may "
                                  "be '{}'",
                                  in_->mnemonic, IndexOf(*ref.op), ref.name, ref.reg.Is64() ? "xzr" : "wzr"));
            return false;
        }
        return true;
    }

    // The arithmetic immediate: twelve bits, optionally shifted left by twelve,
    // with nothing in between the two ranges.
    [[nodiscard]] bool CheckArithImm(const AsmOperand &op, const std::uint64_t value) {
        if (TryEncodeArithImm12(value)) {
            return true;
        }
        FormError(op.location, std::format("'{}' takes an immediate of 0 to 4095 or a multiple of 4096 up to "
                                           "16773120, found {}",
                                           in_->mnemonic, value));
        return false;
    }

    // The logical immediate, which is a pattern rather than a number: no field
    // holds the value, so the values with no pattern are simply unreachable and
    // have to be moved into a register first.
    [[nodiscard]] bool CheckLogicalImm(const AsmOperand &op, const std::uint64_t value, const bool is64) {
        if (TryEncodeBitmaskImm(value, is64)) {
            return true;
        }
        FormError(op.location, std::format("'{}' takes a bitmask immediate (a run of one bits, rotated and repeated "
                                           "to fill the register), and {} is not one",
                                           in_->mnemonic, value));
        return false;
    }

    // A shift or a bit number, which must name a bit the register has.
    [[nodiscard]] std::optional<unsigned> BitOf(const AsmOperand &op, const A64Reg reg, const std::string_view what) {
        return UnsignedImmOf(op, reg.bits - 1U, what);
    }

    // Emission

    // Report `status` against the instruction it came from. An encoder that
    // refuses emits nothing at all, and EncodeInstr fills the hole.
    void Emit(const AsmInstr &in, const A64Status status) {
        if (status != A64Status::Ok) {
            Error(in.location, std::format("cannot encode '{}': {}", in.mnemonic, A64StatusName(status)));
        }
    }

    void EncodeInstr(const AsmInstr &in) {
        const std::uint32_t before = Here();
        const std::size_t reported = result_.diagnostics.size();
        Dispatch(in);
        if (result_.diagnostics.size() != reported && Here() == before) {
            // A refused instruction still occupies its place, so that one
            // mistake does not move every label after it and invent a second.
            enc_.Word(0);
        }
    }

    // Point the instruction at `at` at the operand `target` names: a label this
    // body defines is patched in the second pass, and any other name is a fixup
    // for the linker — or, in a form with no relocation to carry it, a
    // diagnostic rather than an offset of zero left silently in place.
    void RecordTarget(const AsmInstr &in, const AsmOperand &target, const std::uint32_t at, const TargetField field,
                      const std::uint16_t relType) {
        if (target.kind != AsmOperand::Kind::Sym) {
            FormError(target.location, std::format("'{}' takes a label or a symbol as operand {}, found {}",
                                                   in.mnemonic, IndexOf(target), FoundText(target)));
            return;
        }
        if (labels_.contains(target.name)) {
            targets_.push_back({at, target.name, target.location, in.mnemonic, field});
            return;
        }
        if (relType == RcuRelType::None) {
            Error(target.location,
                  std::format("'{}' cannot reference '{}': no label of that name is defined in this body", in.mnemonic,
                              target.name));
            return;
        }
        result_.fixups.push_back({at, target.name, relType, 0});
    }

    void ResolveLocalTargets() {
        for (const auto &target : targets_) {
            const auto it = labels_.find(target.label);
            if (it == labels_.end()) {
                Error(target.loc, std::format("undefined label '{}'", target.label));
                continue;
            }
            const auto delta = static_cast<std::int64_t>(it->second) - static_cast<std::int64_t>(target.instrOffset);
            const std::int64_t reach = ReachOf(target.field);
            if (delta < -reach || delta >= reach) {
                Error(target.loc, std::format("'{}' is {} bytes from '{}', past the {} its offset field reaches",
                                              target.mnemonic, delta, target.label, ReachText(target.field)));
                continue;
            }
            const auto [lsb, width] = LayoutOf(target.field);
            if (target.field == TargetField::Adr) {
                // ADR counts bytes and splits its immediate: the low two bits
                // sit above the register fields, the rest where every other
                // 19-bit offset does.
                enc_.PatchField(target.instrOffset, 29, 2, static_cast<std::uint32_t>(delta) & 3U);
                enc_.PatchField(target.instrOffset, lsb, width - 2U, static_cast<std::uint32_t>(delta >> 2));
                continue;
            }
            enc_.PatchField(target.instrOffset, lsb, width, static_cast<std::uint32_t>(delta / A64Enc::InstrSize));
        }
    }

    // Data processing

    // ADD / ADDS / SUB / SUBS and the CMP / CMN spelling of the last two. The
    // last operand decides the form: an immediate, a register with an extension,
    // or a register with a shift — which is also the plain register form, since
    // a shift of nothing is LSL #0.
    void EncodeArith(const AsmInstr &in, const ArithForms &forms) {
        Begin(in, forms.discardsResult ? "Rn, #imm | Rn, Rm{, shift #amount}"
                                       : "Rd, Rn, #imm | Rd, Rn, Rm{, shift #amount}");
        const std::size_t base = forms.discardsResult ? 0 : 1;
        if (!Operands(base + 2)) {
            return;
        }
        const AsmOperand &first = in.operands[base];
        const auto rn = RegOf(first);
        if (!rn) {
            return;
        }
        A64Reg rd = ZeroLike(*rn);
        if (!forms.discardsResult) {
            const auto dst = RegOf(in.operands[0]);
            if (!dst) {
                return;
            }
            rd = *dst;
            if (!Uniform(RegClass::General, {Ref(in.operands[0], rd), Ref(first, *rn)})) {
                return;
            }
        }
        // ADD and SUB write SP; the flag-setting pair writes the zero register,
        // which is exactly what makes them CMP and CMN.
        if (!forms.writesStackPointer && !forms.discardsResult && !NoStackPointer({Ref(in.operands[0], rd)})) {
            return;
        }

        const AsmOperand &src = in.operands.back();
        const auto amount = static_cast<unsigned>(src.shiftAmount);
        switch (src.kind) {
        case AsmOperand::Kind::Imm: {
            const std::uint64_t value = ShiftedImm(src);
            if (!CheckArithImm(src, value)) {
                return;
            }
            Emit(in, (enc_.*forms.imm)(rd, *rn, value));
            return;
        }
        case AsmOperand::Kind::Sym: {
            // `ADD Xd, Xn, sym` is the low twelve bits of a symbol's address,
            // the instruction an ADRP is completed by.
            if (in.mnemonic != "add") {
                FormError(src.location, std::format("'{}' takes no symbol operand", in.mnemonic));
                return;
            }
            const std::uint32_t at = Here();
            Emit(in, (enc_.*forms.imm)(rd, *rn, 0));
            result_.fixups.push_back({at, src.name, RcuRelType::AArch64AddAbsLo12Nc, 0});
            return;
        }
        default:
            break;
        }

        const auto rm = RegOf(src);
        if (!rm) {
            return;
        }
        if (src.extend != AsmExtendKind::None) {
            // The extension names how much of the register is read, so every
            // option but the two that read all of it takes a W register.
            if (src.extend != AsmExtendKind::Uxtx && src.extend != AsmExtendKind::Sxtx && rm->Is64()) {
                FormError(src.location,
                          std::format("'{}' extends operand {} with {}, which reads a 32-bit register, found '{}'",
                                      in.mnemonic, IndexOf(src), ExtendName(src.extend), src.name));
                return;
            }
            if (amount > 4) {
                FormError(src.location,
                          std::format("'{}' shifts an extended operand by 0 to 4, found {}", in.mnemonic, amount));
                return;
            }
            if (!NoStackPointer({Ref(src, *rm)})) {
                return;
            }
            Emit(in, (enc_.*forms.extended)(rd, *rn, *rm, ToA64Extend(src.extend), amount));
            return;
        }
        // A plain register form that addresses through SP is the extended one
        // with the extension that reads the whole register, which is how every
        // assembler reads `add x0, sp, x1`: code 31 is the stack pointer there
        // and the zero register in the shifted form.
        if ((rn->IsStackPointer() || rd.IsStackPointer()) && src.shift == AsmShiftKind::None && rm->IsGeneral() &&
            !rm->IsStackPointer() && rm->bits == rn->bits) {
            Emit(in, (enc_.*forms.extended)(rd, *rn, *rm, rn->Is64() ? A64ExtendKind::Uxtx : A64ExtendKind::Uxtw, 0));
            return;
        }
        // The shifted-register form reaches neither reading of SP: code 31 is
        // the zero register in all three of its fields.
        if (!NoStackPointer({Ref(first, *rn), Ref(src, *rm)})) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(first, *rn), Ref(src, *rm)})) {
            return;
        }
        if (src.shift == AsmShiftKind::Ror) {
            // ROR has no encoding in the arithmetic forms, though the logical
            // ones next door accept it.
            FormError(src.location, std::format("'{}' shifts its second source by LSL, LSR or ASR, found {}",
                                                in.mnemonic, ShiftName(src.shift)));
            return;
        }
        if (amount >= rm->bits) {
            FormError(src.location,
                      std::format("'{}' takes a shift of 0 to {}, found {}", in.mnemonic, rm->bits - 1U, amount));
            return;
        }
        Emit(in, (enc_.*forms.shifted)(rd, *rn, *rm, ToA64Shift(src.shift), amount));
    }

    // AND / ORR / EOR / ANDS, their inverting counterparts, and TST.
    void EncodeLogic(const AsmInstr &in, const LogicForms &forms) {
        const bool hasImm = forms.imm != nullptr;
        Begin(in, forms.discardsResult ? "Rn, #imm | Rn, Rm{, shift #amount}"
                  : hasImm             ? "Rd, Rn, #imm | Rd, Rn, Rm{, shift #amount}"
                                       : "Rd, Rn, Rm{, shift #amount}");
        const std::size_t base = forms.discardsResult ? 0 : 1;
        if (!Operands(base + 2)) {
            return;
        }
        const AsmOperand &first = in.operands[base];
        const auto rn = RegOf(first);
        if (!rn) {
            return;
        }
        A64Reg rd = ZeroLike(*rn);
        if (!forms.discardsResult) {
            const auto dst = RegOf(in.operands[0]);
            if (!dst) {
                return;
            }
            rd = *dst;
            if (!Uniform(RegClass::General, {Ref(in.operands[0], rd), Ref(first, *rn)})) {
                return;
            }
        }

        const AsmOperand &src = in.operands.back();
        if (src.kind == AsmOperand::Kind::Imm) {
            if (!hasImm) {
                FormError(src.location, std::format("'{}' has no immediate form", in.mnemonic));
                return;
            }
            // The immediate forms write SP and read the zero register, which is
            // the reverse of what the source operand allows.
            if (!NoStackPointer({Ref(first, *rn)})) {
                return;
            }
            const auto value = static_cast<std::uint64_t>(src.imm);
            if (!CheckLogicalImm(src, value, rd.Is64())) {
                return;
            }
            Emit(in, (enc_.*forms.imm)(rd, *rn, value));
            return;
        }
        const auto rm = RegOf(src);
        if (!rm) {
            return;
        }
        // Every field of the shifted-register form reads code 31 as the zero
        // register, destination included.
        if (!forms.discardsResult && !NoStackPointer({Ref(in.operands[0], rd)})) {
            return;
        }
        if (!NoStackPointer({Ref(first, *rn), Ref(src, *rm)})) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(first, *rn), Ref(src, *rm)})) {
            return;
        }
        const auto amount = static_cast<unsigned>(src.shiftAmount);
        if (amount >= rm->bits) {
            FormError(src.location,
                      std::format("'{}' takes a shift of 0 to {}, found {}", in.mnemonic, rm->bits - 1U, amount));
            return;
        }
        Emit(in, (enc_.*forms.shifted)(rd, *rn, *rm, ToA64Shift(src.shift), amount));
    }

    // MOV, which moves a register or materializes a constant. A constant no
    // single instruction reaches becomes the MOVZ / MOVK chain LoadImm64 picks,
    // which is what `LDR Xd, =value` would have assembled to.
    void EncodeMov(const AsmInstr &in) {
        Begin(in, "Rd, Rn | Rd, #imm");
        if (!Operands(2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        if (!rd) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        if (src.kind == AsmOperand::Kind::Imm) {
            const std::int64_t written = src.imm;
            auto value = static_cast<std::uint64_t>(written);
            if (!rd->Is64()) {
                // A negative constant names the bits a W register holds; a
                // positive one that does not fit is a mistake rather than a
                // value to truncate.
                if (written > 0xFFFFFFFFLL || written < -0x80000000LL) {
                    FormError(src.location, std::format("'{}' takes an immediate a 32-bit register can hold, found {}",
                                                        in.mnemonic, written));
                    return;
                }
                value &= 0xFFFFFFFFULL;
            }
            Emit(in, enc_.LoadImm64(*rd, value));
            return;
        }
        const auto rm = RegOf(src);
        if (!rm) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd), Ref(src, *rm)})) {
            return;
        }
        Emit(in, enc_.Mov(*rd, *rm));
    }

    // MOVZ / MOVN / MOVK, whose immediate is one halfword and whose shift names
    // which halfword that is.
    void EncodeMovw(const AsmInstr &in, const MovwFn fn) {
        Begin(in, "Rd, #imm{, LSL #shift}");
        if (!Operands(2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        if (!rd || !NoStackPointer({Ref(in.operands[0], *rd)})) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        const auto imm = UnsignedImmOf(src, 0xFFFF, "a halfword");
        if (!imm) {
            return;
        }
        const unsigned shift = src.shift == AsmShiftKind::Lsl ? static_cast<unsigned>(src.shiftAmount) : 0U;
        // The shift names which halfword of the register the immediate is, so
        // it is a choice of halfwords rather than a shift amount.
        if (shift % 16U != 0 || shift >= rd->bits) {
            FormError(src.location, std::format("'{}' shifts its halfword by {}, found {}", in.mnemonic,
                                                rd->Is64() ? "0, 16, 32 or 48" : "0 or 16", shift));
            return;
        }
        Emit(in, (enc_.*fn)(*rd, static_cast<std::uint16_t>(*imm), shift));
    }

    // ADR and ADRP. ADR reaches a label in this body directly; ADRP names the
    // page a symbol sits on, which is a link-time quantity whatever the symbol
    // is, so it is always a relocation.
    void EncodeAdr(const AsmInstr &in, const bool page) {
        Begin(in, page ? "Xd, symbol" : "Xd, label");
        if (!Operands(2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        if (!rd) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd)}) || !NoStackPointer({Ref(in.operands[0], *rd)})) {
            return;
        }
        if (!rd->Is64()) {
            FormError(in.operands[0].location,
                      std::format("'{}' forms an address, so its destination is 64-bit, found '{}'", in.mnemonic,
                                  in.operands[0].name));
            return;
        }
        const AsmOperand &target = in.operands[1];
        if (target.kind != AsmOperand::Kind::Sym) {
            FormError(target.location, std::format("'{}' takes a label or a symbol as operand {}, found {}",
                                                   in.mnemonic, IndexOf(target), FoundText(target)));
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, page ? enc_.Adrp(*rd, 0) : enc_.Adr(*rd, 0));
        if (page) {
            result_.fixups.push_back({at, target.name, RcuRelType::AArch64AdrPrelPgHi21, 0});
            return;
        }
        RecordTarget(in, target, at, TargetField::Adr, RcuRelType::None);
    }

    // SBFM / UBFM / BFM and the four aliases, which differ only in what their
    // two immediates mean.
    void EncodeBitfield(const AsmInstr &in, const BitfieldForms &form) {
        Begin(in, form.syntax);
        if (!Operands(4)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        if (!rd || !rn) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn)})) {
            return;
        }
        const bool field = form.field;
        const auto first = BitOf(in.operands[2], *rd, field ? "a bit position" : "a rotate");
        if (!first) {
            return;
        }
        const auto second = field ? UnsignedImmOf(in.operands[3], rd->bits - *first, "a field width")
                                  : BitOf(in.operands[3], *rd, "a bit position");
        if (!second) {
            return;
        }
        if (field && *second == 0) {
            FormError(in.operands[3].location,
                      std::format("'{}' moves at least one bit, found a width of 0", in.mnemonic));
            return;
        }
        Emit(in, (enc_.*form.fn)(*rd, *rn, *first, *second));
    }

    // LSL / LSR / ASR / ROR, which name a constant shift written with an
    // immediate and the variable-register instruction written with a register.
    void EncodeShift(const AsmInstr &in, const ShiftForms &forms) {
        Begin(in, "Rd, Rn, #shift | Rd, Rn, Rm");
        if (!Operands(3)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        if (!rd || !rn) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn)})) {
            return;
        }
        if (in.operands[2].kind == AsmOperand::Kind::Reg) {
            const auto rm = RegOf(in.operands[2]);
            if (!rm) {
                return;
            }
            if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd), Ref(in.operands[2], *rm)}) ||
                !NoStackPointer({Ref(in.operands[2], *rm)})) {
                return;
            }
            Emit(in, (enc_.*forms.variable)(*rd, *rn, *rm));
            return;
        }
        const auto amount = BitOf(in.operands[2], *rd, "a shift");
        if (!amount) {
            return;
        }
        Emit(in, (enc_.*forms.imm)(*rd, *rn, *amount));
    }

    void EncodeExtr(const AsmInstr &in) {
        Begin(in, "Rd, Rn, Rm, #lsb");
        if (!Operands(4)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto rm = RegOf(in.operands[2]);
        if (!rd || !rn || !rm) {
            return;
        }
        if (!Uniform(RegClass::General,
                     {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm)})) {
            return;
        }
        const auto lsb = BitOf(in.operands[3], *rd, "a bit position");
        if (!lsb) {
            return;
        }
        Emit(in, enc_.Extr(*rd, *rn, *rm, *lsb));
    }

    // The register-only shapes, each of which is one table away from its
    // encoder: two registers, three, four, or two with a shift on the second.
    void EncodeReg2(const AsmInstr &in, const Form<Reg2Fn> &form) {
        Begin(in, form.syntax.empty() ? "Rd, Rn" : form.syntax);
        if (!Operands(2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        if (!rd || !rn) {
            return;
        }
        if (!Uniform(form.regClass, {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn)})) {
            return;
        }
        Emit(in, (enc_.*form.fn)(*rd, *rn));
    }

    void EncodeReg3(const AsmInstr &in, const Form<Reg3Fn> &form) {
        Begin(in, form.syntax.empty() ? "Rd, Rn, Rm" : form.syntax);
        if (!Operands(3)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto rm = RegOf(in.operands[2]);
        if (!rd || !rn || !rm) {
            return;
        }
        if (!Uniform(form.regClass, {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm)})) {
            return;
        }
        Emit(in, (enc_.*form.fn)(*rd, *rn, *rm));
    }

    void EncodeReg4(const AsmInstr &in, const Form<Reg4Fn> &form) {
        Begin(in, form.syntax.empty() ? "Rd, Rn, Rm, Ra" : form.syntax);
        if (!Operands(4)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto rm = RegOf(in.operands[2]);
        const auto ra = RegOf(in.operands[3]);
        if (!rd || !rn || !rm || !ra) {
            return;
        }
        if (!Uniform(form.regClass, {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm),
                                     Ref(in.operands[3], *ra)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm),
                             Ref(in.operands[3], *ra)})) {
            return;
        }
        Emit(in, (enc_.*form.fn)(*rd, *rn, *rm, *ra));
    }

    void EncodeReg2Shift(const AsmInstr &in, const Reg2ShiftFn fn) {
        Begin(in, "Rd, Rm{, shift #amount}");
        if (!Operands(2)) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        const auto rd = RegOf(in.operands[0]);
        const auto rm = RegOf(src);
        if (!rd || !rm) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd), Ref(src, *rm)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(src, *rm)})) {
            return;
        }
        const auto amount = static_cast<unsigned>(src.shiftAmount);
        if (amount >= rm->bits) {
            FormError(src.location,
                      std::format("'{}' takes a shift of 0 to {}, found {}", in.mnemonic, rm->bits - 1U, amount));
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *rm, ToA64Shift(src.shift), amount));
    }

    // The conditional group: a select over two registers, the aliases that read
    // one, and the two that read none.
    void EncodeCondSel(const AsmInstr &in, const Form<CondSelFn> &form) {
        Begin(in, form.syntax.empty() ? "Rd, Rn, Rm, cond" : form.syntax);
        if (!Operands(4)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto rm = RegOf(in.operands[2]);
        if (!rd || !rn || !rm) {
            return;
        }
        if (!Uniform(form.regClass, {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm)})) {
            return;
        }
        const auto cond = CondOf(in.operands[3]);
        if (!cond) {
            return;
        }
        Emit(in, (enc_.*form.fn)(*rd, *rn, *rm, *cond));
    }

    void EncodeCondAlias(const AsmInstr &in, const CondAliasFn fn) {
        Begin(in, "Rd, Rn, cond");
        if (!Operands(3)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        if (!rd || !rn) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn)}) ||
            !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn)})) {
            return;
        }
        const auto cond = CondOf(in.operands[2]);
        if (!cond) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *rn, *cond));
    }

    void EncodeCondSet(const AsmInstr &in, const CondSetFn fn) {
        Begin(in, "Rd, cond");
        if (!Operands(2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        if (!rd) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd)}) || !NoStackPointer({Ref(in.operands[0], *rd)})) {
            return;
        }
        const auto cond = CondOf(in.operands[1]);
        if (!cond) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *cond));
    }

    // Loads and stores

    // The register a memory operand addresses through, as the encoder models
    // it. What that register is allowed to be is CheckBase's question.
    [[nodiscard]] std::optional<A64Reg> BaseOf(const AsmOperand &op) {
        if (op.memBase.empty()) {
            FormError(op.location, std::format("'{}' addresses memory through a base register, and this operand names "
                                               "none",
                                               in_->mnemonic));
            return std::nullopt;
        }
        return RegNamed(op.memBase, op.location);
    }

    // The base register of a memory operand, checked as such: it addresses
    // memory, so it is 64-bit in every form and is the one field that reads
    // code 31 as the stack pointer.
    [[nodiscard]] bool CheckBase(const AsmOperand &addr, const A64Reg base) {
        if (base.IsVector() || !base.Is64()) {
            FormError(addr.location, std::format("'{}' addresses memory through a 64-bit general-purpose register, "
                                                 "found '{}'",
                                                 in_->mnemonic, addr.memBase));
            return false;
        }
        return true;
    }

    void EncodeMem(const AsmInstr &in, const MemForms &forms) {
        Begin(in, forms.scaled == nullptr ? "Rt, [Xn{, #imm}]"
                  : forms.literal         ? "Rt, [Xn{, #imm}] | Rt, [Xn, Rm{, extend}] | Rt, label"
                                          : "Rt, [Xn{, #imm}] | Rt, [Xn, Rm{, extend}]");
        if (!Operands(2)) {
            return;
        }
        const auto rt = RegOf(in.operands[0]);
        if (!rt || !NoStackPointer({Ref(in.operands[0], *rt)})) {
            return;
        }
        const AsmOperand &addr = in.operands[1];

        // `LDR Xt, label` reads the value sitting at a label rather than the
        // memory a register addresses.
        if (addr.kind == AsmOperand::Kind::Sym) {
            if (!forms.literal) {
                FormError(addr.location, std::format("'{}' takes a memory operand as operand 2, found {}", in.mnemonic,
                                                     FoundText(addr)));
                return;
            }
            const std::uint32_t at = Here();
            Emit(in, enc_.LdrLiteral(*rt, 0));
            RecordTarget(in, addr, at, TargetField::Imm19, RcuRelType::None);
            return;
        }
        if (addr.kind != AsmOperand::Kind::Mem) {
            FormError(addr.location,
                      std::format("'{}' takes a memory operand as operand 2, found {}", in.mnemonic, FoundText(addr)));
            return;
        }
        const auto rn = BaseOf(addr);
        if (!rn || !CheckBase(addr, *rn)) {
            return;
        }

        // Bytes one access moves, which is the scale of its offset field and so
        // the reach a diagnostic names. The narrowing mnemonics carry it; the
        // rest read it from the register they transfer.
        const unsigned width = forms.accessBytes != 0 ? forms.accessBytes : rt->bits / 8U;

        if (!addr.memIndex.empty()) {
            if (forms.indexed == nullptr) {
                FormError(addr.location, std::format("'{}' has no register-offset form", in.mnemonic));
                return;
            }
            const auto rm = RegNamed(addr.memIndex, addr.location);
            if (!rm) {
                return;
            }
            if (rm->IsVector() || rm->IsStackPointer()) {
                FormError(addr.location, std::format("'{}' indexes through a general-purpose register, found '{}'",
                                                     in.mnemonic, addr.memIndex));
                return;
            }
            // Assembly syntax writes the UXTX option of the encoding as LSL,
            // which is also what an index with no qualifier at all means.
            const bool whole = ExtendsWholeRegister(addr.extend);
            if (whole != rm->Is64()) {
                FormError(addr.location,
                          std::format("'{}' extends its index with {}, which reads a {}-bit register, found '{}'",
                                      in.mnemonic, whole ? "LSL" : ExtendName(addr.extend), whole ? 64 : 32,
                                      addr.memIndex));
                return;
            }
            // The instruction carries a scale bit rather than a shift amount,
            // so the only shift it can express is the one that scales the index
            // by the width of the access.
            const auto amount = static_cast<unsigned>(addr.shiftAmount);
            if (amount != 0 && (1U << amount) != width) {
                FormError(addr.location,
                          std::format("'{}' scales its index by the width of the access, so the shift is 0 or {}, "
                                      "found {}",
                                      in.mnemonic, std::countr_zero(width), amount));
                return;
            }
            const A64ExtendKind extend =
                addr.extend != AsmExtendKind::None ? ToA64Extend(addr.extend) : A64ExtendKind::Uxtx;
            Emit(in, (enc_.*forms.indexed)(*rt, *rn, *rm, extend, amount));
            return;
        }

        if (!addr.memSym.empty()) {
            // `[Xn, sym]` is the low twelve bits of a symbol's address, the
            // access an ADRP is completed by.
            if (forms.scaled == nullptr) {
                FormError(addr.location, std::format("'{}' takes no symbol operand", in.mnemonic));
                return;
            }
            const std::uint32_t at = Here();
            Emit(in, (enc_.*forms.scaled)(*rt, *rn, 0));
            result_.fixups.push_back({at, addr.memSym, RcuRelType::AArch64LdstAbsLo12Nc, 0});
            return;
        }

        const A64IndexMode mode = ToA64IndexMode(addr.indexMode);
        if (forms.scaled != nullptr && mode == A64IndexMode::Offset && addr.imm >= 0) {
            // The scaled form reaches furthest and is the canonical spelling of
            // an offset it can express; anything else — negative, or not a
            // multiple of the access width — is what the unscaled form is for,
            // and choosing between them is the assembler's job rather than the
            // programmer's.
            if ((enc_.*forms.scaled)(*rt, *rn, static_cast<std::uint64_t>(addr.imm)) == A64Status::Ok) {
                return;
            }
        }
        const A64Status status = (enc_.*forms.unscaled)(*rt, *rn, addr.imm, mode);
        if (status == A64Status::OutOfRange || status == A64Status::Unaligned) {
            // Both forms were tried, so the reach the diagnostic names is the
            // two of them together rather than whichever was asked last.
            if (forms.scaled != nullptr && mode == A64IndexMode::Offset) {
                FormError(addr.location,
                          std::format("'{}' takes an offset of -256 to 255, or a multiple of {} from 0 to {}, found {}",
                                      in.mnemonic, width, 4095U * width, addr.imm));
                return;
            }
            FormError(addr.location,
                      std::format("'{}' takes an offset of -256 to 255, found {}", in.mnemonic, addr.imm));
            return;
        }
        Emit(in, status);
    }

    void EncodePair(const AsmInstr &in, const PairFn fn) {
        Begin(in, "Rt, Rt2, [Xn{, #imm}]");
        if (!Operands(3)) {
            return;
        }
        const auto rt = RegOf(in.operands[0]);
        const auto rt2 = RegOf(in.operands[1]);
        if (!rt || !rt2) {
            return;
        }
        if (!Uniform(rt->IsVector() ? RegClass::Float : RegClass::General,
                     {Ref(in.operands[0], *rt), Ref(in.operands[1], *rt2)}) ||
            !NoStackPointer({Ref(in.operands[0], *rt), Ref(in.operands[1], *rt2)})) {
            return;
        }
        const AsmOperand &addr = in.operands[2];
        if (addr.kind != AsmOperand::Kind::Mem) {
            FormError(addr.location,
                      std::format("'{}' takes a memory operand as operand 3, found {}", in.mnemonic, FoundText(addr)));
            return;
        }
        const auto rn = BaseOf(addr);
        if (!rn || !CheckBase(addr, *rn)) {
            return;
        }
        // The offset counts registers rather than bytes, which is what lets one
        // STP open a frame and save the frame chain into it.
        const auto width = static_cast<std::int64_t>(rt->bits / 8U);
        if (addr.imm % width != 0 || addr.imm < -64 * width || addr.imm > 63 * width) {
            FormError(addr.location,
                      std::format("'{}' takes an offset that is a multiple of {} from {} to {}, found {}", in.mnemonic,
                                  width, -64 * width, 63 * width, addr.imm));
            return;
        }
        Emit(in, (enc_.*fn)(*rt, *rt2, *rn, addr.imm, ToA64IndexMode(addr.indexMode)));
    }

    // Branches

    void EncodeBranch(const AsmInstr &in, const bool link) {
        Begin(in, "label");
        if (!Operands(1)) {
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, link ? enc_.Bl(0) : enc_.B(0));
        RecordTarget(in, in.operands[0], at, TargetField::Imm26,
                     link ? RcuRelType::AArch64Call26 : RcuRelType::AArch64Jump26);
    }

    void EncodeCondBranch(const AsmInstr &in, const std::string_view suffix) {
        Begin(in, "label");
        const auto cond = ConditionFromName(suffix);
        if (!cond) {
            FormError(in.location, std::format("unknown condition '{}' in '{}'", suffix, in.mnemonic));
            return;
        }
        if (!Operands(1)) {
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, enc_.BCond(*cond, 0));
        RecordTarget(in, in.operands[0], at, TargetField::Imm19, RcuRelType::AArch64CondBr19);
    }

    void EncodeCompareBranch(const AsmInstr &in, const CompareBranchFn fn) {
        Begin(in, "Rt, label");
        if (!Operands(2)) {
            return;
        }
        const auto rt = RegOf(in.operands[0]);
        if (!rt) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rt)}) || !NoStackPointer({Ref(in.operands[0], *rt)})) {
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, (enc_.*fn)(*rt, 0));
        RecordTarget(in, in.operands[1], at, TargetField::Imm19, RcuRelType::AArch64CondBr19);
    }

    void EncodeTestBranch(const AsmInstr &in, const TestBranchFn fn) {
        Begin(in, "Rt, #bit, label");
        if (!Operands(3)) {
            return;
        }
        const auto rt = RegOf(in.operands[0]);
        if (!rt) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rt)}) || !NoStackPointer({Ref(in.operands[0], *rt)})) {
            return;
        }
        const auto bit = BitOf(in.operands[1], *rt, "a bit number");
        if (!bit) {
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, (enc_.*fn)(*rt, *bit, 0));
        RecordTarget(in, in.operands[2], at, TargetField::Imm14, RcuRelType::AArch64TstBr14);
    }

    // BR, BLR and RET, the last of which returns through X30 when it names no
    // register at all.
    void EncodeBranchReg(const AsmInstr &in, const Reg1Fn fn, const bool optional) {
        Begin(in, optional ? "{Xn}" : "Xn");
        if (optional && in.operands.empty()) {
            Emit(in, (enc_.*fn)(A64::Lr));
            return;
        }
        if (!Operands(1)) {
            return;
        }
        const auto rn = RegOf(in.operands[0]);
        if (!rn) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(in.operands[0], *rn)}) || !NoStackPointer({Ref(in.operands[0], *rn)})) {
            return;
        }
        if (!rn->Is64()) {
            FormError(in.operands[0].location,
                      std::format("'{}' branches to an address, so its operand is 64-bit, found '{}'", in.mnemonic,
                                  in.operands[0].name));
            return;
        }
        Emit(in, (enc_.*fn)(*rn));
    }

    // System

    void EncodeException(const AsmInstr &in, const Imm16Fn fn) {
        Begin(in, "#imm");
        if (!Operands(1)) {
            return;
        }
        const auto imm = UnsignedImmOf(in.operands[0], 0xFFFF, "an exception code");
        if (!imm) {
            return;
        }
        Emit(in, (enc_.*fn)(static_cast<std::uint16_t>(*imm)));
    }

    void EncodeBarrier(const AsmInstr &in, const BarrierFn fn) {
        Begin(in, "{option}");
        if (!Operands(0, 1)) {
            return;
        }
        if (in.operands.empty()) {
            Emit(in, (enc_.*fn)(A64Barrier::Sy));
            return;
        }
        const AsmOperand &option = in.operands[0];
        if (option.kind != AsmOperand::Kind::Sym) {
            FormError(option.location,
                      std::format("'{}' takes a barrier option, found {}", in.mnemonic, FoundText(option)));
            return;
        }
        const auto barrier = BarrierFromName(Lowered(option.name));
        if (!barrier) {
            FormError(option.location, std::format("unknown barrier option '{}'", option.name));
            return;
        }
        Emit(in, (enc_.*fn)(*barrier));
    }

    [[nodiscard]] std::optional<std::uint16_t> SysRegOf(const AsmOperand &op) {
        if (op.kind != AsmOperand::Kind::Sym) {
            FormError(op.location, std::format("'{}' takes a system register as operand {}, found {}", in_->mnemonic,
                                               IndexOf(op), FoundText(op)));
            return std::nullopt;
        }
        if (const auto sysreg = SysRegFromName(Lowered(op.name))) {
            return sysreg;
        }
        FormError(op.location, std::format("unknown system register '{}'", op.name));
        return std::nullopt;
    }

    void EncodeSysMove(const AsmInstr &in, const bool read) {
        Begin(in, read ? "Xt, sysreg" : "sysreg, Xt");
        if (!Operands(2)) {
            return;
        }
        const AsmOperand &value = in.operands[read ? 0 : 1];
        const auto rt = RegOf(value);
        if (!rt) {
            return;
        }
        if (!Uniform(RegClass::General, {Ref(value, *rt)}) || !NoStackPointer({Ref(value, *rt)})) {
            return;
        }
        if (!rt->Is64()) {
            FormError(value.location, std::format("'{}' moves a whole system register, so its operand is 64-bit, "
                                                  "found '{}'",
                                                  in.mnemonic, value.name));
            return;
        }
        const auto sysreg = SysRegOf(in.operands[read ? 1 : 0]);
        if (!sysreg) {
            return;
        }
        Emit(in, read ? enc_.Mrs(*rt, *sysreg) : enc_.Msr(*sysreg, *rt));
    }

    // Scalar floating point

    // FMOV, which moves between registers of either file and is the one
    // floating-point instruction that also names a value.
    void EncodeFmov(const AsmInstr &in) {
        Begin(in, "Vd, Vn | Vd, Rn | Rd, Vn | Vd, #imm");
        if (!Operands(2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        if (!rd) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        if (src.kind == AsmOperand::Kind::Imm) {
            // The lexer has no floating-point literal inside an `asm func`
            // body, so the value is written as the integer it equals; anything
            // outside the 256 the field encodes is refused rather than rounded.
            Emit(in, enc_.FmovImm(*rd, static_cast<double>(src.imm)));
            return;
        }
        const auto rn = RegOf(src);
        if (!rn) {
            return;
        }
        Emit(in, enc_.Fmov(*rd, *rn));
    }

    // FCMP and FCMPE, whose second operand is either a register or the zero the
    // dedicated form compares against.
    void EncodeFcmp(const AsmInstr &in, const bool signalling) {
        Begin(in, "Vn, Vm | Vn, #0");
        if (!Operands(2)) {
            return;
        }
        const auto rn = RegOf(in.operands[0]);
        if (!rn) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        if (src.kind == AsmOperand::Kind::Imm) {
            if (src.imm != 0) {
                FormError(src.location, std::format("'{}' compares against a register or against zero, found {}",
                                                    in.mnemonic, src.imm));
                return;
            }
            if (!Uniform(RegClass::Float, {Ref(in.operands[0], *rn)})) {
                return;
            }
            Emit(in, signalling ? enc_.FcmpeZero(*rn) : enc_.FcmpZero(*rn));
            return;
        }
        const auto rm = RegOf(src);
        if (!rm) {
            return;
        }
        if (!Uniform(RegClass::Float, {Ref(in.operands[0], *rn), Ref(src, *rm)})) {
            return;
        }
        Emit(in, signalling ? enc_.Fcmpe(*rn, *rm) : enc_.Fcmp(*rn, *rm));
    }

    void EncodeFccmp(const AsmInstr &in) {
        Begin(in, "Vn, Vm, #nzcv, cond");
        if (!Operands(4)) {
            return;
        }
        const auto rn = RegOf(in.operands[0]);
        const auto rm = RegOf(in.operands[1]);
        if (!rn || !rm) {
            return;
        }
        if (!Uniform(RegClass::Float, {Ref(in.operands[0], *rn), Ref(in.operands[1], *rm)})) {
            return;
        }
        const auto nzcv = UnsignedImmOf(in.operands[2], 15, "a flag value");
        if (!nzcv) {
            return;
        }
        const auto cond = CondOf(in.operands[3]);
        if (!cond) {
            return;
        }
        Emit(in, enc_.Fccmp(*rn, *rm, *nzcv, *cond));
    }

    // Dispatch

    // A mnemonic no table above claimed, which is one of two mistakes: an
    // instruction the architecture has and these encoders do not, or a name the
    // architecture does not have at all. The second is a misspelling far more
    // often than it is a real instruction, so it is worth naming what the body
    // probably meant.
    void Unsupported(const AsmInstr &in) {
        if (IsAsmMnemonic(Target::Arch::AArch64, in.mnemonic)) {
            Error(in.location, std::format("unsupported instruction '{}'", in.mnemonic));
            return;
        }
        if (const auto closest = ClosestAsmMnemonic(Target::Arch::AArch64, in.mnemonic)) {
            Error(in.location, std::format("unknown instruction '{}'; did you mean '{}'?", in.mnemonic, *closest));
            return;
        }
        Error(in.location, std::format("unknown instruction '{}'", in.mnemonic));
    }

    void Dispatch(const AsmInstr &in) {
        const std::string &m = in.mnemonic;

        // A conditional branch carries its condition in its name, so it is the
        // one mnemonic that is not looked up whole.
        if (const std::string_view base = AsmBaseMnemonic(m); base.size() != m.size()) {
            if (base == "b") {
                EncodeCondBranch(in, std::string_view(m).substr(base.size() + 1));
                return;
            }
            Unsupported(in);
            return;
        }

        // ADD and SUB read and write SP; the flag-setting pair reads it and
        // writes the zero register, which is what makes them CMN and CMP.
        static const std::unordered_map<std::string_view, ArithForms> arith = {
            {"add", {&A64Enc::AddImm, &A64Enc::Add, &A64Enc::AddExt, false, true}},
            {"adds", {&A64Enc::AddsImm, &A64Enc::Adds, &A64Enc::AddsExt}},
            {"sub", {&A64Enc::SubImm, &A64Enc::Sub, &A64Enc::SubExt, false, true}},
            {"subs", {&A64Enc::SubsImm, &A64Enc::Subs, &A64Enc::SubsExt}},
            {"cmn", {&A64Enc::AddsImm, &A64Enc::Adds, &A64Enc::AddsExt, true}},
            {"cmp", {&A64Enc::SubsImm, &A64Enc::Subs, &A64Enc::SubsExt, true}},
        };
        if (const auto *forms = Lookup(arith, m)) {
            EncodeArith(in, *forms);
            return;
        }

        static const std::unordered_map<std::string_view, LogicForms> logic = {
            {"and", {&A64Enc::AndImm, &A64Enc::And}},
            {"ands", {&A64Enc::AndsImm, &A64Enc::Ands}},
            {"orr", {&A64Enc::OrrImm, &A64Enc::Orr}},
            {"eor", {&A64Enc::EorImm, &A64Enc::Eor}},
            {"bic", {nullptr, &A64Enc::Bic}},
            {"bics", {nullptr, &A64Enc::Bics}},
            {"orn", {nullptr, &A64Enc::Orn}},
            {"eon", {nullptr, &A64Enc::Eon}},
            {"tst", {&A64Enc::AndsImm, &A64Enc::Ands, true}},
        };
        if (const auto *forms = Lookup(logic, m)) {
            EncodeLogic(in, *forms);
            return;
        }

        // The fourth field is the width of one access, which the narrowing and
        // sign-extending mnemonics carry themselves and the rest read from the
        // register they transfer.
        static const std::unordered_map<std::string_view, MemForms> mem = {
            {"ldr", {&A64Enc::Ldr, &A64Enc::Ldur, &A64Enc::LdrReg, 0, true}},
            {"str", {&A64Enc::Str, &A64Enc::Stur, &A64Enc::StrReg}},
            {"ldrb", {&A64Enc::Ldrb, &A64Enc::Ldurb, &A64Enc::LdrbReg, 1}},
            {"strb", {&A64Enc::Strb, &A64Enc::Sturb, &A64Enc::StrbReg, 1}},
            {"ldrh", {&A64Enc::Ldrh, &A64Enc::Ldurh, &A64Enc::LdrhReg, 2}},
            {"strh", {&A64Enc::Strh, &A64Enc::Sturh, &A64Enc::StrhReg, 2}},
            {"ldrsb", {&A64Enc::Ldrsb, &A64Enc::Ldursb, &A64Enc::LdrsbReg, 1}},
            {"ldrsh", {&A64Enc::Ldrsh, &A64Enc::Ldursh, &A64Enc::LdrshReg, 2}},
            {"ldrsw", {&A64Enc::Ldrsw, &A64Enc::Ldursw, &A64Enc::LdrswReg, 4}},
            {"ldur", {nullptr, &A64Enc::Ldur, nullptr}},
            {"stur", {nullptr, &A64Enc::Stur, nullptr}},
            {"ldurb", {nullptr, &A64Enc::Ldurb, nullptr, 1}},
            {"sturb", {nullptr, &A64Enc::Sturb, nullptr, 1}},
            {"ldurh", {nullptr, &A64Enc::Ldurh, nullptr, 2}},
            {"sturh", {nullptr, &A64Enc::Sturh, nullptr, 2}},
            {"ldursb", {nullptr, &A64Enc::Ldursb, nullptr, 1}},
            {"ldursh", {nullptr, &A64Enc::Ldursh, nullptr, 2}},
            {"ldursw", {nullptr, &A64Enc::Ldursw, nullptr, 4}},
        };
        if (const auto *forms = Lookup(mem, m)) {
            EncodeMem(in, *forms);
            return;
        }

        // The extensions and the conversions are the forms whose two operands
        // deliberately disagree — in width, in file, or in both — so they name
        // their own shape and leave the checking to the encoder.
        static const std::unordered_map<std::string_view, Form<Reg2Fn>> reg2 = {
            {"sxtb", {&A64Enc::Sxtb, "Rd, Wn", RegClass::Mixed}},
            {"sxth", {&A64Enc::Sxth, "Rd, Wn", RegClass::Mixed}},
            {"sxtw", {&A64Enc::Sxtw, "Xd, Wn", RegClass::Mixed}},
            {"uxtb", {&A64Enc::Uxtb, "Wd, Wn", RegClass::Mixed}},
            {"uxth", {&A64Enc::Uxth, "Wd, Wn", RegClass::Mixed}},
            {"clz", {&A64Enc::Clz}},
            {"cls", {&A64Enc::Cls}},
            {"rbit", {&A64Enc::Rbit}},
            {"rev", {&A64Enc::Rev}},
            {"rev16", {&A64Enc::Rev16}},
            {"rev32", {&A64Enc::Rev32, "Xd, Xn"}},
            {"fneg", {&A64Enc::Fneg, "Vd, Vn", RegClass::Float}},
            {"fabs", {&A64Enc::Fabs, "Vd, Vn", RegClass::Float}},
            {"fsqrt", {&A64Enc::Fsqrt, "Vd, Vn", RegClass::Float}},
            {"fcvt", {&A64Enc::Fcvt, "Vd, Vn", RegClass::Mixed}},
            {"fcvtzs", {&A64Enc::Fcvtzs, "Rd, Vn", RegClass::Mixed}},
            {"fcvtzu", {&A64Enc::Fcvtzu, "Rd, Vn", RegClass::Mixed}},
            {"scvtf", {&A64Enc::Scvtf, "Vd, Rn", RegClass::Mixed}},
            {"ucvtf", {&A64Enc::Ucvtf, "Vd, Rn", RegClass::Mixed}},
            {"frinta", {&A64Enc::Frinta, "Vd, Vn", RegClass::Float}},
            {"frintm", {&A64Enc::Frintm, "Vd, Vn", RegClass::Float}},
            {"frintn", {&A64Enc::Frintn, "Vd, Vn", RegClass::Float}},
            {"frintp", {&A64Enc::Frintp, "Vd, Vn", RegClass::Float}},
            {"frintz", {&A64Enc::Frintz, "Vd, Vn", RegClass::Float}},
        };
        if (const auto *form = Lookup(reg2, m)) {
            EncodeReg2(in, *form);
            return;
        }

        static const std::unordered_map<std::string_view, Form<Reg3Fn>> reg3 = {
            {"lslv", {&A64Enc::Lslv}},
            {"lsrv", {&A64Enc::Lsrv}},
            {"asrv", {&A64Enc::Asrv}},
            {"rorv", {&A64Enc::Rorv}},
            {"sdiv", {&A64Enc::Sdiv}},
            {"udiv", {&A64Enc::Udiv}},
            {"mul", {&A64Enc::Mul}},
            {"mneg", {&A64Enc::Mneg}},
            {"smulh", {&A64Enc::Smulh, "Xd, Xn, Xm"}},
            {"umulh", {&A64Enc::Umulh, "Xd, Xn, Xm"}},
            // The widening multiplies read two W registers into an X one.
            {"smull", {&A64Enc::Smull, "Xd, Wn, Wm", RegClass::Mixed}},
            {"umull", {&A64Enc::Umull, "Xd, Wn, Wm", RegClass::Mixed}},
            {"fadd", {&A64Enc::Fadd, "Vd, Vn, Vm", RegClass::Float}},
            {"fsub", {&A64Enc::Fsub, "Vd, Vn, Vm", RegClass::Float}},
            {"fmul", {&A64Enc::Fmul, "Vd, Vn, Vm", RegClass::Float}},
            {"fdiv", {&A64Enc::Fdiv, "Vd, Vn, Vm", RegClass::Float}},
            {"fmax", {&A64Enc::Fmax, "Vd, Vn, Vm", RegClass::Float}},
            {"fmin", {&A64Enc::Fmin, "Vd, Vn, Vm", RegClass::Float}},
            {"fmaxnm", {&A64Enc::Fmaxnm, "Vd, Vn, Vm", RegClass::Float}},
            {"fminnm", {&A64Enc::Fminnm, "Vd, Vn, Vm", RegClass::Float}},
        };
        if (const auto *form = Lookup(reg3, m)) {
            EncodeReg3(in, *form);
            return;
        }

        static const std::unordered_map<std::string_view, Form<Reg4Fn>> reg4 = {
            {"madd", {&A64Enc::Madd}},
            {"msub", {&A64Enc::Msub}},
            {"smaddl", {&A64Enc::Smaddl, "Xd, Wn, Wm, Xa", RegClass::Mixed}},
            {"umaddl", {&A64Enc::Umaddl, "Xd, Wn, Wm, Xa", RegClass::Mixed}},
            {"fmadd", {&A64Enc::Fmadd, "Vd, Vn, Vm, Va", RegClass::Float}},
            {"fmsub", {&A64Enc::Fmsub, "Vd, Vn, Vm, Va", RegClass::Float}},
            {"fnmadd", {&A64Enc::Fnmadd, "Vd, Vn, Vm, Va", RegClass::Float}},
            {"fnmsub", {&A64Enc::Fnmsub, "Vd, Vn, Vm, Va", RegClass::Float}},
        };
        if (const auto *form = Lookup(reg4, m)) {
            EncodeReg4(in, *form);
            return;
        }

        static const std::unordered_map<std::string_view, Reg2ShiftFn> reg2Shift = {
            {"neg", &A64Enc::Neg},
            {"negs", &A64Enc::Negs},
            {"mvn", &A64Enc::Mvn},
        };
        if (const auto *fn = Lookup(reg2Shift, m)) {
            EncodeReg2Shift(in, *fn);
            return;
        }

        static const std::unordered_map<std::string_view, BitfieldForms> bitfield = {
            {"sbfm", {&A64Enc::Sbfm, "Rd, Rn, #immr, #imms"}},
            {"ubfm", {&A64Enc::Ubfm, "Rd, Rn, #immr, #imms"}},
            {"bfm", {&A64Enc::Bfm, "Rd, Rn, #immr, #imms"}},
            {"sbfx", {&A64Enc::Sbfx, "Rd, Rn, #lsb, #width", true}},
            {"ubfx", {&A64Enc::Ubfx, "Rd, Rn, #lsb, #width", true}},
            {"bfi", {&A64Enc::Bfi, "Rd, Rn, #lsb, #width", true}},
            {"bfxil", {&A64Enc::Bfxil, "Rd, Rn, #lsb, #width", true}},
        };
        if (const auto *form = Lookup(bitfield, m)) {
            EncodeBitfield(in, *form);
            return;
        }

        static const std::unordered_map<std::string_view, ShiftForms> shift = {
            {"lsl", {&A64Enc::Lsl, &A64Enc::Lslv}},
            {"lsr", {&A64Enc::Lsr, &A64Enc::Lsrv}},
            {"asr", {&A64Enc::Asr, &A64Enc::Asrv}},
            {"ror", {&A64Enc::Ror, &A64Enc::Rorv}},
        };
        if (const auto *forms = Lookup(shift, m)) {
            EncodeShift(in, *forms);
            return;
        }

        static const std::unordered_map<std::string_view, MovwFn> movw = {
            {"movz", &A64Enc::Movz},
            {"movn", &A64Enc::Movn},
            {"movk", &A64Enc::Movk},
        };
        if (const auto *fn = Lookup(movw, m)) {
            EncodeMovw(in, *fn);
            return;
        }

        static const std::unordered_map<std::string_view, Form<CondSelFn>> condSel = {
            {"csel", {&A64Enc::Csel}},
            {"csinc", {&A64Enc::Csinc}},
            {"csinv", {&A64Enc::Csinv}},
            {"csneg", {&A64Enc::Csneg}},
            {"fcsel", {&A64Enc::Fcsel, "Vd, Vn, Vm, cond", RegClass::Float}},
        };
        if (const auto *form = Lookup(condSel, m)) {
            EncodeCondSel(in, *form);
            return;
        }

        static const std::unordered_map<std::string_view, CondAliasFn> condAlias = {
            {"cinc", &A64Enc::Cinc},
            {"cinv", &A64Enc::Cinv},
            {"cneg", &A64Enc::Cneg},
        };
        if (const auto *fn = Lookup(condAlias, m)) {
            EncodeCondAlias(in, *fn);
            return;
        }

        static const std::unordered_map<std::string_view, CondSetFn> condSet = {
            {"cset", &A64Enc::Cset},
            {"csetm", &A64Enc::Csetm},
        };
        if (const auto *fn = Lookup(condSet, m)) {
            EncodeCondSet(in, *fn);
            return;
        }

        static const std::unordered_map<std::string_view, Imm16Fn> exception = {
            {"svc", &A64Enc::Svc},
            {"brk", &A64Enc::Brk},
            {"hlt", &A64Enc::Hlt},
            {"udf", &A64Enc::Udf},
        };
        if (const auto *fn = Lookup(exception, m)) {
            EncodeException(in, *fn);
            return;
        }

        static const std::unordered_map<std::string_view, BarrierFn> barrier = {
            {"dmb", &A64Enc::Dmb},
            {"dsb", &A64Enc::Dsb},
            {"isb", &A64Enc::Isb},
        };
        if (const auto *fn = Lookup(barrier, m)) {
            EncodeBarrier(in, *fn);
            return;
        }

        if (m == "mov") {
            EncodeMov(in);
            return;
        }
        if (m == "adr" || m == "adrp") {
            EncodeAdr(in, m == "adrp");
            return;
        }
        if (m == "extr") {
            EncodeExtr(in);
            return;
        }
        if (m == "ldp") {
            EncodePair(in, &A64Enc::Ldp);
            return;
        }
        if (m == "stp") {
            EncodePair(in, &A64Enc::Stp);
            return;
        }
        if (m == "b" || m == "bl") {
            EncodeBranch(in, m == "bl");
            return;
        }
        if (m == "cbz") {
            EncodeCompareBranch(in, &A64Enc::Cbz);
            return;
        }
        if (m == "cbnz") {
            EncodeCompareBranch(in, &A64Enc::Cbnz);
            return;
        }
        if (m == "tbz") {
            EncodeTestBranch(in, &A64Enc::Tbz);
            return;
        }
        if (m == "tbnz") {
            EncodeTestBranch(in, &A64Enc::Tbnz);
            return;
        }
        if (m == "br") {
            EncodeBranchReg(in, &A64Enc::Br, false);
            return;
        }
        if (m == "blr") {
            EncodeBranchReg(in, &A64Enc::Blr, false);
            return;
        }
        if (m == "ret") {
            EncodeBranchReg(in, &A64Enc::Ret, true);
            return;
        }
        if (m == "nop") {
            Begin(in, "");
            if (Operands(0)) {
                Emit(in, enc_.Nop());
            }
            return;
        }
        if (m == "hint") {
            Begin(in, "#imm");
            if (!Operands(1)) {
                return;
            }
            if (const auto imm = UnsignedImmOf(in.operands[0], 127, "a hint number")) {
                Emit(in, enc_.Hint(*imm));
            }
            return;
        }
        if (m == "mrs" || m == "msr") {
            EncodeSysMove(in, m == "mrs");
            return;
        }
        if (m == "fmov") {
            EncodeFmov(in);
            return;
        }
        if (m == "fcmp" || m == "fcmpe") {
            EncodeFcmp(in, m == "fcmpe");
            return;
        }
        if (m == "fccmp") {
            EncodeFccmp(in);
            return;
        }

        Unsupported(in);
    }
};
} // namespace

AsmAssembly AssembleAArch64AsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName, Bytes &out) {
    Assembler asmr(instrs, sourceName, out);
    return asmr.Run();
}
} // namespace Rux
