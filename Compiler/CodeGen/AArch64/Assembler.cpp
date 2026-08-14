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
// a diagnostic rather than as a wrong instruction. The assembler implementation
// chooses which encoder a written instruction means: `ADD` is three
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

#include "CodeGen/AArch64/AssemblerContext.h"
#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"
#include "Object/Rcu/Rcu.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Rux {
namespace {
using namespace AArch64AssemblerPrivate;

class Assembler final : public IntegerAssemblerContext {
public:
    using IntegerAssemblerContext::IntegerAssemblerContext;

private:
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
            AddFixup(at, target.name, RcuRelType::AArch64AdrPrelPgHi21);
            return;
        }
        RecordTarget(in, target, at, TargetField::Adr, RcuRelType::None);
    }

    // Loads and stores

    // The register a memory operand addresses through, as the encoder models
    // it. What that register is allowed to be is CheckBase's question.
    [[nodiscard]] std::optional<A64Reg> BaseOf(const AsmOperand &op) {
        if (op.memBase.empty()) {
            FormError(op.location, std::format("'{}' addresses memory through a base register, and this operand names "
                                               "none",
                                               Mnemonic()));
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
                                                 Mnemonic(), addr.memBase));
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
            AddFixup(at, addr.memSym, RcuRelType::AArch64LdstAbsLo12Nc);
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

    void Dispatch(const AsmInstr &in) override {
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

        if (DispatchInteger(in)) {
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

        // The conversions whose two operands deliberately disagree in width,
        // register file, or both name their own shape and leave the remaining
        // checking to the encoder.
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
            return;
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
            return;
        }

        static const std::unordered_map<std::string_view, Form<Reg4Fn>> reg4 = {
            {"fmadd", {&A64Enc::Fmadd, "Vd, Vn, Vm, Va", RegClass::Float}},
            {"fmsub", {&A64Enc::Fmsub, "Vd, Vn, Vm, Va", RegClass::Float}},
            {"fnmadd", {&A64Enc::Fnmadd, "Vd, Vn, Vm, Va", RegClass::Float}},
            {"fnmsub", {&A64Enc::Fnmsub, "Vd, Vn, Vm, Va", RegClass::Float}},
        };
        if (const auto *form = Lookup(reg4, m)) {
            EncodeReg4(in, *form);
            return;
        }

        static const std::unordered_map<std::string_view, Form<CondSelFn>> condSel = {
            {"fcsel", {&A64Enc::Fcsel, "Vd, Vn, Vm, cond", RegClass::Float}},
        };
        if (const auto *form = Lookup(condSel, m)) {
            EncodeCondSel(in, *form);
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
