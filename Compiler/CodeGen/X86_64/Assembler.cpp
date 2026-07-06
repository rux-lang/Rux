// x86-64 assembler for `asm func` bodies. Intel syntax (destination first).
//
// The supported subset covers the instructions typically needed in hand-written
// leaf functions: the ALU ops (mov/add/sub/and/or/xor/cmp/test/adc/sbb),
// lea, the unary/multiply/divide group, shifts, push/pop, movzx/movsx/movsxd,
// setcc, the full jcc family, call/jmp (direct and indirect), ret/leave/nop,
// syscall, int, and the sign-extend helpers (cqo/cdq/cdqe). It also covers a
// scalar/packed SSE/SSE2 subset: the arithmetic ops (add/sub/mul/div/sqrt/
// min/max, sd and ss), the moves (movsd/movss, movaps/movapd/movups/movupd,
// movd/movq), the bitwise ops (xorps/andps/orps/andnps and the pd/integer
// variants, pxor/pand/por), the ordered/unordered compares (comisd/ucomisd
// and ss), and the int<->float conversions (cvtsi2sd/ss, cvt(t)sd2si, cvt(t)
// ss2si, cvtsd2ss, cvtss2sd). Operands may be registers (GP or xmm),
// immediates, or memory references of the form [base + index*scale +/- disp],
// optionally with a byte/word/dword/qword size prefix.

#include "CodeGen/X86_64/Assembler.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "Object/Rcu/Rcu.h"

namespace Rux {
namespace {

using Bytes = std::vector<std::uint8_t>;

// Resolved r/m encoding: the ModRM byte with an empty reg field, plus the SIB,
// displacement, REX.B/X bits, and any rip-relative symbol reference.
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

class Assembler {
public:
    Assembler(const std::vector<AsmInstr> &instrs, std::string sourceName, Bytes &out)
        : instrs_(instrs)
        , sourceName_(std::move(sourceName))
        , out_(out) {
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
        ResolveLocalJumps();
        result_.ok = result_.diagnostics.empty();
        return std::move(result_);
    }

private:
    const std::vector<AsmInstr> &instrs_;
    std::string sourceName_;
    Bytes &out_;
    AsmAssembly result_;

    // Label name -> offset within out_ (absolute).
    std::unordered_map<std::string, std::uint32_t> labels_;

    // Pending rel32 fixups to local labels: (field offset, target label).
    struct LocalJump {
        std::uint32_t fieldOff;
        std::string label;
        SourceLocation loc;
    };

    std::vector<LocalJump> localJumps_;

    void Error(const SourceLocation &loc, std::string msg) {
        Diagnostic d;
        d.severity = Diagnostic::Severity::Error;
        d.message = std::move(msg);
        d.location = loc;
        d.sourceName = sourceName_;
        result_.diagnostics.push_back(std::move(d));
    }

    // Emission primitives
    void Emit8(std::uint8_t b) {
        out_.push_back(b);
    }

    void Emit32(std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        out_.push_back(u & 0xFF);
        out_.push_back((u >> 8) & 0xFF);
        out_.push_back((u >> 16) & 0xFF);
        out_.push_back((u >> 24) & 0xFF);
    }

    void Emit64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            out_.push_back(v & 0xFF);
            v >>= 8;
        }
    }

    std::uint32_t Here() const {
        return static_cast<std::uint32_t>(out_.size());
    }

    void CollectLabels() {
        // Offsets are filled during encoding; here we only reserve the names so
        // a forward `jmp label` can tell a local label from an extern symbol.
        for (const auto &instr : instrs_) {
            if (!instr.labelDef.empty()) {
                labels_.emplace(instr.labelDef, 0);
            }
        }
    }

    // Resolve one operand that names a register into its info, reporting an
    // error if it is not a known register.
    std::optional<X64RegInfo> Reg(const AsmOperand &op) {
        X64RegInfo info = LookupX64Reg(op.name);
        if (!info.valid) {
            Error(op.location, std::format("unknown register '{}'", op.name));
            return std::nullopt;
        }
        return info;
    }

    // Build the ModRM/SIB/disp encoding for a register operand (mod = 3).
    RmEnc EncodeRmReg(const X64RegInfo &r) {
        RmEnc e;
        e.modrm = static_cast<std::uint8_t>(0xC0 | (r.code & 7));
        e.rexB = r.code >= 8;
        e.rexRequired = r.rexRequired;
        e.rexForbidden = r.high8;
        return e;
    }

    // Build the ModRM/SIB/disp encoding for a memory operand.
    RmEnc EncodeRmMem(const AsmOperand &op) {
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
            X64RegInfo b = LookupX64Reg(op.memBase);
            if (!b.valid || b.size != 8) {
                Error(op.location, std::format("invalid base register '{}'", op.memBase));
            }
            baseCode = b.code;
            e.rexB = b.code >= 8;
        }
        if (hasIndex) {
            X64RegInfo x = LookupX64Reg(op.memIndex);
            if (!x.valid || x.size != 8) {
                Error(op.location, std::format("invalid index register '{}'", op.memIndex));
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

    RmEnc EncodeRm(const AsmOperand &op, std::optional<X64RegInfo> &regOut) {
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
    void EmitModRM(int opSize, std::initializer_list<std::uint8_t> opcodes, int regField, const RmEnc &rm,
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
    void EmitRmTail(int regField, const RmEnc &rm) {
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
                result_.fixups.push_back({fieldOff, rm.ripSymbol, RcuRelType::Rel32, rm.disp});
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
    void EmitSse(std::uint8_t mandatoryPrefix, std::initializer_list<std::uint8_t> opcodes, int regField,
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
    int OperandSize(const AsmInstr &in, int defaultSize) {
        for (const auto &op : in.operands) {
            if (op.kind == AsmOperand::Kind::Reg) {
                if (X64RegInfo r = LookupX64Reg(op.name); r.valid) {
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

    // Encode a signed/zero immediate into `out_` at the appropriate width for an
    // ALU immediate (imm8 when it fits, otherwise imm32).
    void EmitAluImm(std::int64_t v, int opSize, bool useImm8) {
        if (opSize == 1 || useImm8) {
            Emit8(static_cast<std::uint8_t>(v));
        }
        else {
            Emit32(static_cast<std::int32_t>(v));
        }
    }

    static bool FitsInt8(std::int64_t v) {
        return v >= -128 && v <= 127;
    }

    static bool FitsInt32(std::int64_t v) {
        return v >= INT32_MIN && v <= INT32_MAX;
    }

    // --- ALU family (add/or/adc/sbb/and/sub/xor/cmp/mov) -------------------
    // For each op we know: the r/m,reg opcode (odd variant), and the /ext used
    // by the 0x80/0x81/0x83 immediate group.
    struct AluOp {
        std::uint8_t mrOpcode; // r/m, reg  (32/64-bit); the r8 form is this & ~1
        std::uint8_t ext;      // group /digit for the immediate form
    };

    void EncodeAlu(const AsmInstr &in, const AluOp &spec) {
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
            std::optional<X64RegInfo> reg;
            RmEnc rm = EncodeRm(dst, reg);
            const bool useImm8 = !byte && FitsInt8(src.imm);
            if (!byte && !useImm8 && !FitsInt32(src.imm)) {
                Error(src.location, "immediate does not fit in 32 bits");
            }
            const std::uint8_t opcode = byte ? 0x80 : (useImm8 ? 0x83 : 0x81);
            EmitModRM(opSize, {opcode}, spec.ext, rm, in.location);
            EmitAluImm(src.imm, opSize, useImm8);
            return;
        }

        if (src.kind == AsmOperand::Kind::Reg &&
            (dst.kind == AsmOperand::Kind::Reg || dst.kind == AsmOperand::Kind::Mem)) {
            // MR form: opcode = mrOpcode (r/m, reg). reg field = src.
            auto srcReg = Reg(src);
            if (!srcReg) {
                return;
            }
            std::optional<X64RegInfo> ignore;
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
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(src, ignore);
            const std::uint8_t opcode = byte ? ((spec.mrOpcode & ~1) | 2) : (spec.mrOpcode | 2);
            EmitModRM(opSize, {opcode}, dstReg->code, rm, in.location);
            return;
        }

        Error(in.location, std::format("unsupported operands for '{}'", in.mnemonic));
    }

    void EncodeMov(const AsmInstr &in) {
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
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(dst, ignore);
            if (!FitsInt32(src.imm)) {
                Error(src.location, "immediate does not fit in 32 bits");
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
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(mem, ignore);
            EmitModRM(8, {0x8D}, dstReg->code, rm, in.location);
            return;
        }

        // Register/memory forms reuse the ALU MR/RM machinery (opcode 0x88/0x89).
        EncodeAlu(in, {0x89, 0});
    }

    void EncodeTest(const AsmInstr &in) {
        if (in.operands.size() != 2) {
            Error(in.location, "'test' expects 2 operands");
            return;
        }
        const AsmOperand &dst = in.operands[0];
        const AsmOperand &src = in.operands[1];
        const int opSize = OperandSize(in, 8);
        const bool byte = opSize == 1;
        if (src.kind == AsmOperand::Kind::Imm) {
            std::optional<X64RegInfo> ignore;
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
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(dst, ignore);
            EmitModRM(opSize, {static_cast<std::uint8_t>(byte ? 0x84 : 0x85)}, srcReg->code, rm, in.location);
            return;
        }
        Error(in.location, "unsupported operands for 'test'");
    }

    void EncodeLea(const AsmInstr &in) {
        if (in.operands.size() != 2 || in.operands[0].kind != AsmOperand::Kind::Reg ||
            in.operands[1].kind != AsmOperand::Kind::Mem) {
            Error(in.location, "'lea' expects: lea reg, [memory]");
            return;
        }
        auto dstReg = Reg(in.operands[0]);
        if (!dstReg) {
            return;
        }
        std::optional<X64RegInfo> ignore;
        RmEnc rm = EncodeRm(in.operands[1], ignore);
        EmitModRM(dstReg->size, {0x8D}, dstReg->code, rm, in.location);
    }

    // movzx / movsx: reg, r/m of a smaller width.
    void EncodeMovExtend(const AsmInstr &in, bool signExtend) {
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
            X64RegInfo s = LookupX64Reg(src.name);
            srcSize = s.size;
        }
        else if (src.kind == AsmOperand::Kind::Mem) {
            srcSize = src.memSize;
        }
        if (srcSize != 1 && srcSize != 2 && srcSize != 4) {
            Error(in.location, std::format("'{}' needs an 8/16/32-bit source (add a size prefix)", in.mnemonic));
            return;
        }
        std::optional<X64RegInfo> ignore;
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

    // Unary group F6/F7 (/ext): not, neg, mul, imul(1), div, idiv.
    void EncodeUnaryGroup(const AsmInstr &in, int ext) {
        if (in.operands.size() != 1) {
            Error(in.location, std::format("'{}' expects 1 operand", in.mnemonic));
            return;
        }
        const int opSize = OperandSize(in, 8);
        std::optional<X64RegInfo> ignore;
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
        std::optional<X64RegInfo> ignore;
        RmEnc rm = EncodeRm(in.operands[0], ignore);
        EmitModRM(opSize, {static_cast<std::uint8_t>(opSize == 1 ? 0xFE : 0xFF)}, ext, rm, in.location);
    }

    void EncodeImul(const AsmInstr &in) {
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
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(src, ignore);
            const bool imm8 = FitsInt8(imm);
            if (!imm8 && !FitsInt32(imm)) {
                Error(in.location, "immediate does not fit in 32 bits");
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
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(in.operands[1], ignore);
            EmitModRM(dstReg->size, {0x0F, 0xAF}, dstReg->code, rm, in.location);
            return;
        }
        Error(in.location, "unsupported operands for 'imul'");
    }

    // Shifts: C0/C1 (imm8), D2/D3 (cl), grouped by /ext.
    void EncodeShift(const AsmInstr &in, int ext) {
        if (in.operands.size() != 2) {
            Error(in.location, std::format("'{}' expects 2 operands", in.mnemonic));
            return;
        }
        const int opSize = OperandSize(in, 8);
        std::optional<X64RegInfo> ignore;
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

    void EncodePush(const AsmInstr &in) {
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
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(op, ignore);
            // push r/m64 -> 0xFF /6 (no REX.W needed for the 64-bit default).
            EmitModRM(4, {0xFF}, 6, rm, in.location);
            return;
        }
        Error(in.location, "unsupported operand for 'push'");
    }

    void EncodePop(const AsmInstr &in) {
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
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(op, ignore);
            EmitModRM(4, {0x8F}, 0, rm, in.location);
            return;
        }
        Error(in.location, "unsupported operand for 'pop'");
    }

    // call / jmp: direct to a label/symbol (rel32) or indirect through r/m.
    void EncodeCallJmp(const AsmInstr &in, bool isCall) {
        if (in.operands.size() != 1) {
            Error(in.location, std::format("'{}' expects 1 operand", in.mnemonic));
            return;
        }
        const AsmOperand &op = in.operands[0];
        if (op.kind == AsmOperand::Kind::Reg || op.kind == AsmOperand::Kind::Mem) {
            std::optional<X64RegInfo> ignore;
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
        std::optional<X64RegInfo> ignore;
        RmEnc rm = EncodeRm(in.operands[0], ignore);
        // setcc operates on r/m8.
        EmitModRM(1, {0x0F, ccOpcode}, 0, rm, in.location);
    }

    // Emit a rel32 field targeting `name` — a local label (resolved later) or
    // an external symbol (reported as a fixup).
    void EmitRel32Target(const std::string &name, const SourceLocation &loc) {
        const std::uint32_t fieldOff = Here();
        Emit32(0);
        if (labels_.contains(name)) {
            localJumps_.push_back({fieldOff, name, loc});
        }
        else {
            // The linker resolves rel32 as targetVA + addend - (site + 4).
            result_.fixups.push_back({fieldOff, name, RcuRelType::Rel32, 0});
        }
    }

    void ResolveLocalJumps() {
        for (const auto &j : localJumps_) {
            const auto it = labels_.find(j.label);
            if (it == labels_.end()) {
                Error(j.loc, std::format("undefined label '{}'", j.label));
                continue;
            }
            const auto target = static_cast<std::int32_t>(it->second);
            const std::int32_t rel = target - static_cast<std::int32_t>(j.fieldOff + 4);
            out_[j.fieldOff] = rel & 0xFF;
            out_[j.fieldOff + 1] = (rel >> 8) & 0xFF;
            out_[j.fieldOff + 2] = (rel >> 16) & 0xFF;
            out_[j.fieldOff + 3] = (rel >> 24) & 0xFF;
        }
    }

    // --- SSE / SSE2 -------------------------------------------------------
    static bool IsXmmOperand(const AsmOperand &op) {
        return op.kind == AsmOperand::Kind::Reg && LookupX64Reg(op.name).isXmm;
    }

    // Resolve an operand that must name an XMM register.
    std::optional<X64RegInfo> Xmm(const AsmOperand &op) {
        if (!IsXmmOperand(op)) {
            Error(op.location, "expected an xmm register");
            return std::nullopt;
        }
        return LookupX64Reg(op.name);
    }

    // Two-operand SSE op with the shape `xmm, xmm/mem` (dst in the reg field,
    // src in r/m): the scalar/packed arithmetic, bitwise and compare ops.
    void EncodeSseRegRm(const AsmInstr &in, std::uint8_t prefix, std::uint8_t opcode) {
        if (in.operands.size() != 2) {
            Error(in.location, std::format("'{}' expects 2 operands", in.mnemonic));
            return;
        }
        auto dst = Xmm(in.operands[0]);
        if (!dst) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        if (src.kind == AsmOperand::Kind::Reg && !IsXmmOperand(src)) {
            Error(src.location, std::format("'{}' source must be an xmm register or memory", in.mnemonic));
            return;
        }
        std::optional<X64RegInfo> ignore;
        RmEnc rm = EncodeRm(src, ignore);
        EmitSse(prefix, {0x0F, opcode}, dst->code, rm, false, in.location);
    }

    // movsd/movss/movaps/movapd/movups/movupd: a bidirectional data move with a
    // load opcode (reg <- r/m) and a store opcode (r/m <- reg).
    void EncodeSseMove(const AsmInstr &in, std::uint8_t prefix, std::uint8_t loadOp, std::uint8_t storeOp) {
        if (in.operands.size() != 2) {
            Error(in.location, std::format("'{}' expects 2 operands", in.mnemonic));
            return;
        }
        const AsmOperand &dst = in.operands[0];
        const AsmOperand &src = in.operands[1];
        if (IsXmmOperand(dst)) {
            if (src.kind == AsmOperand::Kind::Reg && !IsXmmOperand(src)) {
                Error(src.location, std::format("'{}' source must be an xmm register or memory", in.mnemonic));
                return;
            }
            auto d = Xmm(dst);
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(src, ignore);
            EmitSse(prefix, {0x0F, loadOp}, d->code, rm, false, in.location);
            return;
        }
        if (dst.kind == AsmOperand::Kind::Mem && IsXmmOperand(src)) {
            auto s = Xmm(src);
            std::optional<X64RegInfo> ignore;
            RmEnc rm = EncodeRm(dst, ignore);
            EmitSse(prefix, {0x0F, storeOp}, s->code, rm, false, in.location);
            return;
        }
        Error(in.location, std::format("unsupported operands for '{}'", in.mnemonic));
    }

    // movd/movq: move between an XMM register and a GP register/memory, or (movq
    // only) between two XMM registers / an XMM register and a 64-bit slot.
    void EncodeMovdq(const AsmInstr &in, bool isQ) {
        if (in.operands.size() != 2) {
            Error(in.location, std::format("'{}' expects 2 operands", in.mnemonic));
            return;
        }
        const AsmOperand &dst = in.operands[0];
        const AsmOperand &src = in.operands[1];
        std::optional<X64RegInfo> ignore;

        // Pure SSE data moves (no GP register), movq only: xmm <- xmm/m64 uses
        // F3 0F 7E; m64 <- xmm uses 66 0F D6.
        if (isQ && IsXmmOperand(dst) && (IsXmmOperand(src) || src.kind == AsmOperand::Kind::Mem)) {
            auto d = Xmm(dst);
            RmEnc rm = EncodeRm(src, ignore);
            EmitSse(0xF3, {0x0F, 0x7E}, d->code, rm, false, in.location);
            return;
        }
        if (isQ && dst.kind == AsmOperand::Kind::Mem && IsXmmOperand(src)) {
            auto s = Xmm(src);
            RmEnc rm = EncodeRm(dst, ignore);
            EmitSse(0x66, {0x0F, 0xD6}, s->code, rm, false, in.location);
            return;
        }

        // GP register / memory <-> XMM. 66 0F 6E loads the XMM from r/m; 66 0F 7E
        // stores it. REX.W selects the 64-bit (movq) width.
        if (IsXmmOperand(dst) && !IsXmmOperand(src) &&
            (src.kind == AsmOperand::Kind::Reg || src.kind == AsmOperand::Kind::Mem)) {
            auto d = Xmm(dst);
            RmEnc rm = EncodeRm(src, ignore);
            EmitSse(0x66, {0x0F, 0x6E}, d->code, rm, isQ, in.location);
            return;
        }
        if (IsXmmOperand(src) && !IsXmmOperand(dst) &&
            (dst.kind == AsmOperand::Kind::Reg || dst.kind == AsmOperand::Kind::Mem)) {
            auto s = Xmm(src);
            RmEnc rm = EncodeRm(dst, ignore);
            EmitSse(0x66, {0x0F, 0x7E}, s->code, rm, isQ, in.location);
            return;
        }
        Error(in.location, std::format("unsupported operands for '{}'", in.mnemonic));
    }

    // cvtsi2sd / cvtsi2ss: xmm <- r/m integer. REX.W follows the source width.
    void EncodeCvtsi2(const AsmInstr &in, std::uint8_t prefix) {
        if (in.operands.size() != 2) {
            Error(in.location, std::format("'{}' expects 2 operands", in.mnemonic));
            return;
        }
        auto dst = Xmm(in.operands[0]);
        if (!dst) {
            return;
        }
        const AsmOperand &src = in.operands[1];
        int srcSize = 4;
        if (src.kind == AsmOperand::Kind::Reg) {
            X64RegInfo r = LookupX64Reg(src.name);
            if (!r.valid || r.isXmm || (r.size != 4 && r.size != 8)) {
                Error(src.location, std::format("'{}' source must be a 32- or 64-bit gpr or memory", in.mnemonic));
                return;
            }
            srcSize = r.size;
        }
        else if (src.kind == AsmOperand::Kind::Mem) {
            srcSize = src.memSize == 8 ? 8 : 4;
        }
        else {
            Error(src.location, std::format("'{}' source must be a gpr or memory", in.mnemonic));
            return;
        }
        std::optional<X64RegInfo> ignore;
        RmEnc rm = EncodeRm(src, ignore);
        EmitSse(prefix, {0x0F, 0x2A}, dst->code, rm, srcSize == 8, in.location);
    }

    // cvtsd2si / cvttsd2si / cvtss2si / cvttss2si: gpr <- xmm/mem. REX.W follows
    // the destination gpr width.
    void EncodeCvt2si(const AsmInstr &in, std::uint8_t prefix, std::uint8_t opcode) {
        if (in.operands.size() != 2 || in.operands[0].kind != AsmOperand::Kind::Reg) {
            Error(in.location, std::format("'{}' expects: {} gpr, xmm/mem", in.mnemonic, in.mnemonic));
            return;
        }
        X64RegInfo d = LookupX64Reg(in.operands[0].name);
        if (!d.valid || d.isXmm || (d.size != 4 && d.size != 8)) {
            Error(in.operands[0].location, std::format("'{}' destination must be a 32- or 64-bit gpr", in.mnemonic));
            return;
        }
        const AsmOperand &src = in.operands[1];
        if (src.kind == AsmOperand::Kind::Reg && !IsXmmOperand(src)) {
            Error(src.location, std::format("'{}' source must be an xmm register or memory", in.mnemonic));
            return;
        }
        std::optional<X64RegInfo> ignore;
        RmEnc rm = EncodeRm(src, ignore);
        EmitSse(prefix, {0x0F, opcode}, d.code, rm, d.size == 8, in.location);
    }

    void EncodeInstr(const AsmInstr &in) {
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

        // SSE/SSE2. Two-operand `xmm, xmm/mem` ops keyed by (prefix, opcode);
        // prefix 0 = none, 0x66/0xF2/0xF3 = the mandatory prefix.
        struct SseOp {
            std::uint8_t prefix;
            std::uint8_t opcode;
        };

        static const std::unordered_map<std::string_view, SseOp> sse = {
            {"sqrtsd", {0xF2, 0x51}},   {"sqrtss", {0xF3, 0x51}},  {"addsd", {0xF2, 0x58}},
            {"addss", {0xF3, 0x58}},    {"subsd", {0xF2, 0x5C}},   {"subss", {0xF3, 0x5C}},
            {"mulsd", {0xF2, 0x59}},    {"mulss", {0xF3, 0x59}},   {"divsd", {0xF2, 0x5E}},
            {"divss", {0xF3, 0x5E}},    {"minsd", {0xF2, 0x5D}},   {"minss", {0xF3, 0x5D}},
            {"maxsd", {0xF2, 0x5F}},    {"maxss", {0xF3, 0x5F}},   {"cvtsd2ss", {0xF2, 0x5A}},
            {"cvtss2sd", {0xF3, 0x5A}}, {"ucomisd", {0x66, 0x2E}}, {"ucomiss", {0x00, 0x2E}},
            {"comisd", {0x66, 0x2F}},   {"comiss", {0x00, 0x2F}},  {"xorps", {0x00, 0x57}},
            {"xorpd", {0x66, 0x57}},    {"andps", {0x00, 0x54}},   {"andpd", {0x66, 0x54}},
            {"orps", {0x00, 0x56}},     {"orpd", {0x66, 0x56}},    {"andnps", {0x00, 0x55}},
            {"andnpd", {0x66, 0x55}},   {"pxor", {0x66, 0xEF}},    {"pand", {0x66, 0xDB}},
            {"por", {0x66, 0xEB}},
        };
        if (const auto it = sse.find(m); it != sse.end()) {
            EncodeSseRegRm(in, it->second.prefix, it->second.opcode);
            return;
        }

        // SSE data moves: (prefix, load opcode, store opcode).
        struct SseMovOp {
            std::uint8_t prefix;
            std::uint8_t loadOp;
            std::uint8_t storeOp;
        };

        static const std::unordered_map<std::string_view, SseMovOp> ssemov = {
            {"movsd", {0xF2, 0x10, 0x11}},  {"movss", {0xF3, 0x10, 0x11}},  {"movups", {0x00, 0x10, 0x11}},
            {"movupd", {0x66, 0x10, 0x11}}, {"movaps", {0x00, 0x28, 0x29}}, {"movapd", {0x66, 0x28, 0x29}},
        };
        if (const auto it = ssemov.find(m); it != ssemov.end()) {
            EncodeSseMove(in, it->second.prefix, it->second.loadOp, it->second.storeOp);
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

        Error(in.location, std::format("unsupported instruction '{}'", m));
    }

    // Map a jCC/setCC mnemonic (given the "j" or "set" prefix) to the low nibble
    // of its condition-code opcode.
    static std::optional<int> ConditionCode(const std::string &m, std::string_view prefix) {
        if (m.size() <= prefix.size() || std::string_view(m).substr(0, prefix.size()) != prefix) {
            return std::nullopt;
        }
        const std::string_view cc = std::string_view(m).substr(prefix.size());
        static const std::unordered_map<std::string_view, int> table = {
            {"o", 0x0},  {"no", 0x1}, {"b", 0x2},  {"c", 0x2},  {"nae", 0x2}, {"ae", 0x3},  {"nb", 0x3}, {"nc", 0x3},
            {"e", 0x4},  {"z", 0x4},  {"ne", 0x5}, {"nz", 0x5}, {"be", 0x6},  {"na", 0x6},  {"a", 0x7},  {"nbe", 0x7},
            {"s", 0x8},  {"ns", 0x9}, {"p", 0xA},  {"pe", 0xA}, {"np", 0xB},  {"po", 0xB},  {"l", 0xC},  {"nge", 0xC},
            {"ge", 0xD}, {"nl", 0xD}, {"le", 0xE}, {"ng", 0xE}, {"g", 0xF},   {"nle", 0xF},
        };
        if (const auto it = table.find(cc); it != table.end()) {
            return it->second;
        }
        return std::nullopt;
    }
};

} // namespace

AsmAssembly AssembleAsmFunc(const std::vector<AsmInstr> &instrs, const std::string &sourceName, Bytes &out) {
    Assembler asmr(instrs, sourceName, out);
    return asmr.Run();
}

} // namespace Rux
