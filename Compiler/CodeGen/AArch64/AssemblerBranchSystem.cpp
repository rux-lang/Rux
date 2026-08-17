// AArch64 branch, label, symbol-address, exception and system assembly forms,
// plus the final dispatch across all instruction families.

#include "CodeGen/AArch64/AssemblerContext.h"
#include "CodeGen/AArch64/Registers.h"
#include "CodeGen/BackendDiagnostics.h"
#include "Object/Rcu/Rcu.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Rux::AArch64AssemblerPrivate {
namespace {
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
        return {5, 21};
    case TargetField::Imm19:
        break;
    }
    return {5, 19};
}

[[nodiscard]] constexpr std::int64_t ReachOf(const TargetField field) noexcept {
    const std::int64_t half = std::int64_t{1} << (LayoutOf(field).width - 1U);
    return field == TargetField::Adr ? half : half * A64Enc::InstrSize;
}

[[nodiscard]] std::string ReachText(const TargetField field) {
    const std::int64_t reach = ReachOf(field);
    if (reach >= 1024 * 1024) {
        return std::format("+/-{} MiB", reach / (1024 * 1024));
    }
    return std::format("+/-{} KiB", reach / 1024);
}
} // namespace

std::optional<A64Condition> ConditionFromName(const std::string_view name) {
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

std::optional<A64Barrier> BarrierFromName(const std::string_view name) {
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

std::optional<std::uint16_t> SysRegFromName(const std::string_view name) {
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

void AssemblerContext::CollectLabels() {
    for (const auto &instr : instrs) {
        if (!instr.labelDef.empty()) {
            labels.emplace(instr.labelDef, 0);
        }
    }
}

void AssemblerContext::RecordTarget(const AsmInstr &in, const AsmOperand &target, const std::uint32_t at,
                                    const TargetField field, const std::uint16_t relType) {
    if (target.kind != AsmOperand::Kind::Sym) {
        FormError(target.location, std::format("'{}' takes a label or a symbol as operand {}, found {}", in.mnemonic,
                                               IndexOf(target), FoundText(target)));
        return;
    }
    if (labels.contains(target.name)) {
        targets.push_back({at, target.name, target.location, in.mnemonic, field});
        return;
    }
    if (relType == RcuRelType::None) {
        Error(target.location, std::format("'{}' cannot reference '{}': no label of that name is defined in this body",
                                           in.mnemonic, target.name));
        return;
    }
    AddFixup(at, target.name, relType);
}

void AssemblerContext::ResolveLocalTargets() {
    for (const auto &target : targets) {
        const auto it = labels.find(target.label);
        if (it == labels.end()) {
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
            encoder.PatchField(target.instrOffset, 29, 2, static_cast<std::uint32_t>(delta) & 3U);
            encoder.PatchField(target.instrOffset, lsb, width - 2U, static_cast<std::uint32_t>(delta >> 2));
            continue;
        }
        encoder.PatchField(target.instrOffset, lsb, width, static_cast<std::uint32_t>(delta / A64Enc::InstrSize));
    }
}

// ADR reaches a local label directly. ADRP names the page a symbol sits on,
// which is a link-time quantity and therefore always a relocation.
void BranchSystemAssemblerContext::EncodeAdr(const AsmInstr &in, const bool page) {
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
        FormError(target.location, std::format("'{}' takes a label or a symbol as operand {}, found {}", in.mnemonic,
                                               IndexOf(target), FoundText(target)));
        return;
    }
    const std::uint32_t at = Here();
    Emit(in, page ? encoder.Adrp(*rd, 0) : encoder.Adr(*rd, 0));
    if (page) {
        AddFixup(at, target.name, RcuRelType::AArch64AdrPrelPgHi21);
        return;
    }
    RecordTarget(in, target, at, TargetField::Adr, RcuRelType::None);
}

void BranchSystemAssemblerContext::EncodeBranch(const AsmInstr &in, const bool link) {
    Begin(in, "label");
    if (!Operands(1)) {
        return;
    }
    const std::uint32_t at = Here();
    Emit(in, link ? encoder.Bl(0) : encoder.B(0));
    RecordTarget(in, in.operands[0], at, TargetField::Imm26,
                 link ? RcuRelType::AArch64Call26 : RcuRelType::AArch64Jump26);
}

void BranchSystemAssemblerContext::EncodeCondBranch(const AsmInstr &in, const std::string_view suffix) {
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
    Emit(in, encoder.BCond(*cond, 0));
    RecordTarget(in, in.operands[0], at, TargetField::Imm19, RcuRelType::AArch64CondBr19);
}

void BranchSystemAssemblerContext::EncodeCompareBranch(const AsmInstr &in, const CompareBranchFn fn) {
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
    Emit(in, (encoder.*fn)(*rt, 0));
    RecordTarget(in, in.operands[1], at, TargetField::Imm19, RcuRelType::AArch64CondBr19);
}

void BranchSystemAssemblerContext::EncodeTestBranch(const AsmInstr &in, const TestBranchFn fn) {
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
    Emit(in, (encoder.*fn)(*rt, *bit, 0));
    RecordTarget(in, in.operands[2], at, TargetField::Imm14, RcuRelType::AArch64TstBr14);
}

// BR, BLR and RET, the last of which returns through X30 when omitted.
void BranchSystemAssemblerContext::EncodeBranchReg(const AsmInstr &in, const Reg1Fn fn, const bool optional) {
    Begin(in, optional ? "{Xn}" : "Xn");
    if (optional && in.operands.empty()) {
        Emit(in, (encoder.*fn)(A64::Lr));
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
    Emit(in, (encoder.*fn)(*rn));
}

void BranchSystemAssemblerContext::EncodeException(const AsmInstr &in, const Imm16Fn fn) {
    Begin(in, "#imm");
    if (!Operands(1)) {
        return;
    }
    const auto imm = UnsignedImmOf(in.operands[0], 0xFFFF, "an exception code");
    if (!imm) {
        return;
    }
    Emit(in, (encoder.*fn)(static_cast<std::uint16_t>(*imm)));
}

void BranchSystemAssemblerContext::EncodeBarrier(const AsmInstr &in, const BarrierFn fn) {
    Begin(in, "{option}");
    if (!Operands(0, 1)) {
        return;
    }
    if (in.operands.empty()) {
        Emit(in, (encoder.*fn)(A64Barrier::Sy));
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
    Emit(in, (encoder.*fn)(*barrier));
}

std::optional<std::uint16_t> BranchSystemAssemblerContext::SysRegOf(const AsmOperand &op) {
    if (op.kind != AsmOperand::Kind::Sym) {
        FormError(op.location, std::format("'{}' takes a system register as operand {}, found {}", Mnemonic(),
                                           IndexOf(op), FoundText(op)));
        return std::nullopt;
    }
    if (const auto sysreg = SysRegFromName(Lowered(op.name))) {
        return sysreg;
    }
    FormError(op.location, std::format("unknown system register '{}'", op.name));
    return std::nullopt;
}

void BranchSystemAssemblerContext::EncodeSysMove(const AsmInstr &in, const bool read) {
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
    Emit(in, read ? encoder.Mrs(*rt, *sysreg) : encoder.Msr(*sysreg, *rt));
}

void BranchSystemAssemblerContext::Unsupported(const AsmInstr &in) {
    auto diagnostic = ClassifyAsmInstruction(in.mnemonic, TargetOs(), Target::Arch::AArch64);
    Error(in.location, std::move(diagnostic.message), std::move(diagnostic.notes), std::move(diagnostic.help));
}

void BranchSystemAssemblerContext::Dispatch(const AsmInstr &in) {
    const std::string &m = in.mnemonic;

    // A conditional branch carries its condition in its name.
    if (const std::string_view base = AsmBaseMnemonic(m); base.size() != m.size()) {
        if (base == "b") {
            EncodeCondBranch(in, std::string_view(m).substr(base.size() + 1));
            return;
        }
        Unsupported(in);
        return;
    }

    if (DispatchMemoryFloating(in)) {
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

    if (m == "adr" || m == "adrp") {
        EncodeAdr(in, m == "adrp");
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
            Emit(in, encoder.Nop());
        }
        return;
    }
    if (m == "hint") {
        Begin(in, "#imm");
        if (!Operands(1)) {
            return;
        }
        if (const auto imm = UnsignedImmOf(in.operands[0], 127, "a hint number")) {
            Emit(in, encoder.Hint(*imm));
        }
        return;
    }
    if (m == "mrs" || m == "msr") {
        EncodeSysMove(in, m == "mrs");
        return;
    }

    Unsupported(in);
}
} // namespace Rux::AArch64AssemblerPrivate
