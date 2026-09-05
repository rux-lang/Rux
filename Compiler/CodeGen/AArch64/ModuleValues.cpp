#include "CodeGen/AArch64/ModuleEmitter.h"

namespace Rux::AArch64Detail {

// Frame layout
//
// The frame record sits at the bottom of the frame, which is where X29
// points once the prologue has run, so every local is at a positive
// displacement from both X29 and SP. That is the opposite of the x86-64
// frame, where RBP sits at the top and a local is reached below it; the
// sign is the only thing that differs, and it differs because STP writes
// upward from the address it has just decremented SP to.

[[nodiscard]] const AArch64FramePlan &AArch64ModuleEmitter::FramePlan() const {
    return *activeFramePlan;
}

// The alignment the running program gives a value of this type, which is
// what decides whether a block copy of it may use the pair forms. AlignOf
// can only derive an alignment from a size, so a named type answers from
// its computed layout wherever the package declared one.
[[nodiscard]] int AArch64ModuleEmitter::RuntimeAlign(const TypeRef &t) const {
    if (!t.IsRange() && t.kind == TypeRef::Kind::Named) {
        const std::string base = BaseTypeName(t.name);
        if (interfaceNames.contains(base)) {
            return 8;
        }
        if (const auto it = layouts.find(base); it != layouts.end()) {
            return it->second.alignment;
        }
    }
    return AlignOf(t);
}

// Whether a value of this type moves as a block of bytes rather than in one
// register. What counts as an aggregate is a property of the LIR type
// rather than of the machine, so this is the x86-64 rule unchanged.
[[nodiscard]] bool AArch64ModuleEmitter::IsAggregate(const TypeRef &t) const {
    if (IsWideInteger(t) || (IsSoftwareFloat(t) && RuntimeSize(t) > 8)) {
        return true;
    }
    if (t.IsRange()) {
        return true;
    }
    // A string is a 16-byte {data, length} view, exactly the shape a slice has, so it is
    // classified and placed the way a slice is.
    if (t.IsString()) {
        return true;
    }
    switch (t.kind) {
    case TypeRef::Kind::Tuple:
    case TypeRef::Kind::Array:
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(t.name);
        return t.isIntrinsicSlice || interfaceNames.contains(base) || layouts.contains(base) ||
               (!t.inner.empty() && SizeOf(t) > 8);
    }
    default:
        return false;
    }
}

// STP writes the frame record at the address it decrements SP to, so the
// whole frame opens in one instruction whenever its size is inside the
// pre-indexed reach. A larger frame opens with FrameAdjust — which is where
// a size past an imm12 becomes a scratch register and a register-form SUB —
// and stores the record afterwards. Either way X29 ends up at the bottom of
// the frame, and SP is a multiple of 16 at every instruction boundary,
// since nothing between the two moves it by anything else.
// The registers the allocation handed out, in the order it handed them out.
// Two runs rather than one list, because STP names two registers of one
// file: a general-purpose register and a vector one have no pair form
// between them however adjacent their slots are.
[[nodiscard]] std::vector<A64Reg> AArch64ModuleEmitter::SavedRegisters(const bool vectorFile) const {
    std::vector<A64Reg> regs;
    if (vectorFile) {
        regs.reserve(FramePlan().SavedVectorRegisters().size());
        for (const unsigned reg : FramePlan().SavedVectorRegisters()) {
            regs.push_back(A64::Dn(reg));
        }
        return regs;
    }
    regs.reserve(FramePlan().SavedGeneralRegisters().size());
    for (const unsigned reg : FramePlan().SavedGeneralRegisters()) {
        regs.push_back(A64::Xn(reg));
    }
    return regs;
}

// Write one run of them into the save area or read it back, two at a time
// wherever the pair immediate reaches — which is all of it unless the area
// sits more than 504 bytes up a large frame, where the single forms take
// over and ResolveMemOperand reaches whatever displacement is left.
void AArch64ModuleEmitter::EmitCalleeSaveRun(const std::vector<A64Reg> &regs, std::int32_t &offset,
                                             const bool restore) {
    std::size_t index = 0;
    while (index + 1 < regs.size() && InPairRange(offset)) {
        const A64Reg first = regs[index];
        const A64Reg second = regs[index + 1];
        Must(restore ? enc.Ldp(first, second, A64::Fp, offset) : enc.Stp(first, second, A64::Fp, offset),
             "the callee-saved registers");
        offset += 16;
        index += 2;
    }
    for (; index < regs.size(); ++index) {
        if (restore) {
            LoadScalar(regs[index], A64::Fp, offset, 8, false);
        }
        else {
            StoreScalar(regs[index], A64::Fp, offset, 8);
        }
        offset += 8;
    }
}

void AArch64ModuleEmitter::EmitPrologue() {
    if (FramePlan().FrameSize() <= kInlineFrameLimit) {
        Must(enc.Stp(A64::Fp, A64::Lr, A64::Sp, -FramePlan().FrameSize(), A64IndexMode::PreIndex), "the frame record");
    }
    else {
        OpenStackArea(FramePlan().FrameSize(), "the frame");
        Must(enc.Stp(A64::Fp, A64::Lr, A64::Sp, 0), "the frame record");
    }
    Must(enc.Mov(A64::Fp, A64::Sp), "the frame pointer");
    EmitCalleeSaves(false);
}

void AArch64ModuleEmitter::EmitEpilogue() {
    EmitCalleeSaves(true);
    if (FramePlan().FrameSize() <= kInlineFrameLimit) {
        Must(enc.Ldp(A64::Fp, A64::Lr, A64::Sp, FramePlan().FrameSize(), A64IndexMode::PostIndex), "the frame record");
    }
    else {
        Must(enc.Ldp(A64::Fp, A64::Lr, A64::Sp, 0), "the frame record");
        Must(enc.FrameAdjust(FramePlan().FrameSize()), "the frame");
    }
    Must(enc.Ret(), "the return");
}

// Memory access
//
// Every access this back end makes is a base register and a displacement:
// a stack slot is the frame pointer and a slot offset, a `load` or a
// `store` is whatever pointer the program computed, and a field of an
// aggregate is that pointer and the field's offset. ResolveMemOperand
// decides how the displacement is reached — the scaled form where it
// divides, the unscaled one where it does not, a scratch register where
// neither reaches — and emits whatever that took, leaving the access itself
// to be written here, because only here is the width and the signedness of
// the value known.

void AArch64ModuleEmitter::StoreScalar(const A64Reg value, const A64Reg base, const std::int64_t offset,
                                       const unsigned width) {
    A64MemOperand mem{};
    Must(enc.ResolveMemOperand(base, offset, width, mem), "a memory address");
    const auto scaled = static_cast<std::uint64_t>(mem.offset);
    // A float has no representation at a width other than its own, so the
    // register decides the access and nothing is narrowed on the way.
    if (value.IsVector()) {
        Must(mem.unscaled ? enc.Stur(value, mem.base, mem.offset) : enc.Str(value, mem.base, scaled), "a store");
        return;
    }
    // A narrowing store names the register it truncates as a W one,
    // whatever width the value arrived in.
    const A64Reg src = A64::Gpr(value.code, width == 8 ? 64 : 32);
    A64Status status = A64Status::Ok;
    switch (width) {
    case 1:
        status = mem.unscaled ? enc.Sturb(src, mem.base, mem.offset) : enc.Strb(src, mem.base, scaled);
        break;
    case 2:
        status = mem.unscaled ? enc.Sturh(src, mem.base, mem.offset) : enc.Strh(src, mem.base, scaled);
        break;
    default:
        status = mem.unscaled ? enc.Stur(src, mem.base, mem.offset) : enc.Str(src, mem.base, scaled);
        break;
    }
    Must(status, "a store");
}

// Load `width` bytes into the 64-bit register `dst`, widening as `sign`
// says: a signed value sign-extends, and an unsigned one is loaded into the
// W view, which zeroes the half of the register above it.
void AArch64ModuleEmitter::LoadScalar(const A64Reg dst, const A64Reg base, const std::int64_t offset,
                                      const unsigned width, const bool sign) {
    A64MemOperand mem{};
    Must(enc.ResolveMemOperand(base, offset, width, mem), "a memory address");
    const auto scaled = static_cast<std::uint64_t>(mem.offset);
    if (dst.IsVector()) {
        Must(mem.unscaled ? enc.Ldur(dst, mem.base, mem.offset) : enc.Ldr(dst, mem.base, scaled), "a load");
        return;
    }
    const A64Reg narrow = A64::Gpr(dst.code, 32);
    A64Status status = A64Status::Ok;
    switch (width) {
    case 1:
        if (sign) {
            status = mem.unscaled ? enc.Ldursb(dst, mem.base, mem.offset) : enc.Ldrsb(dst, mem.base, scaled);
        }
        else {
            status = mem.unscaled ? enc.Ldurb(narrow, mem.base, mem.offset) : enc.Ldrb(narrow, mem.base, scaled);
        }
        break;
    case 2:
        if (sign) {
            status = mem.unscaled ? enc.Ldursh(dst, mem.base, mem.offset) : enc.Ldrsh(dst, mem.base, scaled);
        }
        else {
            status = mem.unscaled ? enc.Ldurh(narrow, mem.base, mem.offset) : enc.Ldrh(narrow, mem.base, scaled);
        }
        break;
    case 4:
        if (sign) {
            status = mem.unscaled ? enc.Ldursw(dst, mem.base, mem.offset) : enc.Ldrsw(dst, mem.base, scaled);
        }
        else {
            status = mem.unscaled ? enc.Ldur(narrow, mem.base, mem.offset) : enc.Ldr(narrow, mem.base, scaled);
        }
        break;
    default:
        status = mem.unscaled ? enc.Ldur(dst, mem.base, mem.offset) : enc.Ldr(dst, mem.base, scaled);
        break;
    }
    Must(status, "a load");
}

// Where a virtual register's value is, which is a slot in the frame or a
// machine register the allocation gave it. The two are the same to every
// caller below: one emits an access and the other a move, and what a
// mention asks for — a width and a signedness — is the same question either
// way.
//
// A general-purpose home holds the whole register and nothing narrows on
// the way in, exactly as the x86-64 back end does. What makes that correct
// is that the reads narrow: a store into a slot writes only the bytes the
// type occupies and a load out of it extends them again, so a `uint8` sum
// wraps because the load reads a byte — and a load out of a register
// extends the same byte for the same reason.

// Bring the value out of `src` at the width `width` and the signedness
// `sign`, which is the extension the load out of a slot would have made.
void AArch64ModuleEmitter::EmitExtendFromHome(const A64Reg dst, const A64Reg src, const unsigned width,
                                              const bool sign) {
    const A64Reg wide = A64::Xn(dst.code);
    const A64Reg narrow = A64::Wn(dst.code);
    const A64Reg source = A64::Wn(src.code);
    A64Status status = A64Status::Ok;
    switch (width) {
    case 1:
        status = sign ? enc.Sxtb(wide, source) : enc.Uxtb(narrow, source);
        break;
    case 2:
        status = sign ? enc.Sxth(wide, source) : enc.Uxth(narrow, source);
        break;
    case 4:
        // The unsigned word is a MOV of the W view, which zeroes the half
        // of the register above it — a whole instruction the extends have
        // no cheaper form of.
        status = sign ? enc.Sxtw(wide, source) : enc.Mov(narrow, source);
        break;
    default:
        status = enc.Mov(wide, A64::Xn(src.code));
        break;
    }
    Must(status, "a value out of its register");
}

// Operands and results
//
// Everything above brings a value to a register the caller named, which is
// what a fixed place — an argument register, a result register — asks for.
// Selection asks something weaker: it wants the value in *some* register,
// and a value already in one is already there. The three below answer that,
// and answering it is where the allocation pays for itself, because a
// mention of a register that has a home costs no instruction at all.

// The register an operand can be read from, and whatever it took to put it
// there. A home holds the whole register, so it answers a mention that
// wants the whole of it; a narrower mention extends out of it into the
// scratch, which is the same instruction count a slot would have cost.
[[nodiscard]] A64Reg AArch64ModuleEmitter::ReadWidthOperand(const LirReg reg, const unsigned width, const bool sign,
                                                            const A64Reg scratch) {
    if (const std::optional<A64Reg> home = GeneralHome(reg)) {
        if (width == 8) {
            return *home;
        }
        EmitExtendFromHome(scratch, *home, width, sign);
        return scratch;
    }
    LoadScalar(scratch, A64::Fp, Disp(reg), width, sign);
    return scratch;
}

// Where a result is computed. Naming the home means the instruction that
// produces the value writes it where every later mention will read it, so
// the store the frame would have needed is not emitted and StoreToSlot
// below finds the value already home.
//
// Only the instruction that finishes a value may take this: a sequence
// that still has an operand to read must not write a register another
// value may be living in, and the pool is shared by every interval that
// does not overlap.
[[nodiscard]] A64Reg AArch64ModuleEmitter::ResultRegister(const LirReg reg, const A64Reg scratch) const {
    if (const std::optional<A64Reg> home = GeneralHome(reg)) {
        return A64::Gpr(home->code, scratch.bits);
    }
    return scratch;
}

// Move `size` bytes from one address to another, widest chunk first.
//
// A pair of doublewords moves sixteen bytes in two instructions where
// single registers would take four, so it takes as much of the block as it
// reaches: `paired` says the two ends are aligned enough for it, and
// InPairRange says the scaled immediate can name the displacement. What is
// left over goes a doubleword, a word, a halfword and a byte at a time,
// which is the same descent the x86-64 emitter makes.
void AArch64ModuleEmitter::CopyBlock(const A64Reg dst, const std::int64_t dstOff, const A64Reg src,
                                     const std::int64_t srcOff, const int size, const bool paired) {
    const A64Reg first = A64::Xn(kTemp);
    const A64Reg second = A64::Xn(kTemp2);
    std::int64_t offset = 0;
    while (paired && offset + 16 <= size && InPairRange(srcOff + offset) && InPairRange(dstOff + offset)) {
        Must(enc.Ldp(first, second, src, srcOff + offset), "a paired load");
        Must(enc.Stp(first, second, dst, dstOff + offset), "a paired store");
        offset += 16;
    }
    for (const int chunk : {8, 4, 2, 1}) {
        while (offset + chunk <= size) {
            LoadScalar(first, src, srcOff + offset, static_cast<unsigned>(chunk), false);
            StoreScalar(first, dst, dstOff + offset, static_cast<unsigned>(chunk));
            offset += chunk;
        }
    }
}

// Move one value between two places in the frame, at whatever width and in
// whichever register file its type asks for: an aggregate is a block copy,
// a float goes through a vector register because that is the only thing that
// holds one, and everything else is a load and a store.
void AArch64ModuleEmitter::CopyFrameValue(const std::int32_t dstOff, const std::int32_t srcOff, const TypeRef &type) {
    const int size = RuntimeSize(type);
    if (IsAggregate(type) && size > 8) {
        CopyBlock(A64::Fp, dstOff, A64::Fp, srcOff, size, RuntimeAlign(type) >= 8);
        return;
    }
    if (IsFloat(type)) {
        const A64Reg value = type.kind == TypeRef::Kind::Float32 ? A64::Sn(kFpTemp) : A64::Dn(kFpTemp);
        LoadScalar(value, A64::Fp, srcOff, value.bits / 8U, false);
        StoreScalar(value, A64::Fp, dstOff, value.bits / 8U);
        return;
    }
    const A64Reg value = A64::Xn(kTemp);
    const unsigned width = AccessWidth(size);
    LoadScalar(value, A64::Fp, srcOff, width, type.IsSigned());
    StoreScalar(value, A64::Fp, dstOff, width);
}

// Integer arithmetic
//
// Every one of these computes in a whole 64-bit register whatever width its
// type is, and the two ends of that are what make a narrow result behave
// the way the x86-64 back end's does. On the way in, LoadFromSlot extends
// by the type: a signed one sign-extends and an unsigned one zero-extends,
// which is what gives SDIV, UDIV and ASRV the narrow answers rather than
// answers about whatever the slot's upper bytes happened to hold. On the
// way out, StoreToSlot writes only the bytes the type occupies, so a
// `uint8` sum wraps because the byte above it is never written and no
// explicit truncation is emitted anywhere.

// The type a virtual register holds, for an operand whose width is not the
// instruction's own — a shift amount, or an index.
[[nodiscard]] TypeRef AArch64ModuleEmitter::TypeOfReg(const LirReg reg) const {
    const auto &registerTypes = FramePlan().RegisterTypes();
    const auto it = registerTypes.find(reg);
    return it == registerTypes.end() ? TypeRef::MakeInt64() : it->second;
}

// Instruction selection

// Bring a floating-point constant into `dst`. FMOV names 256 values
// outright, which covers most of what a program writes down; anything else
// — an exact fraction the encoding misses, or a value with more precision
// than it carries — is a word or a doubleword in the read-only pool,
// reached in two instructions rather than the four or five a MOVZ chain
// through a general-purpose register would take.
void AArch64ModuleEmitter::LoadFloatConstant(const A64Reg dst, const TypeRef &type, const std::string &literal) {
    const bool single = type.kind == TypeRef::Kind::Float32;
    const double value = single ? ParseFloatLiteral<float>(literal) : ParseFloatLiteral<double>(literal);
    if (TryEncodeFpImm8(value)) {
        Must(enc.FmovImm(dst, value), "a floating-point constant");
        return;
    }
    const std::uint32_t symIdx = single ? InternF32(literal) : InternF64(literal);
    A64SymbolRef ref{};
    Must(enc.LoadFromSymbol(dst, ref), "a floating-point constant");
    AddTextReloc(ref.adrp, symIdx, RcuRelType::AArch64AdrPrelPgHi21);
    AddTextReloc(ref.lo12, symIdx, RcuRelType::AArch64LdstAbsLo12Nc);
}
} // namespace Rux::AArch64Detail
