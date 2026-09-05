#include "CodeGen/X86_64/AssemblerContext.h"

namespace Rux::X86_64AssemblerPrivate {

void AssemblerContext::EncodeInstr(const AsmInstr &in) {
    if (in.arch != Target::Arch::Unknown && in.arch != Target::Arch::X86_64) {
        Error(in.location, ParsedAssemblyArchitectureDiagnostic(in.mnemonic, in.arch, targetOs, Target::Arch::X86_64));
        return;
    }
    const std::string &m = in.mnemonic;

    // Zero-operand and fixed encodings first.
    if (m == "ret") {
        Emit8(0xC3);
        return;
    }
    if (m == "leave") {
        Emit8(0xC9);
        return;
    }
    if (m == "nop") {
        Emit8(0x90);
        return;
    }
    if (m == "syscall") {
        Emit8(0x0F);
        Emit8(0x05);
        return;
    }
    if (m == "int3") {
        Emit8(0xCC);
        return;
    }
    if (m == "int") {
        if (in.operands.size() == 1 && in.operands[0].kind == AsmOperand::Kind::Imm) {
            Emit8(0xCD);
            Emit8(static_cast<std::uint8_t>(in.operands[0].imm));
        }
        else {
            Error(in.location, "'int' expects an immediate");
        }
        return;
    }
    if (m == "pause") {
        Emit8(0xF3);
        Emit8(0x90);
        return;
    }
    if (m == "mfence") {
        Emit8(0x0F);
        Emit8(0xAE);
        Emit8(0xF0);
        return;
    }
    if (m == "lfence") {
        Emit8(0x0F);
        Emit8(0xAE);
        Emit8(0xE8);
        return;
    }
    if (m == "sfence") {
        Emit8(0x0F);
        Emit8(0xAE);
        Emit8(0xF8);
        return;
    }
    if (m == "lock") {
        Emit8(0xF0);
        return;
    }
    if (m == "xchg") {
        EncodeXchg(in);
        return;
    }
    if (m == "cmpxchg") {
        EncodeCmpxchg(in);
        return;
    }
    if (m == "xadd") {
        EncodeXadd(in);
        return;
    }
    if (m == "cqo") {
        Emit8(0x48);
        Emit8(0x99);
        return;
    }
    if (m == "cdq") {
        Emit8(0x99);
        return;
    }
    if (m == "cdqe") {
        Emit8(0x48);
        Emit8(0x98);
        return;
    }

    // ALU family.
    static const std::unordered_map<std::string_view, AluOp> alu = {
        {"add", {0x01, 0}}, {"or", {0x09, 1}},  {"adc", {0x11, 2}}, {"sbb", {0x19, 3}},
        {"and", {0x21, 4}}, {"sub", {0x29, 5}}, {"xor", {0x31, 6}}, {"cmp", {0x39, 7}},
    };
    if (const auto it = alu.find(m); it != alu.end()) {
        EncodeAlu(in, it->second);
        return;
    }
    if (m == "mov") {
        EncodeMov(in);
        return;
    }
    if (m == "test") {
        EncodeTest(in);
        return;
    }
    if (m == "lea") {
        EncodeLea(in);
        return;
    }
    if (m == "movzx") {
        EncodeMovExtend(in, false);
        return;
    }
    if (m == "movsx") {
        EncodeMovExtend(in, true);
        return;
    }
    if (m == "movsxd") {
        EncodeMovExtend(in, true);
        return;
    }
    if (m == "not") {
        EncodeUnaryGroup(in, 2);
        return;
    }
    if (m == "neg") {
        EncodeUnaryGroup(in, 3);
        return;
    }
    if (m == "mul") {
        EncodeUnaryGroup(in, 4);
        return;
    }
    if (m == "div") {
        EncodeUnaryGroup(in, 6);
        return;
    }
    if (m == "idiv") {
        EncodeUnaryGroup(in, 7);
        return;
    }
    if (m == "imul") {
        EncodeImul(in);
        return;
    }
    if (m == "inc") {
        EncodeIncDec(in, 0);
        return;
    }
    if (m == "dec") {
        EncodeIncDec(in, 1);
        return;
    }
    if (m == "shl" || m == "sal") {
        EncodeShift(in, 4);
        return;
    }
    if (m == "shr") {
        EncodeShift(in, 5);
        return;
    }
    if (m == "sar") {
        EncodeShift(in, 7);
        return;
    }
    if (m == "rol") {
        EncodeShift(in, 0);
        return;
    }
    if (m == "ror") {
        EncodeShift(in, 1);
        return;
    }
    if (m == "push") {
        EncodePush(in);
        return;
    }
    if (m == "pop") {
        EncodePop(in);
        return;
    }
    if (m == "call") {
        EncodeCallJmp(in, true);
        return;
    }
    if (m == "jmp") {
        EncodeCallJmp(in, false);
        return;
    }

    // Conditional jumps and set instructions share a condition-code table.
    if (const auto cc = ConditionCode(m, "j"); cc) {
        EncodeJcc(in, static_cast<std::uint8_t>(0x80 + *cc));
        return;
    }
    if (const auto cc = ConditionCode(m, "set"); cc) {
        EncodeSetcc(in, static_cast<std::uint8_t>(0x90 + *cc));
        return;
    }

    // SSE/SSE2 r/m forms are decoded in the private support implementation.
    if (const auto *form = LookupSseForm(m)) {
        if (form->kind == SseFormKind::RegRm) {
            EncodeSseRegRm(in, form->prefix, form->loadOpcode);
        }
        else {
            EncodeSseMove(in, form->prefix, form->loadOpcode, form->storeOpcode);
        }
        return;
    }

    if (m == "movd") {
        EncodeMovdq(in, false);
        return;
    }
    if (m == "movq") {
        EncodeMovdq(in, true);
        return;
    }
    if (m == "cvtsi2sd") {
        EncodeCvtsi2(in, 0xF2);
        return;
    }
    if (m == "cvtsi2ss") {
        EncodeCvtsi2(in, 0xF3);
        return;
    }
    if (m == "cvtsd2si") {
        EncodeCvt2si(in, 0xF2, 0x2D);
        return;
    }
    if (m == "cvttsd2si") {
        EncodeCvt2si(in, 0xF2, 0x2C);
        return;
    }
    if (m == "cvtss2si") {
        EncodeCvt2si(in, 0xF3, 0x2D);
        return;
    }
    if (m == "cvttss2si") {
        EncodeCvt2si(in, 0xF3, 0x2C);
        return;
    }

    Error(in.location, ClassifyAsmInstruction(m, targetOs, Target::Arch::X86_64));
}
} // namespace Rux::X86_64AssemblerPrivate

namespace Rux {
using X86_64AssemblerPrivate::AssemblerContext;
using X86_64AssemblerPrivate::Bytes;

AsmAssembly AssembleAsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName, Bytes &out,
                            const Target::OS targetOs) {
    AssemblerContext asmr(instrs, sourceName, out, targetOs);
    return asmr.Run();
}
} // namespace Rux
