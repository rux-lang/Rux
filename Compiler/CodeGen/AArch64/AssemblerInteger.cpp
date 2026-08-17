#include "CodeGen/AArch64/AssemblerContext.h"
#include "Object/Rcu/Rcu.h"

#include <format>
#include <string_view>
#include <unordered_map>

namespace Rux::AArch64AssemblerPrivate {
// ADD / ADDS / SUB / SUBS and the CMP / CMN spelling of the last two. The
// last operand decides the form: an immediate, a register with an extension,
// or a register with a shift — which is also the plain register form, since a
// shift of nothing is LSL #0.
void IntegerAssemblerContext::EncodeArith(const AsmInstr &in, const ArithForms &forms) {
    Begin(in,
          forms.discardsResult ? "Rn, #imm | Rn, Rm{, shift #amount}" : "Rd, Rn, #imm | Rd, Rn, Rm{, shift #amount}");
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
        Emit(in, (encoder.*forms.imm)(rd, *rn, value));
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
        Emit(in, (encoder.*forms.imm)(rd, *rn, 0));
        AddFixup(at, src.name, RcuRelType::AArch64AddAbsLo12Nc);
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
        Emit(in, (encoder.*forms.extended)(rd, *rn, *rm, ToA64Extend(src.extend), amount));
        return;
    }
    // A plain register form that addresses through SP is the extended one
    // with the extension that reads the whole register, which is how every
    // assembler reads `add x0, sp, x1`: code 31 is the stack pointer there
    // and the zero register in the shifted form.
    if ((rn->IsStackPointer() || rd.IsStackPointer()) && src.shift == AsmShiftKind::None && rm->IsGeneral() &&
        !rm->IsStackPointer() && rm->bits == rn->bits) {
        Emit(in, (encoder.*forms.extended)(rd, *rn, *rm, rn->Is64() ? A64ExtendKind::Uxtx : A64ExtendKind::Uxtw, 0));
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
        FormError(src.location, std::format("'{}' shifts its second source by LSL, LSR or ASR, found {}", in.mnemonic,
                                            ShiftName(src.shift)));
        return;
    }
    if (amount >= rm->bits) {
        FormError(src.location,
                  std::format("'{}' takes a shift of 0 to {}, found {}", in.mnemonic, rm->bits - 1U, amount));
        return;
    }
    Emit(in, (encoder.*forms.shifted)(rd, *rn, *rm, ToA64Shift(src.shift), amount));
}

// AND / ORR / EOR / ANDS, their inverting counterparts, and TST.
void IntegerAssemblerContext::EncodeLogic(const AsmInstr &in, const LogicForms &forms) {
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
        Emit(in, (encoder.*forms.imm)(rd, *rn, value));
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
    Emit(in, (encoder.*forms.shifted)(rd, *rn, *rm, ToA64Shift(src.shift), amount));
}

// MOV, which moves a register or materializes a constant. A constant no
// single instruction reaches becomes the MOVZ / MOVK chain LoadImm64 picks,
// which is what `LDR Xd, =value` would have assembled to.
void IntegerAssemblerContext::EncodeMov(const AsmInstr &in) {
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
        Emit(in, encoder.LoadImm64(*rd, value));
        return;
    }
    const auto rm = RegOf(src);
    if (!rm) {
        return;
    }
    if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd), Ref(src, *rm)})) {
        return;
    }
    Emit(in, encoder.Mov(*rd, *rm));
}

// MOVZ / MOVN / MOVK, whose immediate is one halfword and whose shift names
// which halfword that is.
void IntegerAssemblerContext::EncodeMovw(const AsmInstr &in, const MovwFn fn) {
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
    Emit(in, (encoder.*fn)(*rd, static_cast<std::uint16_t>(*imm), shift));
}

// SBFM / UBFM / BFM and the four aliases, which differ only in what their
// two immediates mean.
void IntegerAssemblerContext::EncodeBitfield(const AsmInstr &in, const BitfieldForms &form) {
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
        FormError(in.operands[3].location, std::format("'{}' moves at least one bit, found a width of 0", in.mnemonic));
        return;
    }
    Emit(in, (encoder.*form.fn)(*rd, *rn, *first, *second));
}

// LSL / LSR / ASR / ROR, which name a constant shift written with an
// immediate and the variable-register instruction written with a register.
void IntegerAssemblerContext::EncodeShift(const AsmInstr &in, const ShiftForms &forms) {
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
        Emit(in, (encoder.*forms.variable)(*rd, *rn, *rm));
        return;
    }
    const auto amount = BitOf(in.operands[2], *rd, "a shift");
    if (!amount) {
        return;
    }
    Emit(in, (encoder.*forms.imm)(*rd, *rn, *amount));
}

void IntegerAssemblerContext::EncodeExtr(const AsmInstr &in) {
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
    if (!Uniform(RegClass::General, {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm)}) ||
        !NoStackPointer({Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm)})) {
        return;
    }
    const auto lsb = BitOf(in.operands[3], *rd, "a bit position");
    if (!lsb) {
        return;
    }
    Emit(in, encoder.Extr(*rd, *rn, *rm, *lsb));
}

// The register-only shapes, each of which is one table away from its encoder:
// two registers, three, four, or two with a shift on the second. The floating
// dispatcher also reuses the arity helpers until its own family is split.
void IntegerAssemblerContext::EncodeReg2(const AsmInstr &in, const Form<Reg2Fn> &form) {
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
    Emit(in, (encoder.*form.fn)(*rd, *rn));
}

void IntegerAssemblerContext::EncodeReg3(const AsmInstr &in, const Form<Reg3Fn> &form) {
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
    Emit(in, (encoder.*form.fn)(*rd, *rn, *rm));
}

void IntegerAssemblerContext::EncodeReg4(const AsmInstr &in, const Form<Reg4Fn> &form) {
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
        !NoStackPointer(
            {Ref(in.operands[0], *rd), Ref(in.operands[1], *rn), Ref(in.operands[2], *rm), Ref(in.operands[3], *ra)})) {
        return;
    }
    Emit(in, (encoder.*form.fn)(*rd, *rn, *rm, *ra));
}

void IntegerAssemblerContext::EncodeReg2Shift(const AsmInstr &in, const Reg2ShiftFn fn) {
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
    Emit(in, (encoder.*fn)(*rd, *rm, ToA64Shift(src.shift), amount));
}

// The conditional group: a select over two registers, the aliases that read
// one, and the two that read none.
void IntegerAssemblerContext::EncodeCondSel(const AsmInstr &in, const Form<CondSelFn> &form) {
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
    Emit(in, (encoder.*form.fn)(*rd, *rn, *rm, *cond));
}

void IntegerAssemblerContext::EncodeCondAlias(const AsmInstr &in, const CondAliasFn fn) {
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
    Emit(in, (encoder.*fn)(*rd, *rn, *cond));
}

void IntegerAssemblerContext::EncodeCondSet(const AsmInstr &in, const CondSetFn fn) {
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
    Emit(in, (encoder.*fn)(*rd, *cond));
}

bool IntegerAssemblerContext::DispatchInteger(const AsmInstr &in) {
    const std::string &mnemonic = in.mnemonic;

    // ADD and SUB read and write SP; the flag-setting pair reads it and writes
    // the zero register, which is what makes them CMN and CMP.
    static const std::unordered_map<std::string_view, ArithForms> arith = {
        {"add", {&A64Enc::AddImm, &A64Enc::Add, &A64Enc::AddExt, false, true}},
        {"adds", {&A64Enc::AddsImm, &A64Enc::Adds, &A64Enc::AddsExt}},
        {"sub", {&A64Enc::SubImm, &A64Enc::Sub, &A64Enc::SubExt, false, true}},
        {"subs", {&A64Enc::SubsImm, &A64Enc::Subs, &A64Enc::SubsExt}},
        {"cmn", {&A64Enc::AddsImm, &A64Enc::Adds, &A64Enc::AddsExt, true}},
        {"cmp", {&A64Enc::SubsImm, &A64Enc::Subs, &A64Enc::SubsExt, true}},
    };
    if (const auto *forms = Lookup(arith, mnemonic)) {
        EncodeArith(in, *forms);
        return true;
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
    if (const auto *forms = Lookup(logic, mnemonic)) {
        EncodeLogic(in, *forms);
        return true;
    }

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
    };
    if (const auto *form = Lookup(reg2, mnemonic)) {
        EncodeReg2(in, *form);
        return true;
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
    };
    if (const auto *form = Lookup(reg3, mnemonic)) {
        EncodeReg3(in, *form);
        return true;
    }

    static const std::unordered_map<std::string_view, Form<Reg4Fn>> reg4 = {
        {"madd", {&A64Enc::Madd}},
        {"msub", {&A64Enc::Msub}},
        {"smaddl", {&A64Enc::Smaddl, "Xd, Wn, Wm, Xa", RegClass::Mixed}},
        {"umaddl", {&A64Enc::Umaddl, "Xd, Wn, Wm, Xa", RegClass::Mixed}},
    };
    if (const auto *form = Lookup(reg4, mnemonic)) {
        EncodeReg4(in, *form);
        return true;
    }

    static const std::unordered_map<std::string_view, Reg2ShiftFn> reg2Shift = {
        {"neg", &A64Enc::Neg},
        {"negs", &A64Enc::Negs},
        {"mvn", &A64Enc::Mvn},
    };
    if (const auto *fn = Lookup(reg2Shift, mnemonic)) {
        EncodeReg2Shift(in, *fn);
        return true;
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
    if (const auto *form = Lookup(bitfield, mnemonic)) {
        EncodeBitfield(in, *form);
        return true;
    }

    static const std::unordered_map<std::string_view, ShiftForms> shift = {
        {"lsl", {&A64Enc::Lsl, &A64Enc::Lslv}},
        {"lsr", {&A64Enc::Lsr, &A64Enc::Lsrv}},
        {"asr", {&A64Enc::Asr, &A64Enc::Asrv}},
        {"ror", {&A64Enc::Ror, &A64Enc::Rorv}},
    };
    if (const auto *forms = Lookup(shift, mnemonic)) {
        EncodeShift(in, *forms);
        return true;
    }

    static const std::unordered_map<std::string_view, MovwFn> movw = {
        {"movz", &A64Enc::Movz},
        {"movn", &A64Enc::Movn},
        {"movk", &A64Enc::Movk},
    };
    if (const auto *fn = Lookup(movw, mnemonic)) {
        EncodeMovw(in, *fn);
        return true;
    }

    static const std::unordered_map<std::string_view, Form<CondSelFn>> condSel = {
        {"csel", {&A64Enc::Csel}},
        {"csinc", {&A64Enc::Csinc}},
        {"csinv", {&A64Enc::Csinv}},
        {"csneg", {&A64Enc::Csneg}},
    };
    if (const auto *form = Lookup(condSel, mnemonic)) {
        EncodeCondSel(in, *form);
        return true;
    }

    static const std::unordered_map<std::string_view, CondAliasFn> condAlias = {
        {"cinc", &A64Enc::Cinc},
        {"cinv", &A64Enc::Cinv},
        {"cneg", &A64Enc::Cneg},
    };
    if (const auto *fn = Lookup(condAlias, mnemonic)) {
        EncodeCondAlias(in, *fn);
        return true;
    }

    static const std::unordered_map<std::string_view, CondSetFn> condSet = {
        {"cset", &A64Enc::Cset},
        {"csetm", &A64Enc::Csetm},
    };
    if (const auto *fn = Lookup(condSet, mnemonic)) {
        EncodeCondSet(in, *fn);
        return true;
    }

    if (mnemonic == "mov") {
        EncodeMov(in);
        return true;
    }
    if (mnemonic == "extr") {
        EncodeExtr(in);
        return true;
    }
    return false;
}
} // namespace Rux::AArch64AssemblerPrivate
