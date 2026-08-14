// AArch64 assembler for `asm func` bodies. ARM syntax (destination first).
//
// The private assembler contexts select and validate integer, memory and
// floating-point forms. This final facade owns branch, label, system and
// unsupported-instruction dispatch while all instruction words go through
// A64Enc.

#include "CodeGen/AArch64/AssemblerContext.h"
#include "CodeGen/AArch64/Registers.h"
#include "Object/Rcu/Rcu.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Rux {
namespace {
using namespace AArch64AssemblerPrivate;

class Assembler final : public MemoryFloatingAssemblerContext {
public:
    using MemoryFloatingAssemblerContext::MemoryFloatingAssemblerContext;

private:
    // ADR reaches a local label directly. ADRP names the page a symbol sits on,
    // which is a link-time quantity and therefore always a relocation.
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
            AddFixup(at, target.name, RcuRelType::AArch64AdrPrelPgHi21);
            return;
        }
        RecordTarget(in, target, at, TargetField::Adr, RcuRelType::None);
    }

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

    // BR, BLR and RET, the last of which returns through X30 when omitted.
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

    void Dispatch(const AsmInstr &in) override {
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

        Unsupported(in);
    }
};
} // namespace

AsmAssembly AssembleAArch64AsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName, Bytes &out) {
    Assembler asmr(instrs, sourceName, out);
    return asmr.Run();
}
} // namespace Rux
