#include "CodeGen/X86_64/AssemblerContext.h"

namespace Rux::X86_64AssemblerPrivate {

AsmAssembly AssemblerContext::Run() {
    labelFixups.Collect(instrs);
    for (const auto &instr : instrs) {
        if (!instr.labelDef.empty()) {
            labelFixups.Define(instr.labelDef, Here());
            continue;
        }
        EncodeInstr(instr);
    }
    labelFixups.Resolve();
    result.ok = result.diagnostics.empty();
    return std::move(result);
}

// Build the ModRM/SIB/disp encoding for a memory operand.
RmEnc AssemblerContext::EncodeRmMem(const AsmOperand &op) {
    RmEnc e;
    const std::int32_t disp = static_cast<std::int32_t>(op.imm);

    // rip-relative: [rip + disp] or [rip + symbol].
    if (op.memBase == "rip") {
        e.modrm = 0x05; // mod=00, rm=101
        e.ripRel = true;
        e.dispLen = 4;
        e.disp = disp;
        e.ripSymbol = op.memSym;
        return e;
    }

    const bool hasBase = !op.memBase.empty();
    const bool hasIndex = !op.memIndex.empty();

    int baseCode = 0;
    int indexCode = 0;
    if (hasBase) {
        AsmRegInfo b = LookupRegister(Target::Arch::X86_64, op.memBase);
        if (!b.valid || b.size != 8) {
            Error(op.location, std::format("'{}' is not an x86-64 base register", op.memBase));
        }
        baseCode = b.code;
        e.rexB = b.code >= 8;
    }
    if (hasIndex) {
        AsmRegInfo x = LookupRegister(Target::Arch::X86_64, op.memIndex);
        if (!x.valid || x.size != 8) {
            Error(op.location, std::format("'{}' is not an x86-64 index register", op.memIndex));
        }
        if (x.code == 4) {
            Error(op.location, "rsp cannot be used as an index register");
        }
        indexCode = x.code;
        e.rexX = x.code >= 8;
    }

    int scaleBits = 0;
    switch (op.memScale) {
    case 1:
        scaleBits = 0;
        break;
    case 2:
        scaleBits = 1;
        break;
    case 4:
        scaleBits = 2;
        break;
    case 8:
        scaleBits = 3;
        break;
    default:
        Error(op.location, std::format("invalid scale {}", op.memScale));
        break;
    }

    auto pickDisp = [&](int baseLow, bool forceDisp) {
        if (!forceDisp && disp == 0 && baseLow != 5) {
            e.dispLen = 0;
            return 0; // mod = 00
        }
        if (disp >= -128 && disp <= 127) {
            e.dispLen = 1;
            e.disp = disp;
            return 1; // mod = 01
        }
        e.dispLen = 4;
        e.disp = disp;
        return 2; // mod = 10
    };

    if (!hasBase && !hasIndex) {
        // Absolute [disp32] via SIB with no base/index.
        e.modrm = 0x04; // mod=00, rm=100
        e.hasSib = true;
        e.sib = static_cast<std::uint8_t>((0 << 6) | (4 << 3) | 5);
        e.dispLen = 4;
        e.disp = disp;
        return e;
    }

    if (hasIndex || (hasBase && (baseCode & 7) == 4)) {
        // SIB form.
        e.hasSib = true;
        const int idx = hasIndex ? (indexCode & 7) : 4; // 4 = no index
        const int bse = hasBase ? (baseCode & 7) : 5;   // 5 = no base (disp32)
        e.sib = static_cast<std::uint8_t>((scaleBits << 6) | (idx << 3) | bse);
        int mod;
        if (!hasBase) {
            mod = 0; // no base: mandatory disp32
            e.dispLen = 4;
            e.disp = disp;
        }
        else {
            mod = pickDisp(baseCode & 7, false);
        }
        e.modrm = static_cast<std::uint8_t>((mod << 6) | 4);
        return e;
    }

    // Base only, no index, base not rsp/r12.
    const int mod = pickDisp(baseCode & 7, false);
    e.modrm = static_cast<std::uint8_t>((mod << 6) | (baseCode & 7));
    return e;
}

RmEnc AssemblerContext::EncodeRm(const AsmOperand &op, std::optional<AsmRegInfo> &regOut) {
    if (op.kind == AsmOperand::Kind::Reg) {
        auto r = Reg(op);
        if (!r) {
            return {};
        }
        regOut = r;
        return EncodeRmReg(*r);
    }
    if (op.kind != AsmOperand::Kind::Mem) {
        Error(op.location, "expected a register or memory operand");
        return {};
    }
    return EncodeRmMem(op);
}

// Emit prefixes + opcode(s) + ModRM/SIB/disp for an instruction whose reg
// field is `regField`. `opSize` is the operand width in bytes.
void AssemblerContext::EmitModRM(int opSize, std::initializer_list<std::uint8_t> opcodes, int regField, const RmEnc &rm,
                                 const SourceLocation &loc) {
    const bool rexR = regField >= 8;
    const bool rexW = opSize == 8;
    bool needRex = rexW || rexR || rm.rexB || rm.rexX || rm.rexRequired;
    if (needRex && rm.rexForbidden) {
        Error(loc, "cannot use a high-byte register (ah/bh/ch/dh) here");
    }
    if (opSize == 2) {
        Emit8(0x66);
    }
    if (needRex) {
        std::uint8_t rex = 0x40;
        if (rexW) {
            rex |= 0x08;
        }
        if (rexR) {
            rex |= 0x04;
        }
        if (rm.rexX) {
            rex |= 0x02;
        }
        if (rm.rexB) {
            rex |= 0x01;
        }
        Emit8(rex);
    }
    for (const std::uint8_t op : opcodes) {
        Emit8(op);
    }
    EmitRmTail(regField, rm);
}

// Emit the ModRM byte (with `regField` folded into its reg slot), then the
// SIB, displacement and any rip-relative relocation. Shared by the integer
// (EmitModRM) and SSE (EmitSse) instruction emitters.
void AssemblerContext::EmitRmTail(int regField, const RmEnc &rm) {
    Emit8(static_cast<std::uint8_t>(rm.modrm | ((regField & 7) << 3)));
    if (rm.hasSib) {
        Emit8(rm.sib);
    }
    if (rm.ripRel) {
        const std::uint32_t fieldOff = Here();
        Emit32(rm.disp);
        if (!rm.ripSymbol.empty()) {
            // The linker patches the whole field as targetVA + addend -
            // (site + 4); fold the in-bracket displacement into the addend.
            result.fixups.push_back({fieldOff, rm.ripSymbol, RcuRelType::Rel32, rm.disp});
        }
    }
    else if (rm.dispLen == 1) {
        Emit8(static_cast<std::uint8_t>(rm.disp));
    }
    else if (rm.dispLen == 4) {
        Emit32(rm.disp);
    }
}

// Emit an SSE/SSE2 instruction: an optional mandatory prefix (0x66/0xF2/0xF3,
// or 0 for none), an optional REX, a two-byte 0F escape opcode, then the
// ModRM/SIB/disp for `rm`. `regField` is the reg-operand register number.
void AssemblerContext::EmitSse(std::uint8_t mandatoryPrefix, std::initializer_list<std::uint8_t> opcodes, int regField,
                               const RmEnc &rm, bool rexW, const SourceLocation &loc) {
    if (mandatoryPrefix != 0) {
        Emit8(mandatoryPrefix);
    }
    const bool rexR = regField >= 8;
    const bool needRex = rexW || rexR || rm.rexB || rm.rexX || rm.rexRequired;
    if (needRex && rm.rexForbidden) {
        Error(loc, "cannot use a high-byte register (ah/bh/ch/dh) here");
    }
    if (needRex) {
        std::uint8_t rex = 0x40;
        if (rexW) {
            rex |= 0x08;
        }
        if (rexR) {
            rex |= 0x04;
        }
        if (rm.rexX) {
            rex |= 0x02;
        }
        if (rm.rexB) {
            rex |= 0x01;
        }
        Emit8(rex);
    }
    for (const std::uint8_t op : opcodes) {
        Emit8(op);
    }
    EmitRmTail(regField, rm);
}

// Determine the operand size (bytes) implied by an instruction's operands.
int AssemblerContext::OperandSize(const AsmInstr &in, int defaultSize) {
    for (const auto &op : in.operands) {
        if (op.kind == AsmOperand::Kind::Reg) {
            if (AsmRegInfo r = LookupRegister(Target::Arch::X86_64, op.name); r.valid) {
                return r.size;
            }
        }
    }
    for (const auto &op : in.operands) {
        if (op.kind == AsmOperand::Kind::Mem && op.memSize != 0) {
            return op.memSize;
        }
    }
    return defaultSize;
}
} // namespace Rux::X86_64AssemblerPrivate
