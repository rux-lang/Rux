#pragma once

#include "CodeGen/BackendDiagnostics.h"
#include "CodeGen/X86_64/AssemblerSupport.h"
#include "Object/Rcu/Rcu.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Rux::X86_64AssemblerPrivate {

using Bytes = std::vector<std::uint8_t>;

/// Resolved r/m encoding: the ModRM byte with an empty reg field, plus the SIB, displacement, REX.B/X bits, and any
/// rip-relative symbol reference.
struct RmEnc {
    std::uint8_t modrm = 0;
    bool hasSib = false;
    std::uint8_t sib = 0;
    int dispLen = 0; // 0, 1 or 4 bytes
    std::int32_t disp = 0;
    bool rexB = false;
    bool rexX = false;
    bool ripRel = false;       // rip-relative: emit a 4-byte field
    std::string ripSymbol;     // non-empty => needs a relocation
    bool rexRequired = false;  // an operand register forces a REX prefix
    bool rexForbidden = false; // a high-8 register forbids a REX prefix
};

class AssemblerContext {
public:
    AssemblerContext(const std::vector<AsmInstr> &inputInstrs, std::string inputSourceName, Bytes &inputOut,
                     const Target::OS inputTargetOs)
        : instrs(inputInstrs)
        , sourceName(std::move(inputSourceName))
        , targetOs(inputTargetOs)
        , out(inputOut)
        , labelFixups(sourceName, out, result) {
    }

    AsmAssembly Run();

private:
    const std::vector<AsmInstr> &instrs;
    std::string sourceName;
    Target::OS targetOs;
    Bytes &out;
    AsmAssembly result;
    LabelFixups labelFixups;

    void Error(const SourceLocation &loc, std::string msg, std::vector<std::string> notes = {},
               std::optional<std::string> help = {}) {
        Diagnostic d = ErrorDiagnostic(std::move(msg), std::move(notes), std::move(help));
        d.location = loc;
        d.sourceName = sourceName;
        result.diagnostics.push_back(std::move(d));
    }

    void Error(const SourceLocation &loc, AsmInstructionDiagnostic diagnostic) {
        Error(loc, std::move(diagnostic.message), std::move(diagnostic.notes), std::move(diagnostic.help));
    }

    // Emission primitives
    void Emit8(std::uint8_t b) {
        out.push_back(b);
    }

    void Emit32(std::int32_t v) {
        Append32(out, v);
    }

    void Emit64(std::uint64_t v) {
        Append64(out, v);
    }

    std::uint32_t Here() const {
        return static_cast<std::uint32_t>(out.size());
    }

    // Resolve one operand that names a register into its info, reporting an
    // error if it is not a known register.
    std::optional<AsmRegInfo> Reg(const AsmOperand &op) {
        AsmRegInfo info = LookupRegister(Target::Arch::X86_64, op.name);
        if (!info.valid) {
            Error(op.location, ClassifyAsmRegister(op.name, targetOs, Target::Arch::X86_64));
            return std::nullopt;
        }
        return info;
    }

    // Build the ModRM/SIB/disp encoding for a register operand (mod = 3).
    RmEnc EncodeRmReg(const AsmRegInfo &r) {
        RmEnc e;
        e.modrm = static_cast<std::uint8_t>(0xC0 | (r.code & 7));
        e.rexB = r.code >= 8;
        e.rexRequired = r.rexRequired;
        e.rexForbidden = r.high8;
        return e;
    }

    // Build the ModRM/SIB/disp encoding for a memory operand.
    RmEnc EncodeRmMem(const AsmOperand &op);

    RmEnc EncodeRm(const AsmOperand &op, std::optional<AsmRegInfo> &regOut);

    // Emit prefixes + opcode(s) + ModRM/SIB/disp for an instruction whose reg
    // field is `regField`. `opSize` is the operand width in bytes.
    void EmitModRM(int opSize, std::initializer_list<std::uint8_t> opcodes, int regField, const RmEnc &rm,
                   const SourceLocation &loc);

    // Emit the ModRM byte (with `regField` folded into its reg slot), then the
    // SIB, displacement and any rip-relative relocation. Shared by the integer
    // (EmitModRM) and SSE (EmitSse) instruction emitters.
    void EmitRmTail(int regField, const RmEnc &rm);

    // Emit an SSE/SSE2 instruction: an optional mandatory prefix (0x66/0xF2/0xF3,
    // or 0 for none), an optional REX, a two-byte 0F escape opcode, then the
    // ModRM/SIB/disp for `rm`. `regField` is the reg-operand register number.
    void EmitSse(std::uint8_t mandatoryPrefix, std::initializer_list<std::uint8_t> opcodes, int regField,
                 const RmEnc &rm, bool rexW, const SourceLocation &loc);

    // Determine the operand size (bytes) implied by an instruction's operands.
    int OperandSize(const AsmInstr &in, int defaultSize);

    // Encode a signed/zero immediate into `out` at the appropriate width for an
    // ALU immediate (imm8 when it fits, otherwise imm32).
    void EmitAluImm(std::int64_t v, int opSize, bool useImm8) {
        if (opSize == 1 || useImm8) {
            Emit8(static_cast<std::uint8_t>(v));
        }
        else {
            Emit32(static_cast<std::int32_t>(v));
        }
    }

    // --- ALU family (add/or/adc/sbb/and/sub/xor/cmp/mov) -------------------
    // For each op we know: the r/m,reg opcode (odd variant), and the /ext used
    // by the 0x80/0x81/0x83 immediate group.
    struct AluOp {
        std::uint8_t mrOpcode; // r/m, reg  (32/64-bit); the r8 form is this & ~1
        std::uint8_t ext;      // group /digit for the immediate form
    };

    void EncodeAlu(const AsmInstr &in, const AluOp &spec);

    void EncodeMov(const AsmInstr &in);

    void EncodeTest(const AsmInstr &in);

    void EncodeLea(const AsmInstr &in);

    void EncodeXchg(const AsmInstr &in);

    void EncodeCmpxchg(const AsmInstr &in);

    void EncodeXadd(const AsmInstr &in);

    // movzx / movsx: reg, r/m of a smaller width.
    void EncodeMovExtend(const AsmInstr &in, bool signExtend);

    // Unary group F6/F7 (/ext): not, neg, mul, imul(1), div, idiv.
    void EncodeUnaryGroup(const AsmInstr &in, int ext) {
        if (in.operands.size() != 1) {
            Error(in.location, std::format("'{}' expects 1 operand", in.mnemonic));
            return;
        }
        const int opSize = OperandSize(in, 8);
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(in.operands[0], ignore);
        EmitModRM(opSize, {static_cast<std::uint8_t>(opSize == 1 ? 0xF6 : 0xF7)}, ext, rm, in.location);
    }

    // inc/dec: FE/FF (/0, /1).
    void EncodeIncDec(const AsmInstr &in, int ext) {
        if (in.operands.size() != 1) {
            Error(in.location, std::format("'{}' expects 1 operand", in.mnemonic));
            return;
        }
        const int opSize = OperandSize(in, 8);
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(in.operands[0], ignore);
        EmitModRM(opSize, {static_cast<std::uint8_t>(opSize == 1 ? 0xFE : 0xFF)}, ext, rm, in.location);
    }

    void EncodeImul(const AsmInstr &in);

    // Shifts: C0/C1 (imm8), D2/D3 (cl), grouped by /ext.
    void EncodeShift(const AsmInstr &in, int ext);

    void EncodePush(const AsmInstr &in);

    void EncodePop(const AsmInstr &in);

    // call / jmp: direct to a label/symbol (rel32) or indirect through r/m.
    void EncodeCallJmp(const AsmInstr &in, bool isCall);

    void EncodeJcc(const AsmInstr &in, std::uint8_t ccOpcode) {
        if (in.operands.size() != 1 || in.operands[0].kind != AsmOperand::Kind::Sym) {
            Error(in.location, std::format("'{}' expects a label", in.mnemonic));
            return;
        }
        Emit8(0x0F);
        Emit8(ccOpcode);
        EmitRel32Target(in.operands[0].name, in.operands[0].location);
    }

    void EncodeSetcc(const AsmInstr &in, std::uint8_t ccOpcode) {
        if (in.operands.size() != 1) {
            Error(in.location, std::format("'{}' expects 1 operand", in.mnemonic));
            return;
        }
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(in.operands[0], ignore);
        // setcc operates on r/m8.
        EmitModRM(1, {0x0F, ccOpcode}, 0, rm, in.location);
    }

    // Emit a rel32 field targeting `name` — a local label (resolved later) or
    // an external symbol (reported as a fixup).
    void EmitRel32Target(const std::string &name, const SourceLocation &loc) {
        const std::uint32_t fieldOff = Here();
        Emit32(0);
        labelFixups.RecordRel32(name, loc, fieldOff);
    }

    // --- SSE / SSE2 -------------------------------------------------------
    static bool IsXmmOperand(const AsmOperand &op) {
        return op.kind == AsmOperand::Kind::Reg && LookupRegister(Target::Arch::X86_64, op.name).IsVector();
    }

    // Resolve an operand that must name an XMM register.
    std::optional<AsmRegInfo> Xmm(const AsmOperand &op) {
        if (!IsXmmOperand(op)) {
            Error(op.location, "expected an xmm register");
            return std::nullopt;
        }
        return LookupRegister(Target::Arch::X86_64, op.name);
    }

    // Two-operand SSE op with the shape `xmm, xmm/mem` (dst in the reg field,
    // src in r/m): the scalar/packed arithmetic, bitwise and compare ops.
    void EncodeSseRegRm(const AsmInstr &in, std::uint8_t prefix, std::uint8_t opcode);

    // movsd/movss/movaps/movapd/movups/movupd: a bidirectional data move with a
    // load opcode (reg <- r/m) and a store opcode (r/m <- reg).
    void EncodeSseMove(const AsmInstr &in, std::uint8_t prefix, std::uint8_t loadOp, std::uint8_t storeOp);

    // movd/movq: move between an XMM register and a GP register/memory, or (movq
    // only) between two XMM registers / an XMM register and a 64-bit slot.
    void EncodeMovdq(const AsmInstr &in, bool isQ);

    // cvtsi2sd / cvtsi2ss: xmm <- r/m integer. REX.W follows the source width.
    void EncodeCvtsi2(const AsmInstr &in, std::uint8_t prefix);

    // cvtsd2si / cvttsd2si / cvtss2si / cvttss2si: gpr <- xmm/mem. REX.W follows
    // the destination gpr width.
    void EncodeCvt2si(const AsmInstr &in, std::uint8_t prefix, std::uint8_t opcode);

    void EncodeInstr(const AsmInstr &in);
};

} // namespace Rux::X86_64AssemblerPrivate
