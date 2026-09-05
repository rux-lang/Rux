#include "CodeGen/X86_64/AssemblerContext.h"

namespace Rux::X86_64AssemblerPrivate {

void AssemblerContext::EncodeAlu(const AsmInstr &in, const AluOp &spec) {
    if (in.operands.size() != 2) {
        Error(in.location, std::format("'{}' expects 2 operands", in.mnemonic));
        return;
    }
    const AsmOperand &dst = in.operands[0];
    const AsmOperand &src = in.operands[1];
    const int opSize = OperandSize(in, 8);
    const bool byte = opSize == 1;

    if (src.kind == AsmOperand::Kind::Imm) {
        // group /ext with 0x80 (byte) or 0x81 / 0x83 (imm8 sign-extended).
        std::optional<AsmRegInfo> reg;
        RmEnc rm = EncodeRm(dst, reg);
        const bool useImm8 = !byte && FitsInt8(src.imm);
        if (!byte && !useImm8 && !FitsInt32(src.imm)) {
            Error(src.location, Signed32EncodingRangeDiagnostic(in.mnemonic, src.imm));
            return;
        }
        const std::uint8_t opcode = byte ? 0x80 : (useImm8 ? 0x83 : 0x81);
        EmitModRM(opSize, {opcode}, spec.ext, rm, in.location);
        EmitAluImm(src.imm, opSize, useImm8);
        return;
    }

    if (src.kind == AsmOperand::Kind::Reg && (dst.kind == AsmOperand::Kind::Reg || dst.kind == AsmOperand::Kind::Mem)) {
        // MR form: opcode = mrOpcode (r/m, reg). reg field = src.
        auto srcReg = Reg(src);
        if (!srcReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(dst, ignore);
        rm.rexRequired = rm.rexRequired || srcReg->rexRequired;
        rm.rexForbidden = rm.rexForbidden || srcReg->high8;
        const std::uint8_t opcode = byte ? (spec.mrOpcode & ~1) : spec.mrOpcode;
        EmitModRM(opSize, {opcode}, srcReg->code, rm, in.location);
        return;
    }

    if (dst.kind == AsmOperand::Kind::Reg && src.kind == AsmOperand::Kind::Mem) {
        // RM form: opcode = mrOpcode | 2 (reg, r/m). reg field = dst.
        auto dstReg = Reg(dst);
        if (!dstReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(src, ignore);
        const std::uint8_t opcode = byte ? ((spec.mrOpcode & ~1) | 2) : (spec.mrOpcode | 2);
        EmitModRM(opSize, {opcode}, dstReg->code, rm, in.location);
        return;
    }

    Error(in.location, std::format("unsupported operands for '{}'", in.mnemonic));
}

void AssemblerContext::EncodeMov(const AsmInstr &in) {
    if (in.operands.size() != 2) {
        Error(in.location, "'mov' expects 2 operands");
        return;
    }
    const AsmOperand &dst = in.operands[0];
    const AsmOperand &src = in.operands[1];
    const int opSize = OperandSize(in, 8);
    const bool byte = opSize == 1;

    if (src.kind == AsmOperand::Kind::Imm && dst.kind == AsmOperand::Kind::Reg) {
        auto r = Reg(dst);
        if (!r) {
            return;
        }
        if (opSize == 8 && !FitsInt32(src.imm)) {
            // movabs r64, imm64 (REX.W + 0xB8+r).
            Emit8(static_cast<std::uint8_t>(0x48 | (r->code >= 8 ? 0x01 : 0x00)));
            Emit8(static_cast<std::uint8_t>(0xB8 + (r->code & 7)));
            Emit64(static_cast<std::uint64_t>(src.imm));
            return;
        }
        if (byte) {
            RmEnc rm = EncodeRmReg(*r);
            // mov r/m8, imm8 -> 0xC6 /0
            EmitModRM(1, {0xC6}, 0, rm, in.location);
            Emit8(static_cast<std::uint8_t>(src.imm));
            return;
        }
        // mov r/m, imm32 (sign-extended for 64-bit) -> 0xC7 /0
        RmEnc rm = EncodeRmReg(*r);
        EmitModRM(opSize, {0xC7}, 0, rm, in.location);
        Emit32(static_cast<std::int32_t>(src.imm));
        return;
    }

    if (src.kind == AsmOperand::Kind::Imm && dst.kind == AsmOperand::Kind::Mem) {
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(dst, ignore);
        if (!FitsInt32(src.imm)) {
            Error(src.location, Signed32EncodingRangeDiagnostic("mov", src.imm));
            return;
        }
        EmitModRM(opSize, {static_cast<std::uint8_t>(byte ? 0xC6 : 0xC7)}, 0, rm, in.location);
        EmitAluImm(src.imm, opSize, byte);
        return;
    }

    // Symbol immediate: mov reg, symbol -> load its (relocated) address.
    if (src.kind == AsmOperand::Kind::Sym && dst.kind == AsmOperand::Kind::Reg) {
        // lea reg, [rip + sym]
        AsmOperand mem;
        mem.kind = AsmOperand::Kind::Mem;
        mem.memBase = "rip";
        mem.memSym = src.name;
        mem.location = src.location;
        auto dstReg = Reg(dst);
        if (!dstReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(mem, ignore);
        EmitModRM(8, {0x8D}, dstReg->code, rm, in.location);
        return;
    }

    // Register/memory forms reuse the ALU MR/RM machinery (opcode 0x88/0x89).
    EncodeAlu(in, {0x89, 0});
}

void AssemblerContext::EncodeTest(const AsmInstr &in) {
    if (in.operands.size() != 2) {
        Error(in.location, "'test' expects 2 operands");
        return;
    }
    const AsmOperand &dst = in.operands[0];
    const AsmOperand &src = in.operands[1];
    const int opSize = OperandSize(in, 8);
    const bool byte = opSize == 1;
    if (src.kind == AsmOperand::Kind::Imm) {
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(dst, ignore);
        EmitModRM(opSize, {static_cast<std::uint8_t>(byte ? 0xF6 : 0xF7)}, 0, rm, in.location);
        EmitAluImm(src.imm, opSize, byte);
        return;
    }
    if (src.kind == AsmOperand::Kind::Reg) {
        auto srcReg = Reg(src);
        if (!srcReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(dst, ignore);
        EmitModRM(opSize, {static_cast<std::uint8_t>(byte ? 0x84 : 0x85)}, srcReg->code, rm, in.location);
        return;
    }
    Error(in.location, "unsupported operands for 'test'");
}

void AssemblerContext::EncodeLea(const AsmInstr &in) {
    if (in.operands.size() != 2 || in.operands[0].kind != AsmOperand::Kind::Reg ||
        in.operands[1].kind != AsmOperand::Kind::Mem) {
        Error(in.location, "'lea' expects: lea reg, [memory]");
        return;
    }
    auto dstReg = Reg(in.operands[0]);
    if (!dstReg) {
        return;
    }
    std::optional<AsmRegInfo> ignore;
    RmEnc rm = EncodeRm(in.operands[1], ignore);
    EmitModRM(dstReg->size, {0x8D}, dstReg->code, rm, in.location);
}

void AssemblerContext::EncodeXchg(const AsmInstr &in) {
    if (in.operands.size() != 2) {
        Error(in.location, "'xchg' expects 2 operands");
        return;
    }
    const AsmOperand &dst = in.operands[0];
    const AsmOperand &src = in.operands[1];
    const int opSize = OperandSize(in, 8);
    const bool byte = opSize == 1;
    if (src.kind == AsmOperand::Kind::Reg && (dst.kind == AsmOperand::Kind::Reg || dst.kind == AsmOperand::Kind::Mem)) {
        auto srcReg = Reg(src);
        if (!srcReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(dst, ignore);
        rm.rexRequired = rm.rexRequired || srcReg->rexRequired;
        rm.rexForbidden = rm.rexForbidden || srcReg->high8;
        EmitModRM(opSize, {static_cast<std::uint8_t>(byte ? 0x86 : 0x87)}, srcReg->code, rm, in.location);
        return;
    }
    if (dst.kind == AsmOperand::Kind::Reg && src.kind == AsmOperand::Kind::Mem) {
        auto dstReg = Reg(dst);
        if (!dstReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(src, ignore);
        rm.rexRequired = rm.rexRequired || dstReg->rexRequired;
        rm.rexForbidden = rm.rexForbidden || dstReg->high8;
        EmitModRM(opSize, {static_cast<std::uint8_t>(byte ? 0x86 : 0x87)}, dstReg->code, rm, in.location);
        return;
    }
    Error(in.location, "unsupported operands for 'xchg'");
}

void AssemblerContext::EncodeCmpxchg(const AsmInstr &in) {
    if (in.operands.size() != 2) {
        Error(in.location, "'cmpxchg' expects 2 operands");
        return;
    }
    const AsmOperand &dst = in.operands[0];
    const AsmOperand &src = in.operands[1];
    const int opSize = OperandSize(in, 8);
    const bool byte = opSize == 1;
    if (src.kind == AsmOperand::Kind::Reg && (dst.kind == AsmOperand::Kind::Reg || dst.kind == AsmOperand::Kind::Mem)) {
        auto srcReg = Reg(src);
        if (!srcReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(dst, ignore);
        rm.rexRequired = rm.rexRequired || srcReg->rexRequired;
        rm.rexForbidden = rm.rexForbidden || srcReg->high8;
        EmitModRM(opSize, {0x0F, static_cast<std::uint8_t>(byte ? 0xB0 : 0xB1)}, srcReg->code, rm, in.location);
        return;
    }
    Error(in.location, "unsupported operands for 'cmpxchg'");
}

void AssemblerContext::EncodeXadd(const AsmInstr &in) {
    if (in.operands.size() != 2) {
        Error(in.location, "'xadd' expects 2 operands");
        return;
    }
    const AsmOperand &dst = in.operands[0];
    const AsmOperand &src = in.operands[1];
    const int opSize = OperandSize(in, 8);
    const bool byte = opSize == 1;
    if (src.kind == AsmOperand::Kind::Reg && (dst.kind == AsmOperand::Kind::Reg || dst.kind == AsmOperand::Kind::Mem)) {
        auto srcReg = Reg(src);
        if (!srcReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(dst, ignore);
        rm.rexRequired = rm.rexRequired || srcReg->rexRequired;
        rm.rexForbidden = rm.rexForbidden || srcReg->high8;
        EmitModRM(opSize, {0x0F, static_cast<std::uint8_t>(byte ? 0xC0 : 0xC1)}, srcReg->code, rm, in.location);
        return;
    }
    Error(in.location, "unsupported operands for 'xadd'");
}

// movzx / movsx: reg, r/m of a smaller width.
void AssemblerContext::EncodeMovExtend(const AsmInstr &in, bool signExtend) {
    if (in.operands.size() != 2 || in.operands[0].kind != AsmOperand::Kind::Reg) {
        Error(in.location, std::format("'{}' expects: {} reg, r/m", in.mnemonic, in.mnemonic));
        return;
    }
    auto dstReg = Reg(in.operands[0]);
    if (!dstReg) {
        return;
    }
    const AsmOperand &src = in.operands[1];
    int srcSize = 0;
    if (src.kind == AsmOperand::Kind::Reg) {
        AsmRegInfo s = LookupRegister(Target::Arch::X86_64, src.name);
        srcSize = s.size;
    }
    else if (src.kind == AsmOperand::Kind::Mem) {
        srcSize = src.memSize;
    }
    if (srcSize != 1 && srcSize != 2 && srcSize != 4) {
        Error(in.location, std::format("'{}' needs an 8/16/32-bit source (add a size prefix)", in.mnemonic));
        return;
    }
    std::optional<AsmRegInfo> ignore;
    RmEnc rm = EncodeRm(src, ignore);
    if (srcSize == 4) {
        // Only movsxd exists (0x63); movzx from 32 is a plain 32-bit mov.
        if (signExtend) {
            EmitModRM(dstReg->size, {0x63}, dstReg->code, rm, in.location);
        }
        else {
            EmitModRM(4, {0x8B}, dstReg->code, rm, in.location);
        }
        return;
    }
    const std::uint8_t op2 = signExtend ? (srcSize == 1 ? 0xBE : 0xBF) : (srcSize == 1 ? 0xB6 : 0xB7);
    EmitModRM(dstReg->size, {0x0F, op2}, dstReg->code, rm, in.location);
}

void AssemblerContext::EncodeImul(const AsmInstr &in) {
    // imul r/m                         -> 0xF7 /5      (one operand)
    // imul reg, r/m                    -> 0x0F 0xAF    (two operands)
    // imul reg, imm  == imul reg,reg,imm
    // imul reg, r/m, imm               -> 0x69 id / 0x6B ib (immediate form)
    if (in.operands.size() == 1) {
        EncodeUnaryGroup(in, 5);
        return;
    }

    // Immediate form: last operand is an immediate.
    const bool hasImm = !in.operands.empty() && in.operands.back().kind == AsmOperand::Kind::Imm;
    if (hasImm && (in.operands.size() == 2 || in.operands.size() == 3) &&
        in.operands[0].kind == AsmOperand::Kind::Reg) {
        auto dstReg = Reg(in.operands[0]);
        if (!dstReg) {
            return;
        }
        // Source r/m: the middle operand, or the destination itself for the
        // two-operand shorthand `imul reg, imm`.
        const AsmOperand &src = in.operands.size() == 3 ? in.operands[1] : in.operands[0];
        const std::int64_t imm = in.operands.back().imm;
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(src, ignore);
        const bool imm8 = FitsInt8(imm);
        if (!imm8 && !FitsInt32(imm)) {
            Error(in.operands.back().location, Signed32EncodingRangeDiagnostic("imul", imm));
            return;
        }
        EmitModRM(dstReg->size, {static_cast<std::uint8_t>(imm8 ? 0x6B : 0x69)}, dstReg->code, rm, in.location);
        if (imm8) {
            Emit8(static_cast<std::uint8_t>(imm));
        }
        else {
            Emit32(static_cast<std::int32_t>(imm));
        }
        return;
    }

    if (in.operands.size() == 2 && in.operands[0].kind == AsmOperand::Kind::Reg) {
        // imul reg, r/m -> 0x0F 0xAF
        auto dstReg = Reg(in.operands[0]);
        if (!dstReg) {
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(in.operands[1], ignore);
        EmitModRM(dstReg->size, {0x0F, 0xAF}, dstReg->code, rm, in.location);
        return;
    }
    Error(in.location, "unsupported operands for 'imul'");
}

// Shifts: C0/C1 (imm8), D2/D3 (cl), grouped by /ext.
void AssemblerContext::EncodeShift(const AsmInstr &in, int ext) {
    if (in.operands.size() != 2) {
        Error(in.location, std::format("'{}' expects 2 operands", in.mnemonic));
        return;
    }
    const int opSize = OperandSize(in, 8);
    std::optional<AsmRegInfo> ignore;
    RmEnc rm = EncodeRm(in.operands[0], ignore);
    const AsmOperand &count = in.operands[1];
    if (count.kind == AsmOperand::Kind::Reg && count.name == "cl") {
        EmitModRM(opSize, {static_cast<std::uint8_t>(opSize == 1 ? 0xD2 : 0xD3)}, ext, rm, in.location);
        return;
    }
    if (count.kind == AsmOperand::Kind::Imm) {
        EmitModRM(opSize, {static_cast<std::uint8_t>(opSize == 1 ? 0xC0 : 0xC1)}, ext, rm, in.location);
        Emit8(static_cast<std::uint8_t>(count.imm));
        return;
    }
    Error(in.location, std::format("'{}' count must be an immediate or cl", in.mnemonic));
}

void AssemblerContext::EncodePush(const AsmInstr &in) {
    if (in.operands.size() != 1) {
        Error(in.location, "'push' expects 1 operand");
        return;
    }
    const AsmOperand &op = in.operands[0];
    if (op.kind == AsmOperand::Kind::Reg) {
        auto r = Reg(op);
        if (!r) {
            return;
        }
        if (r->code >= 8) {
            Emit8(0x41);
        }
        Emit8(static_cast<std::uint8_t>(0x50 + (r->code & 7)));
        return;
    }
    if (op.kind == AsmOperand::Kind::Imm) {
        if (FitsInt8(op.imm)) {
            Emit8(0x6A);
            Emit8(static_cast<std::uint8_t>(op.imm));
        }
        else {
            Emit8(0x68);
            Emit32(static_cast<std::int32_t>(op.imm));
        }
        return;
    }
    if (op.kind == AsmOperand::Kind::Mem) {
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(op, ignore);
        // push r/m64 -> 0xFF /6 (no REX.W needed for the 64-bit default).
        EmitModRM(4, {0xFF}, 6, rm, in.location);
        return;
    }
    Error(in.location, "unsupported operand for 'push'");
}

void AssemblerContext::EncodePop(const AsmInstr &in) {
    if (in.operands.size() != 1) {
        Error(in.location, "'pop' expects 1 operand");
        return;
    }
    const AsmOperand &op = in.operands[0];
    if (op.kind == AsmOperand::Kind::Reg) {
        auto r = Reg(op);
        if (!r) {
            return;
        }
        if (r->code >= 8) {
            Emit8(0x41);
        }
        Emit8(static_cast<std::uint8_t>(0x58 + (r->code & 7)));
        return;
    }
    if (op.kind == AsmOperand::Kind::Mem) {
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(op, ignore);
        EmitModRM(4, {0x8F}, 0, rm, in.location);
        return;
    }
    Error(in.location, "unsupported operand for 'pop'");
}

// call / jmp: direct to a label/symbol (rel32) or indirect through r/m.
void AssemblerContext::EncodeCallJmp(const AsmInstr &in, bool isCall) {
    if (in.operands.size() != 1) {
        Error(in.location, std::format("'{}' expects 1 operand", in.mnemonic));
        return;
    }
    const AsmOperand &op = in.operands[0];
    if (op.kind == AsmOperand::Kind::Reg || op.kind == AsmOperand::Kind::Mem) {
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(op, ignore);
        EmitModRM(4, {0xFF}, isCall ? 2 : 4, rm, in.location);
        return;
    }
    if (op.kind == AsmOperand::Kind::Sym) {
        Emit8(isCall ? 0xE8 : 0xE9);
        EmitRel32Target(op.name, op.location);
        return;
    }
    Error(in.location, std::format("unsupported operand for '{}'", in.mnemonic));
}
} // namespace Rux::X86_64AssemblerPrivate
