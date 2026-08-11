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

#include "CodeGen/AArch64/Assembler.h"

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"
#include "Object/Rcu/Rcu.h"

#include <cctype>
#include <cstdint>
#include <format>
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

// ADD / SUB and their flag-setting forms, which have all three source shapes;
// CMP and CMN are the same instructions with the result discarded.
struct ArithForms {
    RegRegImmFn imm;
    ShiftedFn shifted;
    ExtendedFn extended;
    bool discardsResult = false;
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
    bool literal = false;
};

// A shift by a constant and the variable-register instruction the same mnemonic
// means when its last operand is a register.
struct ShiftForms {
    ShiftImmFn imm;
    Reg3Fn variable;
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

    void Error(const SourceLocation &loc, std::string msg) {
        Diagnostic d;
        d.severity = Diagnostic::Severity::Error;
        d.message = std::move(msg);
        d.location = loc;
        d.sourceName = sourceName_;
        result_.diagnostics.push_back(std::move(d));
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

    [[nodiscard]] bool Operands(const AsmInstr &in, const std::size_t count) {
        if (in.operands.size() == count) {
            return true;
        }
        Error(in.location, std::format("'{}' expects {} operand{}, found {}", in.mnemonic, count, count == 1 ? "" : "s",
                                       in.operands.size()));
        return false;
    }

    [[nodiscard]] bool Operands(const AsmInstr &in, const std::size_t least, const std::size_t most) {
        if (in.operands.size() >= least && in.operands.size() <= most) {
            return true;
        }
        Error(in.location,
              std::format("'{}' expects {} to {} operands, found {}", in.mnemonic, least, most, in.operands.size()));
        return false;
    }

    [[nodiscard]] std::optional<A64Reg> RegNamed(const std::string &name, const SourceLocation &loc) {
        const AsmRegInfo info = LookupRegister(Target::Arch::AArch64, name);
        if (!info.valid) {
            Error(loc, std::format("unknown register '{}'", name));
            return std::nullopt;
        }
        return ToA64Reg(info);
    }

    [[nodiscard]] std::optional<A64Reg> RegOf(const AsmOperand &op) {
        if (op.kind != AsmOperand::Kind::Reg) {
            Error(op.location, "expected a register");
            return std::nullopt;
        }
        return RegNamed(op.name, op.location);
    }

    [[nodiscard]] std::optional<std::int64_t> ImmOf(const AsmOperand &op) {
        if (op.kind != AsmOperand::Kind::Imm) {
            Error(op.location, "expected an immediate");
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

    [[nodiscard]] std::optional<unsigned> UnsignedImmOf(const AsmOperand &op, const std::uint64_t limit) {
        const auto value = ImmOf(op);
        if (!value) {
            return std::nullopt;
        }
        if (*value < 0 || static_cast<std::uint64_t>(*value) > limit) {
            Error(op.location, std::format("immediate {} is outside the range 0 to {}", *value, limit));
            return std::nullopt;
        }
        return static_cast<unsigned>(*value);
    }

    [[nodiscard]] std::optional<A64Condition> CondOf(const AsmOperand &op) {
        if (op.kind != AsmOperand::Kind::Sym) {
            Error(op.location, "expected a condition");
            return std::nullopt;
        }
        if (const auto cond = ConditionFromName(Lowered(op.name))) {
            return cond;
        }
        Error(op.location, std::format("unknown condition '{}'", op.name));
        return std::nullopt;
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
            Error(target.location, std::format("'{}' expects a label or a symbol", in.mnemonic));
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
        if (!Operands(in, forms.discardsResult ? 2 : 3)) {
            return;
        }
        const auto rn = RegOf(in.operands[forms.discardsResult ? 0 : 1]);
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
        }

        const AsmOperand &src = in.operands.back();
        const auto amount = static_cast<unsigned>(src.shiftAmount);
        switch (src.kind) {
        case AsmOperand::Kind::Imm:
            Emit(in, (enc_.*forms.imm)(rd, *rn, ShiftedImm(src)));
            return;
        case AsmOperand::Kind::Sym: {
            // `ADD Xd, Xn, sym` is the low twelve bits of a symbol's address,
            // the instruction an ADRP is completed by.
            if (in.mnemonic != "add") {
                Error(src.location, std::format("'{}' does not take a symbol", in.mnemonic));
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
            Emit(in, (enc_.*forms.extended)(rd, *rn, *rm, ToA64Extend(src.extend), amount));
            return;
        }
        Emit(in, (enc_.*forms.shifted)(rd, *rn, *rm, ToA64Shift(src.shift), amount));
    }

    // AND / ORR / EOR / ANDS, their inverting counterparts, and TST.
    void EncodeLogic(const AsmInstr &in, const LogicForms &forms) {
        if (!Operands(in, forms.discardsResult ? 2 : 3)) {
            return;
        }
        const auto rn = RegOf(in.operands[forms.discardsResult ? 0 : 1]);
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
        }

        const AsmOperand &src = in.operands.back();
        if (src.kind == AsmOperand::Kind::Imm) {
            if (forms.imm == nullptr) {
                Error(src.location, std::format("'{}' has no immediate form", in.mnemonic));
                return;
            }
            Emit(in, (enc_.*forms.imm)(rd, *rn, static_cast<std::uint64_t>(src.imm)));
            return;
        }
        const auto rm = RegOf(src);
        if (!rm) {
            return;
        }
        Emit(in, (enc_.*forms.shifted)(rd, *rn, *rm, ToA64Shift(src.shift), static_cast<unsigned>(src.shiftAmount)));
    }

    // MOV, which moves a register or materializes a constant. A constant no
    // single instruction reaches becomes the MOVZ / MOVK chain LoadImm64 picks,
    // which is what `LDR Xd, =value` would have assembled to.
    void EncodeMov(const AsmInstr &in) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        if (!rd) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        if (src.kind == AsmOperand::Kind::Imm) {
            auto value = static_cast<std::uint64_t>(src.imm);
            if (!rd->Is64()) {
                value &= 0xFFFFFFFFULL;
            }
            Emit(in, enc_.LoadImm64(*rd, value));
            return;
        }
        const auto rm = RegOf(src);
        if (!rm) {
            return;
        }
        Emit(in, enc_.Mov(*rd, *rm));
    }

    // MOVZ / MOVN / MOVK, whose immediate is one halfword and whose shift names
    // which halfword that is.
    void EncodeMovw(const AsmInstr &in, const MovwFn fn) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto imm = UnsignedImmOf(in.operands[1], 0xFFFF);
        if (!rd || !imm) {
            return;
        }
        const unsigned shift =
            in.operands[1].shift == AsmShiftKind::Lsl ? static_cast<unsigned>(in.operands[1].shiftAmount) : 0U;
        Emit(in, (enc_.*fn)(*rd, static_cast<std::uint16_t>(*imm), shift));
    }

    // ADR and ADRP. ADR reaches a label in this body directly; ADRP names the
    // page a symbol sits on, which is a link-time quantity whatever the symbol
    // is, so it is always a relocation.
    void EncodeAdr(const AsmInstr &in, const bool page) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        if (!rd) {
            return;
        }
        const AsmOperand &target = in.operands[1];
        if (target.kind != AsmOperand::Kind::Sym) {
            Error(target.location, std::format("'{}' expects a label or a symbol", in.mnemonic));
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
    void EncodeBitfield(const AsmInstr &in, const BitfieldFn fn) {
        if (!Operands(in, 4)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto first = UnsignedImmOf(in.operands[2], 63);
        const auto second = UnsignedImmOf(in.operands[3], 64);
        if (!rd || !rn || !first || !second) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *rn, *first, *second));
    }

    // LSL / LSR / ASR / ROR, which name a constant shift written with an
    // immediate and the variable-register instruction written with a register.
    void EncodeShift(const AsmInstr &in, const ShiftForms &forms) {
        if (!Operands(in, 3)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        if (!rd || !rn) {
            return;
        }
        if (in.operands[2].kind == AsmOperand::Kind::Reg) {
            const auto rm = RegOf(in.operands[2]);
            if (!rm) {
                return;
            }
            Emit(in, (enc_.*forms.variable)(*rd, *rn, *rm));
            return;
        }
        const auto amount = UnsignedImmOf(in.operands[2], 63);
        if (!amount) {
            return;
        }
        Emit(in, (enc_.*forms.imm)(*rd, *rn, *amount));
    }

    void EncodeExtr(const AsmInstr &in) {
        if (!Operands(in, 4)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto rm = RegOf(in.operands[2]);
        const auto lsb = UnsignedImmOf(in.operands[3], 63);
        if (!rd || !rn || !rm || !lsb) {
            return;
        }
        Emit(in, enc_.Extr(*rd, *rn, *rm, *lsb));
    }

    // The register-only shapes, each of which is one table away from its
    // encoder: two registers, three, four, or two with a shift on the second.
    void EncodeReg2(const AsmInstr &in, const Reg2Fn fn) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        if (!rd || !rn) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *rn));
    }

    void EncodeReg3(const AsmInstr &in, const Reg3Fn fn) {
        if (!Operands(in, 3)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto rm = RegOf(in.operands[2]);
        if (!rd || !rn || !rm) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *rn, *rm));
    }

    void EncodeReg4(const AsmInstr &in, const Reg4Fn fn) {
        if (!Operands(in, 4)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto rm = RegOf(in.operands[2]);
        const auto ra = RegOf(in.operands[3]);
        if (!rd || !rn || !rm || !ra) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *rn, *rm, *ra));
    }

    void EncodeReg2Shift(const AsmInstr &in, const Reg2ShiftFn fn) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rm = RegOf(in.operands[1]);
        if (!rd || !rm) {
            return;
        }
        Emit(in,
             (enc_.*fn)(*rd, *rm, ToA64Shift(in.operands[1].shift), static_cast<unsigned>(in.operands[1].shiftAmount)));
    }

    // The conditional group: a select over two registers, the aliases that read
    // one, and the two that read none.
    void EncodeCondSel(const AsmInstr &in, const CondSelFn fn) {
        if (!Operands(in, 4)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto rm = RegOf(in.operands[2]);
        const auto cond = CondOf(in.operands[3]);
        if (!rd || !rn || !rm || !cond) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *rn, *rm, *cond));
    }

    void EncodeCondAlias(const AsmInstr &in, const CondAliasFn fn) {
        if (!Operands(in, 3)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto rn = RegOf(in.operands[1]);
        const auto cond = CondOf(in.operands[2]);
        if (!rd || !rn || !cond) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *rn, *cond));
    }

    void EncodeCondSet(const AsmInstr &in, const CondSetFn fn) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rd = RegOf(in.operands[0]);
        const auto cond = CondOf(in.operands[1]);
        if (!rd || !cond) {
            return;
        }
        Emit(in, (enc_.*fn)(*rd, *cond));
    }

    // Loads and stores

    // The base register of a memory operand, which is 64-bit in every form and
    // reads code 31 as the stack pointer.
    [[nodiscard]] std::optional<A64Reg> BaseOf(const AsmOperand &op) {
        if (op.memBase.empty()) {
            Error(op.location, "memory operand has no base register");
            return std::nullopt;
        }
        return RegNamed(op.memBase, op.location);
    }

    void EncodeMem(const AsmInstr &in, const MemForms &forms) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rt = RegOf(in.operands[0]);
        if (!rt) {
            return;
        }
        const AsmOperand &addr = in.operands[1];

        // `LDR Xt, label` reads the value sitting at a label rather than the
        // memory a register addresses.
        if (addr.kind == AsmOperand::Kind::Sym) {
            if (!forms.literal) {
                Error(addr.location, std::format("'{}' expects a memory operand", in.mnemonic));
                return;
            }
            const std::uint32_t at = Here();
            Emit(in, enc_.LdrLiteral(*rt, 0));
            RecordTarget(in, addr, at, TargetField::Imm19, RcuRelType::None);
            return;
        }
        if (addr.kind != AsmOperand::Kind::Mem) {
            Error(addr.location, std::format("'{}' expects a memory operand", in.mnemonic));
            return;
        }
        const auto rn = BaseOf(addr);
        if (!rn) {
            return;
        }

        if (!addr.memIndex.empty()) {
            if (forms.indexed == nullptr) {
                Error(addr.location, std::format("'{}' has no register-offset form", in.mnemonic));
                return;
            }
            const auto rm = RegNamed(addr.memIndex, addr.location);
            if (!rm) {
                return;
            }
            // Assembly syntax writes the UXTX option of the encoding as LSL,
            // which is also what an index with no qualifier at all means.
            const A64ExtendKind extend =
                addr.extend != AsmExtendKind::None ? ToA64Extend(addr.extend) : A64ExtendKind::Uxtx;
            Emit(in, (enc_.*forms.indexed)(*rt, *rn, *rm, extend, static_cast<unsigned>(addr.shiftAmount)));
            return;
        }

        if (!addr.memSym.empty()) {
            // `[Xn, sym]` is the low twelve bits of a symbol's address, the
            // access an ADRP is completed by.
            if (forms.scaled == nullptr) {
                Error(addr.location, std::format("'{}' does not take a symbol", in.mnemonic));
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
        Emit(in, (enc_.*forms.unscaled)(*rt, *rn, addr.imm, mode));
    }

    void EncodePair(const AsmInstr &in, const PairFn fn) {
        if (!Operands(in, 3)) {
            return;
        }
        const auto rt = RegOf(in.operands[0]);
        const auto rt2 = RegOf(in.operands[1]);
        if (!rt || !rt2) {
            return;
        }
        const AsmOperand &addr = in.operands[2];
        if (addr.kind != AsmOperand::Kind::Mem) {
            Error(addr.location, std::format("'{}' expects a memory operand", in.mnemonic));
            return;
        }
        const auto rn = BaseOf(addr);
        if (!rn) {
            return;
        }
        Emit(in, (enc_.*fn)(*rt, *rt2, *rn, addr.imm, ToA64IndexMode(addr.indexMode)));
    }

    // Branches

    void EncodeBranch(const AsmInstr &in, const bool link) {
        if (!Operands(in, 1)) {
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, link ? enc_.Bl(0) : enc_.B(0));
        RecordTarget(in, in.operands[0], at, TargetField::Imm26,
                     link ? RcuRelType::AArch64Call26 : RcuRelType::AArch64Jump26);
    }

    void EncodeCondBranch(const AsmInstr &in, const std::string_view suffix) {
        const auto cond = ConditionFromName(suffix);
        if (!cond) {
            Error(in.location, std::format("unknown condition '{}' in '{}'", suffix, in.mnemonic));
            return;
        }
        if (!Operands(in, 1)) {
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, enc_.BCond(*cond, 0));
        RecordTarget(in, in.operands[0], at, TargetField::Imm19, RcuRelType::AArch64CondBr19);
    }

    void EncodeCompareBranch(const AsmInstr &in, const CompareBranchFn fn) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rt = RegOf(in.operands[0]);
        if (!rt) {
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, (enc_.*fn)(*rt, 0));
        RecordTarget(in, in.operands[1], at, TargetField::Imm19, RcuRelType::AArch64CondBr19);
    }

    void EncodeTestBranch(const AsmInstr &in, const TestBranchFn fn) {
        if (!Operands(in, 3)) {
            return;
        }
        const auto rt = RegOf(in.operands[0]);
        const auto bit = UnsignedImmOf(in.operands[1], 63);
        if (!rt || !bit) {
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, (enc_.*fn)(*rt, *bit, 0));
        RecordTarget(in, in.operands[2], at, TargetField::Imm14, RcuRelType::AArch64TstBr14);
    }

    // BR, BLR and RET, the last of which returns through X30 when it names no
    // register at all.
    void EncodeBranchReg(const AsmInstr &in, const Reg1Fn fn, const bool optional) {
        if (optional && in.operands.empty()) {
            Emit(in, (enc_.*fn)(A64::Lr));
            return;
        }
        if (!Operands(in, 1)) {
            return;
        }
        const auto rn = RegOf(in.operands[0]);
        if (!rn) {
            return;
        }
        Emit(in, (enc_.*fn)(*rn));
    }

    // System

    void EncodeException(const AsmInstr &in, const Imm16Fn fn) {
        if (!Operands(in, 1)) {
            return;
        }
        const auto imm = UnsignedImmOf(in.operands[0], 0xFFFF);
        if (!imm) {
            return;
        }
        Emit(in, (enc_.*fn)(static_cast<std::uint16_t>(*imm)));
    }

    void EncodeBarrier(const AsmInstr &in, const BarrierFn fn) {
        if (!Operands(in, 0, 1)) {
            return;
        }
        if (in.operands.empty()) {
            Emit(in, (enc_.*fn)(A64Barrier::Sy));
            return;
        }
        const AsmOperand &option = in.operands[0];
        if (option.kind != AsmOperand::Kind::Sym) {
            Error(option.location, std::format("'{}' expects a barrier option", in.mnemonic));
            return;
        }
        const auto barrier = BarrierFromName(Lowered(option.name));
        if (!barrier) {
            Error(option.location, std::format("unknown barrier option '{}'", option.name));
            return;
        }
        Emit(in, (enc_.*fn)(*barrier));
    }

    [[nodiscard]] std::optional<std::uint16_t> SysRegOf(const AsmOperand &op) {
        if (op.kind != AsmOperand::Kind::Sym) {
            Error(op.location, "expected a system register");
            return std::nullopt;
        }
        if (const auto sysreg = SysRegFromName(Lowered(op.name))) {
            return sysreg;
        }
        Error(op.location, std::format("unknown system register '{}'", op.name));
        return std::nullopt;
    }

    void EncodeSysMove(const AsmInstr &in, const bool read) {
        if (!Operands(in, 2)) {
            return;
        }
        const auto rt = RegOf(in.operands[read ? 0 : 1]);
        const auto sysreg = SysRegOf(in.operands[read ? 1 : 0]);
        if (!rt || !sysreg) {
            return;
        }
        Emit(in, read ? enc_.Mrs(*rt, *sysreg) : enc_.Msr(*sysreg, *rt));
    }

    // Scalar floating point

    // FMOV, which moves between registers of either file and is the one
    // floating-point instruction that also names a value.
    void EncodeFmov(const AsmInstr &in) {
        if (!Operands(in, 2)) {
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
        if (!Operands(in, 2)) {
            return;
        }
        const auto rn = RegOf(in.operands[0]);
        if (!rn) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        if (src.kind == AsmOperand::Kind::Imm) {
            if (src.imm != 0) {
                Error(src.location, std::format("'{}' compares against zero or a register", in.mnemonic));
                return;
            }
            Emit(in, signalling ? enc_.FcmpeZero(*rn) : enc_.FcmpZero(*rn));
            return;
        }
        const auto rm = RegOf(src);
        if (!rm) {
            return;
        }
        Emit(in, signalling ? enc_.Fcmpe(*rn, *rm) : enc_.Fcmp(*rn, *rm));
    }

    void EncodeFccmp(const AsmInstr &in) {
        if (!Operands(in, 4)) {
            return;
        }
        const auto rn = RegOf(in.operands[0]);
        const auto rm = RegOf(in.operands[1]);
        const auto nzcv = UnsignedImmOf(in.operands[2], 15);
        const auto cond = CondOf(in.operands[3]);
        if (!rn || !rm || !nzcv || !cond) {
            return;
        }
        Emit(in, enc_.Fccmp(*rn, *rm, *nzcv, *cond));
    }

    // Dispatch

    void Dispatch(const AsmInstr &in) {
        const std::string &m = in.mnemonic;

        // A conditional branch carries its condition in its name, so it is the
        // one mnemonic that is not looked up whole.
        if (const std::string_view base = AsmBaseMnemonic(m); base.size() != m.size()) {
            if (base == "b") {
                EncodeCondBranch(in, std::string_view(m).substr(base.size() + 1));
                return;
            }
            Error(in.location, std::format("unsupported instruction '{}'", m));
            return;
        }

        static const std::unordered_map<std::string_view, ArithForms> arith = {
            {"add", {&A64Enc::AddImm, &A64Enc::Add, &A64Enc::AddExt}},
            {"adds", {&A64Enc::AddsImm, &A64Enc::Adds, &A64Enc::AddsExt}},
            {"sub", {&A64Enc::SubImm, &A64Enc::Sub, &A64Enc::SubExt}},
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

        static const std::unordered_map<std::string_view, MemForms> mem = {
            {"ldr", {&A64Enc::Ldr, &A64Enc::Ldur, &A64Enc::LdrReg, true}},
            {"str", {&A64Enc::Str, &A64Enc::Stur, &A64Enc::StrReg}},
            {"ldrb", {&A64Enc::Ldrb, &A64Enc::Ldurb, &A64Enc::LdrbReg}},
            {"strb", {&A64Enc::Strb, &A64Enc::Sturb, &A64Enc::StrbReg}},
            {"ldrh", {&A64Enc::Ldrh, &A64Enc::Ldurh, &A64Enc::LdrhReg}},
            {"strh", {&A64Enc::Strh, &A64Enc::Sturh, &A64Enc::StrhReg}},
            {"ldrsb", {&A64Enc::Ldrsb, &A64Enc::Ldursb, &A64Enc::LdrsbReg}},
            {"ldrsh", {&A64Enc::Ldrsh, &A64Enc::Ldursh, &A64Enc::LdrshReg}},
            {"ldrsw", {&A64Enc::Ldrsw, &A64Enc::Ldursw, &A64Enc::LdrswReg}},
            {"ldur", {nullptr, &A64Enc::Ldur, nullptr}},
            {"stur", {nullptr, &A64Enc::Stur, nullptr}},
            {"ldurb", {nullptr, &A64Enc::Ldurb, nullptr}},
            {"sturb", {nullptr, &A64Enc::Sturb, nullptr}},
            {"ldurh", {nullptr, &A64Enc::Ldurh, nullptr}},
            {"sturh", {nullptr, &A64Enc::Sturh, nullptr}},
            {"ldursb", {nullptr, &A64Enc::Ldursb, nullptr}},
            {"ldursh", {nullptr, &A64Enc::Ldursh, nullptr}},
            {"ldursw", {nullptr, &A64Enc::Ldursw, nullptr}},
        };
        if (const auto *forms = Lookup(mem, m)) {
            EncodeMem(in, *forms);
            return;
        }

        static const std::unordered_map<std::string_view, Reg2Fn> reg2 = {
            {"sxtb", &A64Enc::Sxtb},     {"sxth", &A64Enc::Sxth},     {"sxtw", &A64Enc::Sxtw},
            {"uxtb", &A64Enc::Uxtb},     {"uxth", &A64Enc::Uxth},     {"clz", &A64Enc::Clz},
            {"cls", &A64Enc::Cls},       {"rbit", &A64Enc::Rbit},     {"rev", &A64Enc::Rev},
            {"rev16", &A64Enc::Rev16},   {"rev32", &A64Enc::Rev32},   {"fneg", &A64Enc::Fneg},
            {"fabs", &A64Enc::Fabs},     {"fsqrt", &A64Enc::Fsqrt},   {"fcvt", &A64Enc::Fcvt},
            {"fcvtzs", &A64Enc::Fcvtzs}, {"fcvtzu", &A64Enc::Fcvtzu}, {"scvtf", &A64Enc::Scvtf},
            {"ucvtf", &A64Enc::Ucvtf},   {"frinta", &A64Enc::Frinta}, {"frintm", &A64Enc::Frintm},
            {"frintn", &A64Enc::Frintn}, {"frintp", &A64Enc::Frintp}, {"frintz", &A64Enc::Frintz},
        };
        if (const auto *fn = Lookup(reg2, m)) {
            EncodeReg2(in, *fn);
            return;
        }

        static const std::unordered_map<std::string_view, Reg3Fn> reg3 = {
            {"lslv", &A64Enc::Lslv},     {"lsrv", &A64Enc::Lsrv},     {"asrv", &A64Enc::Asrv},
            {"rorv", &A64Enc::Rorv},     {"sdiv", &A64Enc::Sdiv},     {"udiv", &A64Enc::Udiv},
            {"mul", &A64Enc::Mul},       {"mneg", &A64Enc::Mneg},     {"smull", &A64Enc::Smull},
            {"umull", &A64Enc::Umull},   {"smulh", &A64Enc::Smulh},   {"umulh", &A64Enc::Umulh},
            {"fadd", &A64Enc::Fadd},     {"fsub", &A64Enc::Fsub},     {"fmul", &A64Enc::Fmul},
            {"fdiv", &A64Enc::Fdiv},     {"fmax", &A64Enc::Fmax},     {"fmin", &A64Enc::Fmin},
            {"fmaxnm", &A64Enc::Fmaxnm}, {"fminnm", &A64Enc::Fminnm},
        };
        if (const auto *fn = Lookup(reg3, m)) {
            EncodeReg3(in, *fn);
            return;
        }

        static const std::unordered_map<std::string_view, Reg4Fn> reg4 = {
            {"madd", &A64Enc::Madd},     {"msub", &A64Enc::Msub},     {"smaddl", &A64Enc::Smaddl},
            {"umaddl", &A64Enc::Umaddl}, {"fmadd", &A64Enc::Fmadd},   {"fmsub", &A64Enc::Fmsub},
            {"fnmadd", &A64Enc::Fnmadd}, {"fnmsub", &A64Enc::Fnmsub},
        };
        if (const auto *fn = Lookup(reg4, m)) {
            EncodeReg4(in, *fn);
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

        static const std::unordered_map<std::string_view, BitfieldFn> bitfield = {
            {"sbfm", &A64Enc::Sbfm}, {"ubfm", &A64Enc::Ubfm}, {"bfm", &A64Enc::Bfm},     {"sbfx", &A64Enc::Sbfx},
            {"ubfx", &A64Enc::Ubfx}, {"bfi", &A64Enc::Bfi},   {"bfxil", &A64Enc::Bfxil},
        };
        if (const auto *fn = Lookup(bitfield, m)) {
            EncodeBitfield(in, *fn);
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

        static const std::unordered_map<std::string_view, CondSelFn> condSel = {
            {"csel", &A64Enc::Csel},   {"csinc", &A64Enc::Csinc}, {"csinv", &A64Enc::Csinv},
            {"csneg", &A64Enc::Csneg}, {"fcsel", &A64Enc::Fcsel},
        };
        if (const auto *fn = Lookup(condSel, m)) {
            EncodeCondSel(in, *fn);
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
            if (Operands(in, 0)) {
                Emit(in, enc_.Nop());
            }
            return;
        }
        if (m == "hint") {
            if (!Operands(in, 1)) {
                return;
            }
            if (const auto imm = UnsignedImmOf(in.operands[0], 127)) {
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

        Error(in.location, std::format("unsupported instruction '{}'", m));
    }
};
} // namespace

AsmAssembly AssembleAArch64AsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName, Bytes &out) {
    Assembler asmr(instrs, sourceName, out);
    return asmr.Run();
}
} // namespace Rux
