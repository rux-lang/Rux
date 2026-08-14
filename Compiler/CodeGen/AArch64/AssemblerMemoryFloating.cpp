#include "CodeGen/AArch64/AssemblerContext.h"
#include "Object/Rcu/Rcu.h"

#include <bit>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Rux::AArch64AssemblerPrivate {
// The register a memory operand addresses through, as the encoder models it.
// What that register is allowed to be is CheckBase's question.
std::optional<A64Reg> MemoryFloatingAssemblerContext::BaseOf(const AsmOperand &op) {
    if (op.memBase.empty()) {
        FormError(op.location, std::format("'{}' addresses memory through a base register, and this operand names "
                                           "none",
                                           Mnemonic()));
        return std::nullopt;
    }
    return RegNamed(op.memBase, op.location);
}

// The base register of a memory operand addresses memory, so it is 64-bit in
// every form and is the one field that reads code 31 as the stack pointer.
bool MemoryFloatingAssemblerContext::CheckBase(const AsmOperand &addr, const A64Reg base) {
    if (base.IsVector() || !base.Is64()) {
        FormError(addr.location, std::format("'{}' addresses memory through a 64-bit general-purpose register, "
                                             "found '{}'",
                                             Mnemonic(), addr.memBase));
        return false;
    }
    return true;
}

void MemoryFloatingAssemblerContext::EncodeMem(const AsmInstr &in, const MemForms &forms) {
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
            FormError(addr.location,
                      std::format("'{}' takes a memory operand as operand 2, found {}", in.mnemonic, FoundText(addr)));
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
        // The instruction carries a scale bit rather than a shift amount, so
        // the only shift it can express scales the index by the access width.
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
        // `[Xn, sym]` is the low twelve bits of a symbol's address, the access
        // an ADRP is completed by.
        if (forms.scaled == nullptr) {
            FormError(addr.location, std::format("'{}' takes no symbol operand", in.mnemonic));
            return;
        }
        const std::uint32_t at = Here();
        Emit(in, (enc_.*forms.scaled)(*rt, *rn, 0));
        AddFixup(at, addr.memSym, RcuRelType::AArch64LdstAbsLo12Nc);
        return;
    }

    const A64IndexMode mode = ToA64IndexMode(addr.indexMode);
    if (forms.scaled != nullptr && mode == A64IndexMode::Offset && addr.imm >= 0) {
        // The scaled form reaches furthest and is the canonical spelling of an
        // offset it can express. Negative or unaligned offsets use unscaled.
        if ((enc_.*forms.scaled)(*rt, *rn, static_cast<std::uint64_t>(addr.imm)) == A64Status::Ok) {
            return;
        }
    }
    const A64Status status = (enc_.*forms.unscaled)(*rt, *rn, addr.imm, mode);
    if (status == A64Status::OutOfRange || status == A64Status::Unaligned) {
        // Both forms were tried, so name their combined reach.
        if (forms.scaled != nullptr && mode == A64IndexMode::Offset) {
            FormError(addr.location,
                      std::format("'{}' takes an offset of -256 to 255, or a multiple of {} from 0 to {}, found {}",
                                  in.mnemonic, width, 4095U * width, addr.imm));
            return;
        }
        FormError(addr.location, std::format("'{}' takes an offset of -256 to 255, found {}", in.mnemonic, addr.imm));
        return;
    }
    Emit(in, status);
}

void MemoryFloatingAssemblerContext::EncodePair(const AsmInstr &in, const PairFn fn) {
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
    // The offset counts registers rather than bytes, which lets one STP open a
    // frame and save the frame chain into it.
    const auto width = static_cast<std::int64_t>(rt->bits / 8U);
    if (addr.imm % width != 0 || addr.imm < -64 * width || addr.imm > 63 * width) {
        FormError(addr.location, std::format("'{}' takes an offset that is a multiple of {} from {} to {}, found {}",
                                             in.mnemonic, width, -64 * width, 63 * width, addr.imm));
        return;
    }
    Emit(in, (enc_.*fn)(*rt, *rt2, *rn, addr.imm, ToA64IndexMode(addr.indexMode)));
}

// FMOV moves between registers of either file and is the one floating-point
// instruction that also names a value.
void MemoryFloatingAssemblerContext::EncodeFmov(const AsmInstr &in) {
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
        // The lexer has no floating-point literal inside an `asm func` body,
        // so the value is written as the integer it equals.
        Emit(in, enc_.FmovImm(*rd, static_cast<double>(src.imm)));
        return;
    }
    const auto rn = RegOf(src);
    if (!rn) {
        return;
    }
    Emit(in, enc_.Fmov(*rd, *rn));
}

// FCMP and FCMPE take either a register or the dedicated zero form.
void MemoryFloatingAssemblerContext::EncodeFcmp(const AsmInstr &in, const bool signalling) {
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
            FormError(src.location,
                      std::format("'{}' compares against a register or against zero, found {}", in.mnemonic, src.imm));
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

void MemoryFloatingAssemblerContext::EncodeFccmp(const AsmInstr &in) {
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

bool MemoryFloatingAssemblerContext::DispatchMemoryFloating(const AsmInstr &in) {
    if (DispatchInteger(in)) {
        return true;
    }

    const std::string &m = in.mnemonic;
    // The fourth field is one access's width. Narrowing and sign-extending
    // mnemonics carry it; the rest read it from the transferred register.
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
        return true;
    }

    // These conversions deliberately disagree in width, register file, or
    // both, so their shapes leave the remaining checking to the encoder.
    static const std::unordered_map<std::string_view, Form<Reg2Fn>> reg2 = {
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
        return true;
    }

    static const std::unordered_map<std::string_view, Form<Reg3Fn>> reg3 = {
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
        return true;
    }

    static const std::unordered_map<std::string_view, Form<Reg4Fn>> reg4 = {
        {"fmadd", {&A64Enc::Fmadd, "Vd, Vn, Vm, Va", RegClass::Float}},
        {"fmsub", {&A64Enc::Fmsub, "Vd, Vn, Vm, Va", RegClass::Float}},
        {"fnmadd", {&A64Enc::Fnmadd, "Vd, Vn, Vm, Va", RegClass::Float}},
        {"fnmsub", {&A64Enc::Fnmsub, "Vd, Vn, Vm, Va", RegClass::Float}},
    };
    if (const auto *form = Lookup(reg4, m)) {
        EncodeReg4(in, *form);
        return true;
    }

    static const std::unordered_map<std::string_view, Form<CondSelFn>> condSel = {
        {"fcsel", {&A64Enc::Fcsel, "Vd, Vn, Vm, cond", RegClass::Float}},
    };
    if (const auto *form = Lookup(condSel, m)) {
        EncodeCondSel(in, *form);
        return true;
    }

    if (m == "ldp" || m == "stp") {
        EncodePair(in, m == "ldp" ? &A64Enc::Ldp : &A64Enc::Stp);
        return true;
    }
    if (m == "fmov") {
        EncodeFmov(in);
        return true;
    }
    if (m == "fcmp" || m == "fcmpe") {
        EncodeFcmp(in, m == "fcmpe");
        return true;
    }
    if (m == "fccmp") {
        EncodeFccmp(in);
        return true;
    }
    return false;
}
} // namespace Rux::AArch64AssemblerPrivate
