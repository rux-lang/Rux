#include "CodeGen/X86_64/AssemblerContext.h"

namespace Rux::X86_64AssemblerPrivate {

// Two-operand SSE op with the shape `xmm, xmm/mem` (dst in the reg field,
// src in r/m): the scalar/packed arithmetic, bitwise and compare ops.
void AssemblerContext::EncodeSseRegRm(const AsmInstr &in, std::uint8_t prefix, std::uint8_t opcode) {
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
    std::optional<AsmRegInfo> ignore;
    RmEnc rm = EncodeRm(src, ignore);
    EmitSse(prefix, {0x0F, opcode}, dst->code, rm, false, in.location);
}

// movsd/movss/movaps/movapd/movups/movupd: a bidirectional data move with a
// load opcode (reg <- r/m) and a store opcode (r/m <- reg).
void AssemblerContext::EncodeSseMove(const AsmInstr &in, std::uint8_t prefix, std::uint8_t loadOp,
                                     std::uint8_t storeOp) {
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
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(src, ignore);
        EmitSse(prefix, {0x0F, loadOp}, d->code, rm, false, in.location);
        return;
    }
    if (dst.kind == AsmOperand::Kind::Mem && IsXmmOperand(src)) {
        auto s = Xmm(src);
        std::optional<AsmRegInfo> ignore;
        RmEnc rm = EncodeRm(dst, ignore);
        EmitSse(prefix, {0x0F, storeOp}, s->code, rm, false, in.location);
        return;
    }
    Error(in.location, std::format("unsupported operands for '{}'", in.mnemonic));
}

// movd/movq: move between an XMM register and a GP register/memory, or (movq
// only) between two XMM registers / an XMM register and a 64-bit slot.
void AssemblerContext::EncodeMovdq(const AsmInstr &in, bool isQ) {
    if (in.operands.size() != 2) {
        Error(in.location, std::format("'{}' expects 2 operands", in.mnemonic));
        return;
    }
    const AsmOperand &dst = in.operands[0];
    const AsmOperand &src = in.operands[1];
    std::optional<AsmRegInfo> ignore;

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
void AssemblerContext::EncodeCvtsi2(const AsmInstr &in, std::uint8_t prefix) {
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
        AsmRegInfo r = LookupRegister(Target::Arch::X86_64, src.name);
        if (!r.valid || r.IsVector() || (r.size != 4 && r.size != 8)) {
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
    std::optional<AsmRegInfo> ignore;
    RmEnc rm = EncodeRm(src, ignore);
    EmitSse(prefix, {0x0F, 0x2A}, dst->code, rm, srcSize == 8, in.location);
}

// cvtsd2si / cvttsd2si / cvtss2si / cvttss2si: gpr <- xmm/mem. REX.W follows
// the destination gpr width.
void AssemblerContext::EncodeCvt2si(const AsmInstr &in, std::uint8_t prefix, std::uint8_t opcode) {
    if (in.operands.size() != 2 || in.operands[0].kind != AsmOperand::Kind::Reg) {
        Error(in.location, std::format("'{}' expects: {} gpr, xmm/mem", in.mnemonic, in.mnemonic));
        return;
    }
    AsmRegInfo d = LookupRegister(Target::Arch::X86_64, in.operands[0].name);
    if (!d.valid || d.IsVector() || (d.size != 4 && d.size != 8)) {
        Error(in.operands[0].location, std::format("'{}' destination must be a 32- or 64-bit gpr", in.mnemonic));
        return;
    }
    const AsmOperand &src = in.operands[1];
    if (src.kind == AsmOperand::Kind::Reg && !IsXmmOperand(src)) {
        Error(src.location, std::format("'{}' source must be an xmm register or memory", in.mnemonic));
        return;
    }
    std::optional<AsmRegInfo> ignore;
    RmEnc rm = EncodeRm(src, ignore);
    EmitSse(prefix, {0x0F, opcode}, d.code, rm, d.size == 8, in.location);
}
} // namespace Rux::X86_64AssemblerPrivate
